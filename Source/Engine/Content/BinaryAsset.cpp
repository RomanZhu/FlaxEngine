// Copyright (c) Wojciech Figat. All rights reserved.

#include "BinaryAsset.h"
#include "Storage/ContentStorageManager.h"
#include "Loading/Tasks/LoadAssetDataTask.h"
#include "Factories/BinaryAssetFactory.h"
#include "Artifacts/ResolvedArtifact.h"
#include "AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/Content.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Debug/Exceptions/JsonParseException.h"
#include "Engine/Threading/ThreadPoolTask.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Profiler/ProfilerMemory.h"
#if USE_EDITOR
#include "Engine/Platform/FileSystem.h"
#include "Engine/Engine/Globals.h"
#endif

REGISTER_BINARY_ASSET_ABSTRACT(BinaryAsset, "FlaxEngine.BinaryAsset");

BinaryAsset::BinaryAsset(const SpawnParams& params, const AssetInfo* info)
    : Asset(params, info)
    , _canonicalPath(info ? info->Path : String::Empty)
    , _storageRef(nullptr) // We link storage container later
    , _isSaving(false)
    , _isUsingExactArtifact(true)
    , _isGeneratedArtifact(false)
    , _artifactLoadDisposition(ArtifactLoadDisposition::Ready)
    , Storage(nullptr)
{
}

BinaryAsset::~BinaryAsset()
{
#if USE_EDITOR
    if (Storage)
        Storage->OnReloaded.Unbind<BinaryAsset, &BinaryAsset::OnStorageReloaded>(this);
#endif
}

bool BinaryAsset::Init(const FlaxStorageReference& storage, AssetHeader& header)
{
    // We allow to init asset only once like that
    ASSERT(Storage == nullptr && _header.ID.IsValid() == false);

    // Block initialization with a different storage
    bool isChanged = _storageRef != storage;
    if (Storage != nullptr && isChanged)
    {
        LOG(Error, "Asset \'{0}\' has been already initialized.", GetPath());
        return true;
    }

    // Get data
    _storageRef = storage;
    Storage = storage.Get();
    _header = header;

#if USE_EDITOR
    // Link for storage reload event
    if (Storage && isChanged)
        Storage->OnReloaded.Bind<BinaryAsset, &BinaryAsset::OnStorageReloaded>(this);
#endif

    return false;
}

bool BinaryAsset::Init(AssetInitData& initData)
{
    // Validate serialized version
    if (initData.SerializedVersion != GetSerializedVersion())
    {
        LOG(Error, "Asset \'{0}\' is using different serialized version. Loaded: {1}, Runtime: {2}.", GetPath(), initData.SerializedVersion, GetSerializedVersion());
        return true;
    }

    // Get asset data
    _header = initData.Header;
#if USE_EDITOR
    Metadata.Copy(initData.Metadata);
    ClearDependencies();
    Dependencies = initData.Dependencies;
    for (auto& e : Dependencies)
    {
        auto asset = Cast<BinaryAsset>(Content::GetRuntimeObject(e.First));
        if (asset)
        {
            asset->_dependantAssets.Add(this);
        }
        else
        {
            // Dependency is not yet loaded to keep track this link to act when it's loaded
            Content::onAssetDepend(this, e.First);
        }
    }
#endif

    return init(initData);
}

bool BinaryAsset::InitVirtual(AssetInitData& initData)
{
    // Be virtual
    _isVirtual = true;

    return Init(initData);
}

void BinaryAsset::SetResolvedArtifact(const ResolvedArtifact& artifact)
{
    ASSERT(Storage == nullptr);
    _artifactKey = artifact.Key;
    _isUsingExactArtifact = artifact.IsExact;
    _isGeneratedArtifact = artifact.IsGenerated();
    _artifactLoadDisposition = ArtifactLoadDisposition::Ready;
    _artifactLease = _isGeneratedArtifact ? ArtifactLease::Acquire(artifact.StoragePath.Get()) : ArtifactLease();
}

BinaryAssetStorageSwitchResult BinaryAsset::SwitchStorage(const ResolvedArtifact& artifact)
{
    if (!IsInMainThread())
    {
        LOG(Error, "Binary asset storage can only be switched from the main thread. Asset: '{0}'.", GetPath());
        return BinaryAssetStorageSwitchResult::InvalidThread;
    }
    if (artifact.ObjectID.Asset.Value != GetPersistentObjectId() || !artifact.AssetID.IsValid() || artifact.AssetID != GetID() ||
        artifact.TypeName != GetTypeName() || artifact.StoragePath.Get().IsEmpty())
        return BinaryAssetStorageSwitchResult::InvalidArtifact;

    const FlaxStorageReference newStorage = ContentStorageManager::GetStorage(artifact.StoragePath.Get(), false);
    if (!newStorage || (!newStorage->IsLoaded() && newStorage->Load()))
        return BinaryAssetStorageSwitchResult::InvalidArtifact;

    AssetInitData newData;
    const bool headerLoadFailed = newStorage->UsesAssetObjectIds()
        ? newStorage->LoadAssetHeader(_internalObjectId, newData)
        : newStorage->LoadAssetHeader(GetID(), newData);
    if (headerLoadFailed)
        return BinaryAssetStorageSwitchResult::InvalidArtifact;
    if (newData.Header.ID != GetID() || newData.Header.TypeName != GetTypeName())
        return BinaryAssetStorageSwitchResult::IdentityMismatch;
    if (newData.SerializedVersion != GetSerializedVersion())
        return BinaryAssetStorageSwitchResult::UnsupportedVersion;

    // Drain the current load graph before retaining and replacing its backing storage.
    WaitForLoaded();
    const FlaxStorageReference oldStorage = _storageRef;
    const AssetHeader oldHeader = _header;
    const String oldKey = _artifactKey;
    const bool oldExactness = _isUsingExactArtifact;
    const bool oldGenerated = _isGeneratedArtifact;
    const ArtifactLoadDisposition oldDisposition = _artifactLoadDisposition;
    const ArtifactLease oldLease = _artifactLease;
    const ArtifactLease newLease = artifact.IsGenerated() ? ArtifactLease::Acquire(artifact.StoragePath.Get()) : ArtifactLease();

    CancelStreaming();
    OnBeforeArtifactStorageChange();
    if (!IsInternalType())
    {
        Content::AssetArtifactReloading(this);
        Content::AssetReloading(this);
    }
    OnReloading(this);
    {
        ScopeLock lock(Locker);
        if (IsLoaded() || LastLoadFailed())
        {
            unload(true);
            Platform::AtomicStore(&_loadState, (int64)LoadState::Unloaded);
        }
    }
#if USE_EDITOR
    if (Storage)
        Storage->OnReloaded.Unbind<BinaryAsset, &BinaryAsset::OnStorageReloaded>(this);
#endif
    _storageRef = newStorage;
    Storage = newStorage.Get();
    _header = newData.Header;
    _artifactKey = artifact.Key;
    _isUsingExactArtifact = artifact.IsExact;
    _isGeneratedArtifact = artifact.IsGenerated();
    _artifactLoadDisposition = ArtifactLoadDisposition::Ready;
    _artifactLease = newLease;
#if USE_EDITOR
    Storage->OnReloaded.Bind<BinaryAsset, &BinaryAsset::OnStorageReloaded>(this);
#endif

    startLoading();
    if (!WaitForLoaded())
    {
        OnAfterArtifactStorageChange();
        return BinaryAssetStorageSwitchResult::Success;
    }

    // The replacement could be structurally valid but unusable by the concrete asset. Restore the old state.
    {
        ScopeLock lock(Locker);
        if (IsLoaded() || LastLoadFailed())
        {
            unload(true);
            Platform::AtomicStore(&_loadState, (int64)LoadState::Unloaded);
        }
    }
#if USE_EDITOR
    Storage->OnReloaded.Unbind<BinaryAsset, &BinaryAsset::OnStorageReloaded>(this);
#endif
    _storageRef = oldStorage;
    Storage = oldStorage.Get();
    _header = oldHeader;
    _artifactKey = oldKey;
    _isUsingExactArtifact = oldExactness;
    _isGeneratedArtifact = oldGenerated;
    _artifactLoadDisposition = oldDisposition;
    _artifactLease = oldLease;
#if USE_EDITOR
    Storage->OnReloaded.Bind<BinaryAsset, &BinaryAsset::OnStorageReloaded>(this);
#endif
    startLoading();
    return WaitForLoaded() ? BinaryAssetStorageSwitchResult::RollbackFailed : BinaryAssetStorageSwitchResult::LoadFailed;
}

#if USE_EDITOR

#if COMPILE_WITH_ASSETS_IMPORTER

void BinaryAsset::Reimport() const
{
    if (_isGeneratedArtifact)
    {
        LOG(Error, "Generated artifact storage cannot be reimported as an authoritative binary. Rebuild it from the canonical source instead.");
        return;
    }
    AssetPipelineService::RebuildAsset(GetID());
}

#endif

void BinaryAsset::GetImportMetadata(String& path, String& username) const
{
    if (Metadata.IsInvalid())
        return;

    // Parse metadata and try to get import info
    rapidjson_flax::Document document;
    document.Parse((const char*)Metadata.Get(), Metadata.Length());
    if (document.HasParseError() == false)
    {
        path = JsonTools::GetString(document, "ImportPath");
        username = JsonTools::GetString(document, "ImportUsername");
        if (path.HasChars() && FileSystem::IsRelative(path))
        {
            // Convert path back to thr absolute (eg. if stored in relative format)
            path = Globals::ProjectFolder / path;
            StringUtils::PathRemoveRelativeParts(path);
        }
    }
    else
    {
        Log::JsonParseException(document.GetParseError(), document.GetErrorOffset(), GetPath());
    }
}

String BinaryAsset::GetImportPath() const
{
    String path, username;
    GetImportMetadata(path, username);
    return path;
}

void BinaryAsset::ClearDependencies()
{
    for (auto& e : Dependencies)
    {
        auto asset = Cast<BinaryAsset>(Content::GetRuntimeObject(e.First));
        if (asset)
            asset->_dependantAssets.Remove(this);
    }
    Dependencies.Clear();
}

void BinaryAsset::AddDependency(BinaryAsset* asset)
{
    ASSERT_LOW_LAYER(asset);
    const Guid id = asset->GetID();
    for (auto& e : Dependencies)
    {
        if (e.First == id)
            return;
    }
    ASSERT(!asset->_dependantAssets.Contains(asset));
    Dependencies.Add(ToPair(id, FileSystem::GetFileLastEditTime(asset->GetPath())));
    asset->_dependantAssets.Add(this);
}

bool BinaryAsset::HasDependenciesModified() const
{
    AssetInfo info;
    for (const auto& e : Dependencies)
    {
        if (Content::GetRuntimeAssetInfo(e.First, info))
        {
            const auto editTime = FileSystem::GetFileLastEditTime(info.Path);
            if (editTime > e.Second)
            {
                LOG(Info, "Asset {0} was modified - dependency of {1}", info.Path, GetPath());
                return true;
            }
        }
    }
    return false;
}

#endif

FlaxChunk* BinaryAsset::GetOrCreateChunk(int32 index) const
{
    if (IsVirtual()) // Virtual assets don't own storage container
        return nullptr;
    ASSERT(Math::IsInRange(index, 0, ASSET_FILE_DATA_CHUNKS - 1));

    // Try get
    auto chunk = _header.Chunks[index];
    if (chunk)
    {
        chunk->RegisterUsage();
        return chunk;
    }

    // Allocate
    ASSERT(Storage);
    const_cast<BinaryAsset*>(this)->_header.Chunks[index] = chunk = Storage->AllocateChunk();
    if (chunk)
        chunk->RegisterUsage();

    return chunk;
}

void BinaryAsset::SetChunk(int32 index, const Span<byte>& data)
{
    auto chunk = GetOrCreateChunk(index);
    if (chunk)
        chunk->Data.Copy(data.Get(), data.Length());
}

void BinaryAsset::ReleaseChunks() const
{
    for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        ReleaseChunk(i);
}

void BinaryAsset::ReleaseChunk(int32 index) const
{
    auto chunk = GetChunk(index);
    if (chunk)
        chunk->Data.Release();
}

ContentLoadTask* BinaryAsset::RequestChunkDataAsync(int32 index)
{
    auto chunk = GetChunk(index);
    if (chunk != nullptr && chunk->IsLoaded())
    {
        // Data already here
        chunk->RegisterUsage();
        return nullptr;
    }

    // Spawn loading task
    return New<LoadAssetDataTask>(this, GET_CHUNK_FLAG(index));
}

void BinaryAsset::GetChunkData(int32 index, BytesContainer& data) const
{
    //ScopeLock lock(Locker);

    // Check if has data missing
    if (!HasChunkLoaded(index))
    {
        // Missing data
        data.Release();
        return;
    }

    // Get data
    auto chunk = GetChunk(index);
    data.Link(chunk->Data);
}

bool BinaryAsset::LoadChunk(int32 chunkIndex) const
{
    ASSERT(Storage);
    const auto chunk = _header.Chunks[chunkIndex];
    if (chunk != nullptr
        && chunk->IsMissing()
        && chunk->ExistsInFile())
    {
        if (Storage->LoadAssetChunk(chunk))
            return true;
    }
    return false;
}

bool BinaryAsset::LoadChunks(AssetChunksFlag chunks) const
{
    if (chunks == 0)
        return false;
    ASSERT(Storage);
    for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
    {
        auto chunk = _header.Chunks[i];
        if (chunk != nullptr
            && chunks & GET_CHUNK_FLAG(i)
            && chunk->IsMissing()
            && chunk->ExistsInFile())
        {
            if (Storage->LoadAssetChunk(chunk))
                return true;
        }
    }
    return false;
}

#if USE_EDITOR

bool BinaryAsset::SaveAsset(AssetInitData& data, bool silentMode) const
{
    if (_isGeneratedArtifact)
    {
        LOG(Error, "Generated artifact storage is immutable and cannot be saved in place.");
        return true;
    }
    return SaveAsset(GetStoragePath(), data, silentMode);
}

bool BinaryAsset::SaveAsset(const StringView& path, AssetInitData& data, bool silentMode) const
{
    data.Header = _header;
    data.Metadata.Link(Metadata);
    data.Dependencies = Dependencies;
    return SaveToAsset(path, data, silentMode);
}

bool BinaryAsset::SaveToAsset(const StringView& path, AssetInitData& data, bool silentMode)
{
    PROFILE_CPU();

    // Ensure path is in a valid format
    String pathNorm(path);
    ContentStorageManager::FormatPath(pathNorm);
    const StringView filePath = pathNorm;

    // Find target storage container and the asset
    auto storage = ContentStorageManager::TryGetStorage(filePath);
    auto asset = Content::GetAsset(filePath);
    auto binaryAsset = dynamic_cast<BinaryAsset*>(asset);
    if (asset && !binaryAsset)
    {
        LOG(Warning, "Cannot write to the non-binary asset location.");
        return true;
    }
    if (!binaryAsset && !storage && FileSystem::FileExists(filePath))
    {
        // Force-resolve storage (asset at that path could be not yet loaded into registry)
        storage = ContentStorageManager::GetStorage(filePath);
    }
    if (storage && storage->IsReadOnly())
    {
        LOG(Warning, "Cannot write to the asset storage container.");
        return true;
    }

    // Initialize data container
    ASSERT(data.SerializedVersion > 0);
    if (binaryAsset)
    {
        // Use the same asset ID
        data.Header.ID = binaryAsset->GetID();
    }
    else if (storage && storage->GetEntriesCount())
    {
        // Use the same file ID
        data.Header.ID = storage->GetEntry(0).ID;
    }
    else
    {
        // Randomize ID
        data.Header.ID = Guid::New();
    }

    // Save (set flag to lock reloads on storage modified)
    if (binaryAsset)
        binaryAsset->_isSaving = true;
    bool result;
    if (storage)
    {
        // HACK: file is locked by some tasks (e.g material asset loaded some data and is updating the asset)
        // Let's hide these locks just for the saving
        const auto locks = Platform::AtomicRead(&storage->_chunksLock);
        Platform::AtomicStore(&storage->_chunksLock, 0);
        result = storage->Save(data, silentMode);
        Platform::InterlockedAdd(&storage->_chunksLock, locks);
    }
    else
    {
        result = FlaxStorage::Create(filePath, data, silentMode);
    }
    if (binaryAsset)
        binaryAsset->_isSaving = false;

    if (binaryAsset)
    {
        // Inform dependant asset (use cloned version because it might be modified by assets when they got reloaded)
        auto dependantAssets = binaryAsset->_dependantAssets;
        for (auto& e : dependantAssets)
        {
            e->OnDependencyModified(binaryAsset);
        }
    }

    return result;
}

void BinaryAsset::OnStorageReloaded(FlaxStorage* storage, bool failed)
{
    ASSERT(Storage != nullptr && Storage == storage);

    // Clear header (prevent from using old chunks)
    auto oldHeader = _header;
    Platform::MemoryClear(_header.Chunks, sizeof(_header.Chunks));

    // Check if reload failed
    if (failed)
    {
        LOG(Error, "Asset storage reloading failed. Asset: \'{0}\'.", ToString());
        return;
    }

    // Gather updated asset init data
    AssetInitData initData;
    const bool headerLoadFailed = Storage->UsesAssetObjectIds()
        ? Storage->LoadAssetHeader(_internalObjectId, initData)
        : Storage->LoadAssetHeader(GetID(), initData);
    if (headerLoadFailed)
    {
        LOG(Error, "Asset header loading failed. Asset: \'{0}\'.", ToString());
        return;
    }
    if (oldHeader.ID != initData.Header.ID || oldHeader.TypeName != initData.Header.TypeName)
    {
        LOG(Warning, "Asset reloading data mismatch. Old ID:{0},TypeName:{1}, New ID:{2},TypeName:{3}. Asset: \'{4}\'.", oldHeader.ID, oldHeader.TypeName, initData.Header.ID, initData.Header.TypeName, GetPath());

        // Unload asset (file contains different asset data)
        // For eg. texture has been changed into sprite atlas on reimport
        Content::UnloadAsset(this);

        // Delete managed object now because it way fail when we recreate the asset object and want to register the new managed object (IDs will overlap)
        DeleteManaged();

        return;
    }

    // Reinitialize (file may modify some data so it needs to be flushed)
    if (Init(initData))
    {
        LOG(Error, "Asset reloading failed. Asset: \'{0}\'.", ToString());
    }

    // Don't reload on save
    if (_isSaving == false)
    {
        Reload();
    }

    // Inform dependant asset (use cloned version because it might be modified by assets when they got reloaded)
    auto dependantAssets = _dependantAssets;
    for (auto& e : dependantAssets)
    {
        e->OnDependencyModified(this);
    }
}

#endif

void BinaryAsset::OnDeleteObject()
{
#if USE_EDITOR
    ClearDependencies();
    _dependantAssets.Clear();
#endif
    _artifactLease.Reset();
    Asset::OnDeleteObject();
}

StringView BinaryAsset::GetPath() const
{
#if USE_EDITOR
    return _canonicalPath;
#else
    // In build all assets are packed into packages so use ID for original path lookup
    return Content::GetEditorAssetPath(GetPersistentObjectId());
#endif
}

StringView BinaryAsset::GetStoragePath() const
{
    return Storage ? StringView(Storage->GetPath()) : StringView::Empty;
}

uint64 BinaryAsset::GetMemoryUsage() const
{
    Locker.Lock();
    uint64 result = Asset::GetMemoryUsage();
    result += sizeof(BinaryAsset) - sizeof(Asset);
    result += _dependantAssets.Capacity() * sizeof(BinaryAsset*);
    for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
    {
        auto chunk = _header.Chunks[i];
        if (chunk != nullptr && chunk->IsLoaded())
            result += chunk->Size();
    }
    Locker.Unlock();
    return result;
}

/// <summary>
/// Helper task used to initialize binary asset and upgrade it if need to in background.
/// </summary>
/// <seealso cref="ContentLoadTask" />
class InitAssetTask : public ContentLoadTask
{
private:
    WeakAssetReference<BinaryAsset> _asset;
    FlaxStorage::LockData _dataLock;

public:
    /// <summary>
    /// Initializes a new instance of the <see cref="InitAssetTask"/> class.
    /// </summary>
    /// <param name="asset">The asset.</param>
    InitAssetTask(BinaryAsset* asset)
        : _asset(asset)
        , _dataLock(asset->Storage->Lock())
    {
    }

public:
    // [ContentLoadTask]
    bool HasReference(Object* obj) const override
    {
        return obj == _asset;
    }

protected:
    // [ContentLoadTask]
    Result run() override
    {
        AssetReference<BinaryAsset> ref = _asset.Get();
        if (ref == nullptr)
            return Result::MissingReferences;
        auto storage = ref->Storage;
        auto factory = (BinaryAssetFactoryBase*)Content::GetAssetFactory(ref->GetTypeName());
        ASSERT(factory);
        PROFILE_MEM(ContentAssets);

        // Here we should open storage and extract AssetInitData
        // This would also allow to convert/upgrade data
        if (!storage->IsLoaded() && storage->Load())
            return Result::AssetLoadError;
        if (factory->Init(ref.Get()))
            return Result::AssetLoadError;

        return Result::Ok;
    }

    void OnEnd() override
    {
        _dataLock.Release();
        _asset = nullptr;

        ContentLoadTask::OnEnd();
    }
};

ContentLoadTask* BinaryAsset::createLoadingTask()
{
    ContentLoadTask* loadTask = Asset::createLoadingTask();

    // Check if asset need any just to be preloaded
    auto chunksToPreload = getChunksToPreload();
    if (chunksToPreload != 0)
    {
        // Inject loading chunks task
        auto preLoadChunksTask = New<LoadAssetDataTask>(this, chunksToPreload);
        preLoadChunksTask->ContinueWith(loadTask);
        loadTask = preLoadChunksTask;
    }

    // Before asset loading we have to initialize storage and pull the asset header
    auto initTask = New<InitAssetTask>(this);
    initTask->ContinueWith(loadTask);
    loadTask = initTask;

    return loadTask;
}

Asset::LoadResult BinaryAsset::loadAsset()
{
    // Ensure that asset has been initialized
    ASSERT(Storage && _header.ID.IsValid() && _header.TypeName.HasChars());

    auto lock = Storage->Lock();
    auto chunksToPreload = getChunksToPreload();
    if (chunksToPreload != 0)
    {
        // Ensure that any chunks that were requested before are loaded in memory (in case streaming flushed them out after timeout)
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            const auto chunk = _header.Chunks[i];
            if (GET_CHUNK_FLAG(i) & chunksToPreload && chunk && chunk->IsMissing())
                Storage->LoadAssetChunk(chunk);
        }
    }
    const LoadResult result = load();
#if !BUILD_RELEASE
    if (result == LoadResult::MissingDataChunk)
    {
        // Provide more insights on potentially missing asset data chunk
        Char chunksBitMask[ASSET_FILE_DATA_CHUNKS + 1];
        Char chunksExistBitMask[ASSET_FILE_DATA_CHUNKS + 1];
        Char chunksLoadBitMask[ASSET_FILE_DATA_CHUNKS + 1];
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            if (const FlaxChunk* chunk = _header.Chunks[i])
            {
                chunksBitMask[i] = '1';
                chunksExistBitMask[i] = chunk->ExistsInFile() ? '1' : '0';
                chunksLoadBitMask[i] = chunk->IsLoaded() ? '1' : '0';
            }
            else
            {
                chunksBitMask[i] = chunksExistBitMask[i] = chunksLoadBitMask[i] = '0';
            }
        }
        chunksBitMask[ASSET_FILE_DATA_CHUNKS] = chunksExistBitMask[ASSET_FILE_DATA_CHUNKS] = chunksLoadBitMask[ASSET_FILE_DATA_CHUNKS] = 0;
        LOG(Warning, "Asset reports missing data chunk. Chunks bitmask: {}, existing chunks: {} loaded chunks: {}. '{}'", chunksBitMask, chunksExistBitMask, chunksLoadBitMask, ToString());
    }
#endif
    return result;
}

void BinaryAsset::releaseStorage()
{
#if USE_EDITOR
    // Close file
    if (Storage)
        Storage->CloseFileHandles();
#endif
}

#if USE_EDITOR

void BinaryAsset::onRename(const StringView& newPath)
{
    ScopeLock lock(Locker);

    if (!_isGeneratedArtifact)
    {
        // We don't support packages now
        ASSERT(!Storage->IsPackage() && !Storage->IsReadOnly() && Storage->GetEntriesCount() == 1);

        // Legacy canonical storage moves with the asset
        Storage->OnRename(newPath);
    }
    _canonicalPath = newPath;
}

#endif
