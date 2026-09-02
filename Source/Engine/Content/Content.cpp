// Copyright (c) Wojciech Figat. All rights reserved.

#include "Content.h"
#include "JsonAsset.h"
#include "SceneReference.h"
#include "AssetObjectRegistry.h"
#include "Builtin/BuiltinAssetCatalog.h"
#include "Storage/ContentStorageManager.h"
#include "Storage/JsonStorageProxy.h"
#include "Factories/IAssetFactory.h"
#include "Artifacts/ResolvedArtifact.h"
#include "Loading/LoadingThread.h"
#include "Loading/ContentLoadTask.h"
#include "Loading/AssetObjectLoader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/LogContext.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/ObjectsRemovalService.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Platform/Thread.h"
#include "Engine/Platform/CPUInfo.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Threading/MainThreadTask.h"
#include "Engine/Threading/ConcurrentTaskQueue.h"
#include "Engine/Graphics/Graphics.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Engine/Time.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/Types.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Profiler/ProfilerMemory.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/Internal/InternalCalls.h"
#include "Engine/Scripting/Scripting.h"
#if USE_EDITOR
#include "AssetDatabase/Identity/AssetIdentitySerialization.h"
#include "Artifacts/ArtifactResolver.h"
#include "AssetDatabase/AssetPath.h"
#include "AssetDatabase/AssetDatabase.h"
#include "AssetDatabase/AssetMeta.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#endif
#if USE_EDITOR && PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#include <propidlbase.h>
#endif

TimeSpan Content::AssetsUpdateInterval = TimeSpan::FromMilliseconds(500);
TimeSpan Content::AssetsUnloadInterval = TimeSpan::FromSeconds(10);
Delegate<Asset*> Content::AssetDisposing;
Delegate<Asset*> Content::AssetReloading;
Delegate<Asset*> Content::AssetArtifactReloading;

String AssetInfo::ToString() const
{
    return String::Format(TEXT("InstanceID: {0}, ObjectID: {1}, Revision: {2}, TypeName: {3}, Path: \'{4}\'"), ID, ObjectID.ToString(), Revision, TypeName, Path);
}

void FLAXENGINE_API Serialization::Serialize(ISerializable::SerializeStream& stream, const SceneReference& v, const void* otherObj)
{
    Serialize(stream, v.ID, otherObj);
}

void FLAXENGINE_API Serialization::Deserialize(ISerializable::DeserializeStream& stream, SceneReference& v, ISerializeModifier* modifier)
{
    Deserialize(stream, v.ID, modifier);
}

namespace
{
    // Assets
    CriticalSection AssetsLocker;
    Dictionary<Guid, Asset*> Assets;
    CriticalSection LoadedAssetsToInvokeLocker;
    Array<Asset*> LoadedAssetsToInvoke;
    Array<Asset*> ToUnload;

    // Editor-private transient objects or the immutable cooked runtime catalog.
    AssetObjectRegistry ObjectRegistry;

    class ContentAssetObjectFactory final : public IAssetObjectFactory
    {
    public:
        bool CreateObject(const AssetObjectLoadLocation& location, void*& instance,
            AssetPipelineDiagnostic& diagnostic) override
        {
            String storagePath = location.StorageName;
            if (location.StorageKind == AssetObjectStorageKind::RuntimePackage && FileSystem::IsRelative(storagePath))
                storagePath = Globals::ProjectContentFolder / storagePath;
            const String sourcePath = location.StorageKind == AssetObjectStorageKind::RuntimePackage || !location.SourceName.HasChars()
                ? storagePath
                : location.SourceName;
            Guid instanceId = location.InstanceID;
            if (location.StorageKind == AssetObjectStorageKind::RuntimePackage)
            {
                const FlaxStorageReference storage = ContentStorageManager::GetStorage(storagePath);
                AssetInitData header;
                if (!storage || !storage->UsesAssetObjectIds() || storage->LoadAssetHeader(location.StorageObject, header) ||
                    !header.Header.ID.IsValid() || header.Header.TypeName != String(location.TypeName))
                {
                    diagnostic = AssetPipelineDiagnostic();
                    diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
                    diagnostic.AssetGuid = location.Object;
                    diagnostic.SourcePath = storagePath;
                    diagnostic.Message = TEXT("Runtime package has no matching persistent object GUID entry.");
                    instance = nullptr;
                    return true;
                }
                instanceId = header.Header.ID;
            }
            if (!instanceId.IsValid())
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
                diagnostic.AssetGuid = location.Object;
                diagnostic.SourcePath = storagePath;
                diagnostic.Message = TEXT("Resolved object has no valid transient instance identifier.");
                instance = nullptr;
                return true;
            }
            AssetInfo info(instanceId, location.InstanceID, String(location.TypeName), sourcePath, location.Revision);
            AssetLoadLocation contentLocation;
            contentLocation.Info = info;
            contentLocation.Artifact.ObjectID = location.StorageObject;
            contentLocation.Artifact.AssetID = info.ID;
            contentLocation.Artifact.TypeName = info.TypeName;
            contentLocation.Artifact.StoragePath = ArtifactStoragePath(storagePath);
            contentLocation.Artifact.OutputKind = location.StorageKind == AssetObjectStorageKind::RuntimePackage
                ? TEXT("package")
                : TEXT("runtime");
            contentLocation.Artifact.Key = String(location.Artifact.ToString());
            contentLocation.Artifact.Content = location.Content;
            contentLocation.Artifact.Size = location.Size;
            contentLocation.Artifact.StorageKind = location.StorageKind == AssetObjectStorageKind::RuntimePackage
                ? ArtifactStorageKind::Package
                : ArtifactStorageKind::Generated;

            IAssetFactory* factory = Content::GetAssetFactory(info);
            Asset* asset = factory ? factory->New(contentLocation) : nullptr;
            if (!asset)
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = factory ? AssetPipelineDiagnosticCode::ArtifactInvalid : AssetPipelineDiagnosticCode::ProcessorMissing;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
                diagnostic.AssetGuid = location.Object;
                diagnostic.SourcePath = storagePath;
                diagnostic.Message = factory
                    ? TEXT("Asset factory could not materialize the resolved object artifact.")
                    : TEXT("No asset factory is registered for the resolved object type.");
                instance = nullptr;
                return true;
            }
            instance = asset;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        void DestroyObject(void* instance) override
        {
            static_cast<Asset*>(instance)->DeleteObject();
        }
    };

    LoadedAssetRegistry LoadedObjects;
    ContentAssetObjectFactory ObjectFactory;
#if USE_EDITOR
    EditorArtifactAssetObjectResolver ObjectResolver;
    AssetObjectLoader ObjectLoader(LoadedObjects, static_cast<IEditorAssetObjectResolver&>(ObjectResolver), ObjectFactory);
#else
    RuntimeCatalogAssetObjectResolver ObjectResolver(ObjectRegistry.GetRuntimeCatalog(), 1);
    AssetObjectLoader ObjectLoader(LoadedObjects, static_cast<IRuntimeAssetObjectResolver&>(ObjectResolver), ObjectFactory);
#endif

    // Loading assets
    THREADLOCAL LoadingThread* ThisLoadThread = nullptr;
    THREADLOCAL int32 PassiveLoadDepth = 0;
    LoadingThread* MainLoadThread = nullptr;
#if PLATFORM_THREADS_LIMIT > 1
    Array<LoadingThread*> LoadThreads;
    ConcurrentTaskQueue<ContentLoadTask> LoadTasks;
    ConditionVariable LoadTasksSignal;
    CriticalSection LoadTasksMutex;
    Array<Guid> LoadCallAssets;
#else
    Array<ContentLoadTask*> LoadTasks;
#endif

    // Unloading assets
    Dictionary<Asset*, TimeSpan> UnloadQueue;
    TimeSpan LastUnloadCheckTime(0);
    bool IsExiting = false;

#if USE_EDITOR
    bool MovePathWithRetry(const StringView& destination, const StringView& source)
    {
        // Case-only renames use an internal temporary path and must not be repeated if the
        // second leg fails. Ordinary moves can safely tolerate brief external file access.
        const int32 attempts = FileSystem::AreFilePathsEquivalent(destination, source) ? 1 : 20;
#if PLATFORM_WINDOWS
        uint32 firstError = 0;
        uint32 lastError = 0;
        const String destinationPath(destination);
        const String sourcePath(source);
#endif
        for (int32 attempt = 0; attempt < attempts; attempt++)
        {
#if PLATFORM_WINDOWS
            // Content mutations must be rename-only. MOVEFILE_COPY_ALLOWED can report failure
            // after copying and leave both paths behind if deleting the source is blocked.
            if (MoveFileExW(*sourcePath, *destinationPath, 0) != 0)
                return false;
            lastError = (uint32)GetLastError();
            if (attempt == 0)
                firstError = lastError;
            if (lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS)
                break;
#else
            if (!FileSystem::MoveFile(destination, source))
                return false;
#endif
            if (attempt + 1 < attempts)
                Platform::Sleep(50);
        }
#if PLATFORM_WINDOWS
        LOG(Warning, "Win32 failed to rename '{0}' to '{1}' (first error 0x{2:x}, final error 0x{3:x}).", source, destination, firstError, lastError);
#endif
        return true;
    }

    bool MoveAssetFileSafely(const StringView& destination, const StringView& source)
    {
#if PLATFORM_WINDOWS
        // A stale failed FileItem can synchronously recreate its zero-byte placeholder between
        // cleanup and rename. Replace only that verified-empty artifact atomically. All valid or
        // non-empty destinations have already been rejected by RenameAsset.
        if (FileSystem::FileExists(destination) && FileSystem::GetFileSize(destination) != 0)
            return true;
        const String destinationPath(destination);
        const String sourcePath(source);
        if (MoveFileExW(*sourcePath, *destinationPath, MOVEFILE_REPLACE_EXISTING) != 0)
            return false;
        LOG(Warning, "Win32 failed to atomically move '{0}' to '{1}' (error 0x{2:x}).", source, destination, (uint32)GetLastError());
        return true;
#else
        return MovePathWithRetry(destination, source);
#endif
    }

    bool MoveFolderPathSafely(const StringView& destination, const StringView& source)
    {
        if (FileSystem::AreFilePathsEquivalent(destination, source))
            return MovePathWithRetry(destination, source);

#if PLATFORM_WINDOWS
        // A stale content item can recreate a zero-byte file at the folder destination while
        // the move is being validated. MOVEFILE_REPLACE_EXISTING atomically replaces that
        // invalid file with the source directory, closing the delete-then-rename race. Never
        // replace a directory or a file containing data.
        const int32 attempts = 20;
        uint32 firstError = 0;
        uint32 lastError = 0;
        bool replacedInvalidFile = false;
        const String destinationPath(destination);
        const String sourcePath(source);
        for (int32 attempt = 0; attempt < attempts; attempt++)
        {
            const bool destinationIsFile = FileSystem::FileExists(destination);
            if (FileSystem::DirectoryExists(destination) || (destinationIsFile && FileSystem::GetFileSize(destination) != 0))
                return true;

            const DWORD flags = destinationIsFile ? MOVEFILE_REPLACE_EXISTING : 0;
            if (MoveFileExW(*sourcePath, *destinationPath, flags) != 0)
            {
                if (destinationIsFile)
                    LOG(Warning, "Replaced invalid zero-byte destination file '{0}' while moving folder.", destination);
                return false;
            }
            lastError = (uint32)GetLastError();
            if (attempt == 0)
                firstError = lastError;
            replacedInvalidFile |= destinationIsFile;
            if (attempt + 1 < attempts)
                Platform::Sleep(50);
        }
        LOG(Warning, "Win32 failed to move folder '{0}' to '{1}' (first error 0x{2:x}, final error 0x{3:x}, invalid destination observed: {4}).", source, destination, firstError, lastError, replacedInvalidFile);
        return true;
#else
        if (FileSystem::FileExists(destination))
        {
            if (FileSystem::GetFileSize(destination) != 0 || FileSystem::DeleteFile(destination))
                return true;
            LOG(Warning, "Removed invalid zero-byte destination file '{0}' while moving folder.", destination);
        }
        return MovePathWithRetry(destination, source);
#endif
    }

    enum class FlaxStorageFileState
    {
        Missing,
        Valid,
        Invalid,
        Inaccessible,
    };

    FlaxStorageFileState GetFlaxStorageFileState(const StringView& path)
    {
        if (!FileSystem::FileExists(path))
            return FlaxStorageFileState::Missing;

        File* file = File::Open(path, FileMode::OpenExisting, FileAccess::Read, FileShare::All);
        if (file == nullptr)
            return FileSystem::FileExists(path) ? FlaxStorageFileState::Inaccessible : FlaxStorageFileState::Missing;

        uint32 magicCode = 0;
        uint32 bytesRead = 0;
        const bool readFailed = file->Read(&magicCode, sizeof(magicCode), &bytesRead);
        Delete(file);
        return !readFailed && bytesRead == sizeof(magicCode) && magicCode == FlaxStorage::MagicCode
                   ? FlaxStorageFileState::Valid
                   : FlaxStorageFileState::Invalid;
    }

#endif

#if USE_EDITOR
    Dictionary<Guid, HashSet<BinaryAsset*>> PendingDependencies;

    bool IsSceneAssetPath(const StringView& path)
    {
        return FileSystem::GetExtension(path).ToLower() == TEXT("scene");
    }

    bool ReadJsonDocument(const StringView& path, rapidjson_flax::Document& document)
    {
        BytesContainer fileData;
        if (File::ReadAllBytes(path, fileData))
            return true;
        document.Parse(fileData.Get<char>(), fileData.Length());
        if (document.HasParseError())
        {
            LOG(Error, "Failed to parse json file '{0}' at offset {1}.", path, document.GetErrorOffset());
            return true;
        }
        return false;
    }

    bool IsExternalActorsSceneDocument(const rapidjson_flax::Document& document)
    {
        if (!document.IsObject())
            return false;
        const auto externalActors = document.FindMember("externalActors");
        return externalActors != document.MemberEnd() && externalActors->value.IsBool() && externalActors->value.GetBool();
    }

    bool TryGetSceneGuid(const StringView& scenePath, Guid& sceneGuid)
    {
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(String(scenePath) + TEXT(".meta"), meta, diagnostic) || !meta.ID.IsValid())
        {
            LOG(Error, "Cannot resolve scene identity for private fragments at '{0}': {1}", scenePath, diagnostic.Message);
            return true;
        }
        sceneGuid = meta.ID;
        return false;
    }

    void DeleteSceneFragments(const StringView& scenePath, const Guid* knownSceneGuid)
    {
        if (!IsSceneAssetPath(scenePath))
            return;
        Guid sceneGuid = knownSceneGuid ? *knownSceneGuid : Guid::Empty;
        if (!sceneGuid.IsValid() && TryGetSceneGuid(scenePath, sceneGuid))
            return;
        const String fragmentsPath = SceneFragmentStore::GetScenePath(sceneGuid);
        if (!FileSystem::DirectoryExists(fragmentsPath))
            return;
#if PLATFORM_WINDOWS || PLATFORM_LINUX
        if (FileSystem::MoveFileToRecycleBin(fragmentsPath))
            LOG(Warning, "Failed to move private scene fragments to Recycle Bin for scene '{0}'.", sceneGuid);
#else
        if (FileSystem::DeleteDirectory(fragmentsPath))
            LOG(Warning, "Failed to delete private scene fragments for scene '{0}'.", sceneGuid);
#endif
    }
#endif
}

class ContentService : public EngineService
{
public:
    ContentService()
        : EngineService(TEXT("Content"), -600)
    {
    }

    bool Init() override;
    void Update() override;
    void LateUpdate() override;
    void BeforeExit() override;
    void Dispose() override;
};

ContentService ContentServiceInstance;

bool ContentService::Init()
{
    PROFILE_MEM(Content);

    // Init memory containers
    Assets.EnsureCapacity(2048);
    LoadedAssetsToInvoke.EnsureCapacity(64);
#if PLATFORM_THREADS_LIMIT > 1
    LoadCallAssets.EnsureCapacity(PLATFORM_THREADS_LIMIT);
#endif

    // Load the immutable runtime object catalog (editor registry stays transient and empty).
    ObjectRegistry.Init();
#if USE_EDITOR
    AssetPipelineDiagnostic builtinDiagnostic;
    if (BuiltinAssetCatalog::Get().Initialize(builtinDiagnostic))
    {
        LOG(Fatal, "Cannot initialize built-in asset catalog. {0}: {1} Path: '{2}'.",
            GetAssetPipelineDiagnosticCodeName(builtinDiagnostic.Code), builtinDiagnostic.Message, builtinDiagnostic.SourcePath);
        return true;
    }
    Array<Guid> builtinRuntimeIds;
    BuiltinAssetCatalog::Get().GetAll(builtinRuntimeIds);
    for (const Guid& builtinRuntimeId : builtinRuntimeIds)
        ObjectRegistry.RemoveTransientRuntimeObject(builtinRuntimeId, nullptr);
    LOG(Info, "Built-in asset catalog loaded {0} immutable objects from {1} prebuilt roots ({2} generated).",
        BuiltinAssetCatalog::Get().Count(), BuiltinAssetCatalog::Get().PrebuiltRootsCount(), BuiltinAssetCatalog::Get().GeneratedRootsCount());
#endif

    // Create loading threads
    MainLoadThread = New<LoadingThread>();
    ThisLoadThread = MainLoadThread;
#if PLATFORM_THREADS_LIMIT > 1
    const CPUInfo cpuInfo = Platform::GetCPUInfo();
    const int32 count = Math::Clamp(Math::CeilToInt(LOADING_THREAD_PER_LOGICAL_CORE * (float)cpuInfo.LogicalProcessorCount), 1, 12);
    LOG(Info, "Creating {0} content loading threads...", count);
    LoadThreads.Resize(count);
    for (int32 i = 0; i < count; i++)
    {
        auto thread = New<LoadingThread>();
        LoadThreads[i] = thread;
        if (thread->Start(String::Format(TEXT("Load Thread {0}"), i)))
        {
            LOG(Fatal, "Cannot spawn content thread {0}/{1}", i, count);
            Delete(thread);
            return true;
        }
    }
#endif

    return false;
}

void ContentService::Update()
{
    PROFILE_CPU();

#if PLATFORM_THREADS_LIMIT == 1
    // Run content-streaming tasks on a main thread
    if (LoadTasks.HasItems())
    {
        double timeLimit = 0.01; // 10ms
        double startTime = Platform::GetTimeSeconds();
        do
        {
            auto task = LoadTasks[0];
            LoadTasks.RemoveAt(0);
            MainLoadThread->Run(task);
        } while (LoadTasks.HasItems() && Platform::GetTimeSeconds() - startTime < timeLimit);
    }
#endif

    // Broadcast `OnLoaded` events
    LoadedAssetsToInvokeLocker.Lock();
    while (LoadedAssetsToInvoke.HasItems())
    {
        auto asset = LoadedAssetsToInvoke.Dequeue();
        asset->onLoaded_MainThread();
#if USE_EDITOR
        Content::onAddDependencies(asset);
#endif
    }
    LoadedAssetsToInvokeLocker.Unlock();
}

void ContentService::LateUpdate()
{
    PROFILE_CPU();
    PROFILE_MEM(Content);

    // Check if need to perform an update of unloading assets
    const TimeSpan timeNow = Time::Update.UnscaledTime;
    if (timeNow - LastUnloadCheckTime < Content::AssetsUpdateInterval)
        return;
    LastUnloadCheckTime = timeNow;
    AssetsLocker.Lock();

    // Verify all assets
    for (auto i = Assets.Begin(); i.IsNotEnd(); ++i)
    {
        Asset* asset = i->Value;

        // Check if has no references and is not during unloading
        if (asset->GetReferencesCount() <= 0 && !UnloadQueue.ContainsKey(asset))
        {
            // Add to removes
            UnloadQueue.Add(asset, timeNow);
        }
    }

    // Find assets to unload in unload queue
    ToUnload.Clear();
    for (auto i = UnloadQueue.Begin(); i != UnloadQueue.End(); ++i)
    {
        // Check if asset gain any new reference or if need to unload it
        if (i->Key->GetReferencesCount() > 0 || timeNow - i->Value >= Content::AssetsUnloadInterval)
        {
            ToUnload.Add(i->Key);
        }
    }

    // Unload marked assets
    for (int32 i = 0; i < ToUnload.Count(); i++)
    {
        Asset* asset = ToUnload[i];

        // Check if has no references
        if (asset->GetReferencesCount() <= 0)
        {
            Content::UnloadAsset(asset);
        }

        // Remove from unload queue
        UnloadQueue.Remove(asset);
    }

    AssetsLocker.Unlock();

}

void ContentService::BeforeExit()
{
#if PLATFORM_THREADS_LIMIT > 1
    // Signal threads to end work soon
    for (auto thread : LoadThreads)
        thread->NotifyExit();
    LoadTasksSignal.NotifyAll();
#endif
}

void ContentService::Dispose()
{
    IsExiting = true;

    // Flush objects (some asset-related objects/references may be pending to delete)
    ObjectsRemovalService::Flush();

    // Unload all remaining assets
    {
        ScopeLock lock(AssetsLocker);

        for (auto i = Assets.Begin(); i.IsNotEnd(); ++i)
        {
            i->Value->DeleteObject();
        }
    }

    // Flush objects (some assets may be pending to delete)
    ObjectsRemovalService::Flush();

    // NOW dispose graphics device - where there is no loaded assets at all
    Graphics::DisposeDevice();

#if PLATFORM_THREADS_LIMIT > 1
    // Exit all load threads
    for (auto thread : LoadThreads)
        thread->NotifyExit();
    LoadTasksSignal.NotifyAll();
    for (auto thread : LoadThreads)
        thread->Join();
    LoadThreads.ClearDelete();
#endif
    Delete(MainLoadThread);
    MainLoadThread = nullptr;
    ThisLoadThread = nullptr;

#if PLATFORM_THREADS_LIMIT > 1
    // Cancel all remaining tasks (no chance to execute them)
    LoadTasks.CancelAll();
#else
    for (auto* e : LoadTasks)
        e->Cancel();
    LoadTasks.Clear();
    LoadTasks.SetCapacity(0);
#endif

#if USE_EDITOR
    BuiltinAssetCatalog::Get().Dispose();
#endif
}

IAssetFactory::Collection& IAssetFactory::Get()
{
    static Collection Factories(1024);
    return Factories;
}

LoadingThread::LoadingThread()
    : _exitFlag(false)
    , _thread(nullptr)
    , _totalTasksDoneCount(0)
{
}

LoadingThread::~LoadingThread()
{
    if (_thread != nullptr)
    {
        _thread->Kill(true);
        Delete(_thread);
    }
}

void LoadingThread::NotifyExit()
{
    Platform::InterlockedIncrement(&_exitFlag);
}

void LoadingThread::Join()
{
    auto thread = _thread;
    if (thread)
        thread->Join();
}

bool LoadingThread::Start(const String& name)
{
    ASSERT(_thread == nullptr && name.HasChars());

    // Create new thread
    auto thread = Thread::Create(this, name, ThreadPriority::Normal);
    if (thread == nullptr)
        return true;

    _thread = thread;

    return false;
}

void LoadingThread::Run(ContentLoadTask* job)
{
    ASSERT(job);

    job->Execute();
    _totalTasksDoneCount++;
}

String LoadingThread::ToString() const
{
    return String::Format(TEXT("Loading Thread {0}"), _thread ? _thread->GetID() : 0);
}

int32 LoadingThread::Run()
{
#if PLATFORM_THREADS_LIMIT > 1
    PROFILE_MEM(Content);
#if USE_EDITOR && PLATFORM_WINDOWS
    // Initialize COM
    // TODO: maybe add sth to Thread::Create to indicate that thread will use COM stuff
    const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(result))
    {
        LOG(Error, "Failed to init COM for WIC texture importing! Result: {0:x}", static_cast<uint32>(result));
        return -1;
    }
#endif
#ifdef LOADING_THREAD_AFFINITY_MASK
    Platform::SetThreadAffinityMask(LOADING_THREAD_AFFINITY_MASK(LoadThreads.Find(this)));
#endif

    ContentLoadTask* task;
    ThisLoadThread = this;

    MONO_THREAD_INFO_TYPE* monoThreadInfo = nullptr;
    while (Platform::AtomicRead(&_exitFlag) == 0)
    {
        if (LoadTasks.try_dequeue(task))
        {
            Run(task);
            MONO_THREAD_INFO_GET(monoThreadInfo);
        }
        else
        {
            MONO_ENTER_GC_SAFE_WITH_INFO(monoThreadInfo);
            LoadTasksMutex.Lock();
            LoadTasksSignal.Wait(LoadTasksMutex);
            LoadTasksMutex.Unlock();
            MONO_EXIT_GC_SAFE_WITH_INFO;
        }
    }

    ThisLoadThread = nullptr;
#endif
    return 0;
}

void LoadingThread::Exit()
{
    LOG(Info, "Content thread '{0}' exited. Load calls: {1}", _thread->GetName(), _totalTasksDoneCount);
}

String ContentLoadTask::ToString() const
{
    return String::Format(TEXT("Content Load Task ({})"), (int32)GetState());
}

void ContentLoadTask::Enqueue()
{
    LoadTasks.Add(this);
#if PLATFORM_THREADS_LIMIT > 1
    LoadTasksSignal.NotifyOne();
#endif
}

bool ContentLoadTask::Run()
{
    const auto result = run();
    const bool failed = result != Result::Ok;
    if (failed)
    {
        LOG(Warning, "\'{0}\' failed with result: {1}", ToString(), ToString(result));
    }
    return failed;
}

AssetObjectRegistry* Content::GetObjectRegistry()
{
    return &ObjectRegistry;
}

Guid Content::GetRuntimeGameSettingsObject()
{
    return ObjectRegistry.GetGameSettingsObject();
}

bool Content::GetRuntimeAssetInfo(const Guid& runtimeId, AssetInfo& info)
{
    if (!runtimeId.IsValid())
        return false;

#if USE_EDITOR
    {
        AssetRecord record;
        if (AssetDatabase::Get().TryGetRecord(runtimeId, record))
        {
            if (record.Status == AssetRecordStatus::MissingSource)
                return false;
            AssetInfo builtinInfo;
            if (BuiltinAssetCatalog::Get().TryGet(runtimeId, builtinInfo))
            {
                LOG(Error, "Project asset identity {0} collides with read-only built-in '{1}'.", runtimeId, builtinInfo.Path);
                return false;
            }
            info = record.ToAssetInfo();
            return true;
        }
    }
    if (BuiltinAssetCatalog::Get().TryGet(runtimeId, info))
        return true;

    return false;
#else
    return ObjectRegistry.FindRuntimeObject(runtimeId, info);
#endif
}

bool Content::GetAssetInfo(const Guid& id, AssetInfo& info)
{
    return GetRuntimeAssetInfo(id, info);
}

bool Content::GetAssetInfo(const StringView& path, AssetInfo& info)
{
#if USE_EDITOR
    if (BuiltinAssetCatalog::Get().TryGetByPath(path, info))
        return true;

    String formattedPath(path);
    FileSystem::NormalizePath(formattedPath);

    {
        AssetPathPolicy::ProjectPath projectPath;
        AssetPipelineDiagnostic diagnostic;
        AssetRecord record;
        if (!AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
                formattedPath, projectPath, diagnostic) &&
            AssetDatabase::Get().TryGetMainRecordByPath(projectPath.PortabilityKey, record))
        {
            info = record.ToAssetInfo();
            return true;
        }
    }
    if (AssetPathPolicy::IsSameOrChild(formattedPath, Globals::ProjectCacheFolder))
        return ObjectRegistry.FindObject(formattedPath, info);
    return false;
#else
    return ObjectRegistry.FindObject(path, info);
#endif
}

StringView Content::GetEditorAssetPath(const Guid& objectId)
{
#if USE_EDITOR
    const StringView builtinPath = BuiltinAssetCatalog::Get().GetStoragePath(objectId);
    if (builtinPath.HasChars())
        return builtinPath;
#endif
    return ObjectRegistry.GetEditorObjectPath(objectId);
}

Array<Guid> Content::GetAllAssets()
{
    Array<Guid> result;
#if USE_EDITOR
    BuiltinAssetCatalog::Get().GetAll(result);
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.Status != AssetRecordStatus::MissingSource && !result.Contains(record.ID))
            result.Add(record.ID);
    }
#else
    ObjectRegistry.GetAllRuntimeIds(result);
#endif
    return result;
}

Array<Guid> Content::GetAllAssetsByType(const MClass* type)
{
    Array<Guid> result;
    CHECK_RETURN(type, result);
    const String typeName(type->GetFullName());
#if USE_EDITOR
    BuiltinAssetCatalog::Get().GetAllByTypeName(typeName, result);
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.TypeName == typeName && record.Status != AssetRecordStatus::MissingSource && !result.Contains(record.ID))
            result.Add(record.ID);
    }
#else
    ObjectRegistry.GetAllRuntimeIdsByTypeName(typeName, result);
#endif
    return result;
}

IAssetFactory* Content::GetAssetFactory(const StringView& typeName)
{
    IAssetFactory* result = nullptr;
    IAssetFactory::Get().TryGet(typeName, result);
    return result;
}

IAssetFactory* Content::GetAssetFactory(const AssetInfo& assetInfo)
{
    IAssetFactory* result = nullptr;
    if (!IAssetFactory::Get().TryGet(assetInfo.TypeName, result))
    {
        // Check if it's a json asset (in editor only)
        // In build game all asset factories are valid and json assets are cooked into binary packages
#if USE_EDITOR
        if (JsonStorageProxy::IsValidExtension(FileSystem::GetExtension(assetInfo.Path).ToLower()))
#endif
        {
            IAssetFactory::Get().TryGet(JsonAsset::TypeName, result);
        }
    }

    return result;
}

String Content::CreateTemporaryAssetPath()
{
    return Globals::TemporaryFolder / (Guid::New().ToString(Guid::FormatType::N) + ASSET_FILES_EXTENSION_WITH_DOT);
}

ContentStats Content::GetStats()
{
    ContentStats stats;
    AssetsLocker.Lock();
    stats.AssetsCount = Assets.Count();
    int32 loadFailedCount = 0;
    for (const auto& e : Assets)
    {
        if (e.Value->IsLoaded())
            stats.LoadedAssetsCount++;
        else if (e.Value->LastLoadFailed())
            loadFailedCount++;
        if (e.Value->IsVirtual())
            stats.VirtualAssetsCount++;
    }
    stats.LoadingAssetsCount = stats.AssetsCount - loadFailedCount - stats.LoadedAssetsCount;
    AssetsLocker.Unlock();
    return stats;
}

Asset* Content::LoadAsyncInternal(const StringView& internalPath, const MClass* type)
{
    CHECK_RETURN(type, nullptr);
    const auto scriptingType = Scripting::FindScriptingType(type->GetFullName());
    if (scriptingType)
        return LoadAsyncInternal(internalPath, scriptingType);
    LOG(Error, "Failed to find asset type '{0}'.", String(type->GetFullName()));
    return nullptr;
}

Asset* Content::LoadAsyncInternal(const StringView& internalPath, const ScriptingTypeHandle& type)
{
    PROFILE_MEM(Content);
#if USE_EDITOR
    const String path = Globals::EngineContentFolder / internalPath + ASSET_FILES_EXTENSION_WITH_DOT;
    if (!FileSystem::FileExists(path))
    {
        LOG(Error, "Missing file \'{0}\'", path);
        return nullptr;
    }
#else
    const String path = Globals::ProjectContentFolder / internalPath + ASSET_FILES_EXTENSION_WITH_DOT;
#endif

    const auto asset = LoadAsync(path, type);
    if (asset == nullptr)
    {
        LOG(Error, "Failed to load \'{0}\' (type: {1})", internalPath, type.ToString());
    }

    return asset;
}

Asset* Content::LoadAsyncInternal(const Char* internalPath, const ScriptingTypeHandle& type)
{
    return LoadAsyncInternal(StringView(internalPath), type);
}

FLAXENGINE_API Asset* LoadAsset(const Guid& id, const ScriptingTypeHandle& type)
{
    return Content::LoadAssetAsync(id, type);
}

Asset* Content::LoadAssetAsync(const Guid& objectId, const MClass* type)
{
    CHECK_RETURN(type, nullptr);
    const auto scriptingType = Scripting::FindScriptingType(type->GetFullName());
    return scriptingType ? LoadAssetAsync(objectId, scriptingType) : nullptr;
}

Asset* Content::LoadAssetAsync(const Guid& objectId, const ScriptingTypeHandle& type)
{
    return LoadAssetObjectAsyncInternal(objectId, type);
}

Asset* Content::LoadMainAssetAsync(const AssetGuid& asset, const MClass* type)
{
    return LoadAssetAsync(asset.Value, type);
}

Asset* Content::LoadMainAssetAsync(const AssetGuid& asset, const ScriptingTypeHandle& type)
{
    return LoadAssetAsync(asset.Value, type);
}

Asset* Content::LoadAsync(const StringView& path, const MClass* type)
{
    CHECK_RETURN(type, nullptr);
    const auto scriptingType = Scripting::FindScriptingType(type->GetFullName());
    if (scriptingType)
        return LoadAsync(path, scriptingType);
    LOG(Error, "Failed to find asset type '{0}'.", String(type->GetFullName()));
    return nullptr;
}

Asset* Content::LoadAsync(const StringView& path, const ScriptingTypeHandle& type)
{
    PROFILE_MEM(Content);

#if USE_EDITOR
    AssetInfo builtinInfo;
    if (BuiltinAssetCatalog::Get().TryGetByPath(path, builtinInfo))
        return LoadAssetAsync(builtinInfo.ObjectID, type);
#endif

    // Ensure path is in a valid format
    String pathNorm(path);
    ContentStorageManager::FormatPath(pathNorm);
    const StringView filePath = pathNorm;

    AssetInfo assetInfo;
    if (GetAssetInfo(filePath, assetInfo))
    {
        return LoadAssetAsync(assetInfo.ObjectID.IsValid() ? assetInfo.ObjectID : assetInfo.ID, type);
    }

    return nullptr;
}

Array<Asset*> Content::GetAssets()
{
    Array<Asset*> assets;
    AssetsLocker.Lock();
    Assets.GetValues(assets);
    AssetsLocker.Unlock();
    return assets;
}

Array<Asset*> Content::GetAssets(const MClass* type)
{
    Array<Asset*> assets;
    AssetsLocker.Lock();
    for (auto& e : Assets)
    {
        if (e.Value->Is(type))
            assets.Add(e.Value);
    }
    AssetsLocker.Unlock();
    return assets;
}

const Dictionary<Guid, Asset*>& Content::GetAssetsRaw()
{
    AssetsLocker.Lock();
    AssetsLocker.Unlock();
    return Assets;
}

Asset* Content::GetAsset(const StringView& outputPath)
{
    if (outputPath.IsEmpty())
        return nullptr;
    PROFILE_CPU();
#if USE_EDITOR
    AssetInfo builtinInfo;
    if (BuiltinAssetCatalog::Get().TryGetByPath(outputPath, builtinInfo))
        return GetAsset(builtinInfo.ObjectID);
#endif
    String formattedPath(outputPath);
    FileSystem::NormalizePath(formattedPath);
    ScopeLock lock(AssetsLocker);
    for (auto i = Assets.Begin(); i.IsNotEnd(); ++i)
    {
        if (i->Value->GetPath() == formattedPath)
        {
            return i->Value;
        }
    }
    return nullptr;
}

Asset* Content::GetRuntimeObject(const Guid& runtimeId)
{
    return GetAsset(runtimeId);
}

Asset* Content::GetAsset(const Guid& objectId)
{
    if (!objectId.IsValid())
        return nullptr;
    ScopeLock lock(AssetsLocker);
    Asset* result = nullptr;
    Assets.TryGet(objectId, result);
    return result;
}

void Content::DeleteAsset(Asset* asset)
{
    if (asset == nullptr || asset->_deleteFileOnUnload)
        return;
#if USE_EDITOR
    if (BuiltinAssetCatalog::Get().GetUri(asset->GetPersistentObjectId()).HasChars())
    {
        LOG(Error, "Cannot delete read-only built-in asset {0}.", asset->GetPersistentObjectId());
        return;
    }
#endif

    LOG(Info, "Deleting asset {0}...", asset->ToString());

    // Ensure that asset is loaded (easier than cancel in-flight loading)
    asset->WaitForLoaded();

    // Mark asset for delete queue (delete it after auto unload)
    asset->_deleteFileOnUnload = true;

    // Unload
    asset->DeleteObject();
}

void Content::DeleteScript(const StringView& path)
{
    PROFILE_CPU();
    if (path.IsEmpty())
        return;
    
    // Return if asset
    Asset* asset = GetAsset(path);
    if (asset != nullptr)
    {
        return;
    }
    
#if USE_EDITOR
    LOG(Info, "Deleting script '{0}'", path);

    // Delete file
    deleteFileSafety(path);
#endif
}

void Content::DeleteAsset(const StringView& path)
{
    PROFILE_CPU();

#if USE_EDITOR
    AssetInfo builtinInfo;
    if (BuiltinAssetCatalog::Get().TryGetByPath(path, builtinInfo))
    {
        LOG(Error, "Cannot delete read-only built-in asset '{0}'.", path);
        return;
    }
#endif

    // Try to delete already loaded asset
    Asset* asset = GetAsset(path);
    if (asset != nullptr)
    {
        DeleteAsset(asset);
        return;
    }

#if USE_EDITOR
    ScopeLock locker(AssetsLocker);

    // Remove from registry
    AssetInfo info;
    if (ObjectRegistry.RemoveTransientPackage(path, &info))
    {
        LOG(Info, "Deleting asset '{0}':{1}({2})", path, info.ID, info.TypeName);
    }
    else
    {
        LOG(Info, "Deleting asset '{0}':{1}({2})", path, TEXT("?"), TEXT("?"));
        info.ID = Guid::Empty;
    }

    // Delete file
    deleteFileSafety(path, &info.ID);
#endif
}

void Content::deleteFileSafety(const StringView& path, const Guid* id)
{
    if (id && !id->IsValid())
    {
        LOG(Warning, "Cannot remove file \'{0}\'. Given ID is invalid.", path);
        return;
    }
    PROFILE_CPU();

    // Ensure that file has the same ID (prevent from deleting different assets)
    auto storage = ContentStorageManager::TryGetStorage(path);
    if (storage && id)
    {
        storage->CloseFileHandles(); // Close file handle to allow removing it
        if (!storage->HasAsset(*id))
        {
            LOG(Warning, "Cannot remove file \'{0}\'. It doesn\'t contain asset {1}.", path, *id);
            return;
        }
    }

    bool removeFileFailed;
#if PLATFORM_WINDOWS || PLATFORM_LINUX
    // Safety way - move file to the recycle bin
    removeFileFailed = FileSystem::MoveFileToRecycleBin(path);
    if (removeFileFailed)
    {
        LOG(Warning, "Failed to move file to Recycle Bin. Path: \'{0}\'", path);
    }
#else
    // Remove file
    removeFileFailed = FileSystem::DeleteFile(path);
    if (removeFileFailed)
    {
        LOG(Warning, "Failed to delete file Path: \'{0}\'", path);
    }
#endif

#if USE_EDITOR
    if (!removeFileFailed)
        DeleteSceneFragments(path, id);
#endif
}

#if !COMPILE_WITHOUT_CSHARP

#include "Engine/Scripting/ManagedCLR/MUtils.h"

void* Content::GetAssetsInternal()
{
    AssetsLocker.Lock();
    MArray* result = MCore::Array::New(Asset::TypeInitializer.GetClass(), Assets.Count());
    int32 i = 0;
    for (const auto& e : Assets)
        MCore::GC::WriteArrayRef(result, e.Value->GetOrCreateManagedInstance(), i++);
    AssetsLocker.Unlock();
    return result;
}

#endif

#if USE_EDITOR

bool Content::RenameAsset(const StringView& oldPathInput, const StringView& newPathInput)
{
    ASSERT(IsInMainThread());

    String oldPath(oldPathInput);
    String newPath(newPathInput);
    ContentStorageManager::FormatPath(oldPath);
    ContentStorageManager::FormatPath(newPath);

    if (oldPath == newPath)
        return false;

    AssetInfo builtinInfo;
    if (BuiltinAssetCatalog::Get().TryGetByPath(oldPath, builtinInfo))
    {
        LOG(Error, "Cannot move read-only built-in asset '{0}'.", oldPath);
        return true;
    }

    // Cache data
    Asset* oldAsset = GetAsset(oldPath);
    Asset* newAsset = GetAsset(newPath);

    // Validate the filesystem destination. A failed move from an older Editor can leave a
    // transient, zero-byte asset file behind. Remove only that provably empty artifact; invalid
    // files containing any data are preserved and reported as collisions.
    const bool samePath = FileSystem::AreFilePathsEquivalent(oldPath, newPath);
    FlaxStorageReference lockedDestinationStorage;
    if (!samePath && ContentStorageManager::LockFileAccess(newPath, lockedDestinationStorage))
    {
        LOG(Error, "Cannot release destination asset '{0}' for move.", newPath);
        return true;
    }
    SCOPE_EXIT
    {
        if (lockedDestinationStorage)
            lockedDestinationStorage->UnlockFileAccess();
    };
    FlaxStorageFileState destinationState = samePath ? FlaxStorageFileState::Missing : GetFlaxStorageFileState(newPath);
    if (!samePath && destinationState == FlaxStorageFileState::Invalid)
    {
        // Json assets such as scenes and prefabs don't use the FlaxStorage magic code. The
        // recovery only needs to prove that deleting the empty destination cannot discard the
        // sole copy of the source data.
        if (!FileSystem::FileExists(oldPath) || FileSystem::GetFileSize(oldPath) == 0)
        {
            LOG(Error, "Cannot recover invalid destination '{0}' because source asset '{1}' is missing or empty.", newPath, oldPath);
            return true;
        }

        if (FileSystem::GetFileSize(newPath) != 0)
        {
            LOG(Error, "Cannot replace invalid non-empty destination asset '{0}'. Move it aside manually to preserve its data.", newPath);
            return true;
        }

        // Dispose the stale failed asset before removing its placeholder. In particular, this
        // prevents an editor properties proxy from saving it back while the move is in progress.
        if (newAsset != nullptr && newAsset != oldAsset && newAsset->LastLoadFailed())
        {
            ObjectRegistry.RemoveTransientPackage(newPath, nullptr);
            UnloadAsset(newAsset);
            newAsset = nullptr;
        }

        const bool cleanupFailed = FileSystem::DeleteFile(newPath);
        destinationState = GetFlaxStorageFileState(newPath);
        if (cleanupFailed && destinationState != FlaxStorageFileState::Missing)
        {
            LOG(Error, "Cannot remove invalid zero-byte destination asset '{0}'.", newPath);
            return true;
        }
        LOG(Warning, "Removed invalid zero-byte destination asset '{0}' before move.", newPath);
        destinationState = FlaxStorageFileState::Missing;
    }
    if (!samePath && (destinationState != FlaxStorageFileState::Missing || FileSystem::DirectoryExists(newPath)))
    {
        LOG(Error, "Cannot move asset '{0}' to '{1}' because the destination already exists.", oldPath, newPath);
        return true;
    }

    // Validate name. Ignore a stale failed object after its invalid backing file was removed; it
    // will be unloaded by the normal missing-file update and must not block the filesystem move.
    if (newAsset != nullptr && newAsset != oldAsset)
    {
        if (!newAsset->LastLoadFailed())
        {
            LOG(Error, "Invalid name '{0}' when trying to rename '{1}'.", newPath, oldPath);
            return true;
        }
        ObjectRegistry.RemoveTransientPackage(newPath, nullptr);
        UnloadAsset(newAsset);
        newAsset = nullptr;
    }

    // Ensure asset is ready for renaming
    if (oldAsset)
    {
        // Failed assets still own a valid path/cache identity, but waiting for them to load can
        // never succeed. Release any storage handle and move their raw bytes transactionally.
        // Valid assets retain the existing wait-before-release behavior.
        if (!oldAsset->LastLoadFailed() && oldAsset->WaitForLoaded())
        {
            LOG(Error, "Failed to load asset '{0}'.", oldAsset->ToString());
            return true;
        }
        if (oldAsset->LastLoadFailed())
            LOG(Warning, "Moving failed asset '{0}' as raw content while preserving its registered identity.", oldAsset->ToString());

        // Unload
        // Don't unload asset fully, only release ref to file, don't call OnUnload so managed asset and all refs will remain alive
        oldAsset->releaseStorage();
        //oldAsset->onUnload_MainThread();
        //ScopeLock lock(oldAsset->Locker);
        //oldAsset->unload(true);
    }

    // Hold exclusive storage access through the rename so background streaming work
    // cannot reopen the file between releasing its handle and moving it.
    FlaxStorageReference lockedStorage;
    if (ContentStorageManager::LockFileAccess(oldPath, lockedStorage))
    {
        LOG(Error, "Cannot move asset '{0}' because its content storage is still in use.", oldPath);
        return true;
    }
    SCOPE_EXIT
    {
        if (lockedStorage)
            lockedStorage->UnlockFileAccess();
    };

    // Move file
    if (MoveAssetFileSafely(newPath, oldPath))
    {
        LOG(Error, "Cannot move file '{0}' to '{1}'", oldPath, newPath);
        return true;
    }

    // Update cache
    ObjectRegistry.RenameTransientPackage(oldPath, newPath);
    ContentStorageManager::OnRenamed(oldPath, newPath);

    // Check if is loaded
    if (oldAsset)
    {
        // Rename internal call
        oldAsset->onRename(newPath);

        // Load
        //ScopeLock lock(oldAsset->Locker);
        //oldAsset->startLoading();
    }

    return false;
}

bool Content::RenameAssetFolder(const StringView& oldPathInput, const StringView& newPathInput)
{
    ASSERT(IsInMainThread());

    String oldPath(oldPathInput);
    String newPath(newPathInput);
    ContentStorageManager::FormatPath(oldPath);
    ContentStorageManager::FormatPath(newPath);

    if (BuiltinAssetCatalog::Get().IsReadOnlyPath(oldPath) || BuiltinAssetCatalog::Get().IsReadOnlyPath(newPath))
    {
        LOG(Error, "Cannot move a folder into or out of read-only built-in content: '{0}' to '{1}'.", oldPath, newPath);
        return true;
    }

    const bool samePath = FileSystem::AreFilePathsEquivalent(oldPath, newPath);
    const bool destinationIsDirectory = !samePath && FileSystem::DirectoryExists(newPath);
    const bool destinationIsFile = !samePath && FileSystem::FileExists(newPath);
    const bool recoverableZeroByteDestination = destinationIsFile && FileSystem::GetFileSize(newPath) == 0;
    if (!FileSystem::DirectoryExists(oldPath) || destinationIsDirectory || (destinationIsFile && !recoverableZeroByteDestination))
    {
        LOG(Error, "Cannot move folder '{0}' to '{1}'. Source is missing or destination already exists.", oldPath, newPath);
        return true;
    }

    struct LoadedAssetRename
    {
        Asset* Instance;
        String NewPath;
    };
    Array<LoadedAssetRename> loadedAssets;
    {
        ScopeLock lock(AssetsLocker);
        for (const auto& entry : Assets)
        {
            const StringView path = entry.Value->GetPath();
            if (path.Length() > oldPath.Length() && path.StartsWith(oldPath, StringSearchCase::IgnoreCase))
            {
                const Char separator = path[oldPath.Length()];
                if (separator == '/' || separator == '\\')
                {
                    LoadedAssetRename rename;
                    rename.Instance = entry.Value;
                    rename.NewPath = String(newPath) + path.Substring(oldPath.Length());
                    loadedAssets.Add(rename);
                }
            }
        }
    }

    for (auto& rename : loadedAssets)
    {
        if (rename.Instance->WaitForLoaded())
        {
            LOG(Error, "Failed to load asset '{0}' before moving its folder.", rename.Instance->ToString());
            return true;
        }
        rename.Instance->releaseStorage();
    }
    Array<FlaxStorageReference> lockedStorages;
    if (ContentStorageManager::LockFolderAccess(oldPath, lockedStorages))
    {
        LOG(Error, "Cannot move folder '{0}' because one or more asset files are still in use.", oldPath);
        return true;
    }
    SCOPE_EXIT { ContentStorageManager::UnlockFolderAccess(lockedStorages); };

    // FileSystem::MoveFile maps to an atomic rename on the same volume and also supports directories.
    if (MoveFolderPathSafely(newPath, oldPath))
    {
        LOG(Error, "Cannot move folder '{0}' to '{1}'.", oldPath, newPath);
        return true;
    }
    ObjectRegistry.RenameTransientFolder(oldPath, newPath);
    ContentStorageManager::OnRenamedFolder(oldPath, newPath);
    for (auto& rename : loadedAssets)
        rename.Instance->onRename(rename.NewPath);
    return false;
}

bool Content::FastTmpAssetClone(const StringView& path, String& resultPath)
{
    ASSERT(path.HasChars());

    const String dstPath = Globals::TemporaryFolder / Guid::New().ToString(Guid::FormatType::D) + ASSET_FILES_EXTENSION_WITH_DOT;

    if (CloneAssetFile(dstPath, path, Guid::New()))
        return true;

    resultPath = dstPath;
    return false;
}

class CloneAssetFileTask : public MainThreadTask
{
public:
    StringView dstPath;
    StringView srcPath;
    Guid dstId;
    bool overwrite;
    bool* output;

protected:
    bool Run() override
    {
        *output = Content::CloneAssetFile(dstPath, srcPath, dstId, overwrite);
        return false;
    }
};

bool Content::CloneAssetFile(const StringView& dstPath, const StringView& srcPath, const Guid& dstId, bool overwrite)
{
    // Best to run this on the main thread to avoid clone conflicts.
    if (IsInMainThread())
    {
        PROFILE_CPU();
        ASSERT(FileSystem::AreFilePathsEquivalent(srcPath, dstPath) == false && dstId.IsValid());

        LOG(Info, "Cloning asset \'{0}\' to \'{1}\'({2}).", srcPath, dstPath, dstId);

        // Check source file
        if (!FileSystem::FileExists(srcPath))
        {
            LOG(Warning, "Missing source file.");
            return true;
        }
        if (IsSceneAssetPath(srcPath))
        {
            rapidjson_flax::Document sourceDocument;
            if (ReadJsonDocument(srcPath, sourceDocument))
                return true;
            if (IsExternalActorsSceneDocument(sourceDocument))
            {
                LOG(Error, "[FLX-ASSET-CLONE-EXTERNAL-ACTORS-0006] Legacy asset clone cannot copy external-actor scene '{0}'. Use AssetOperationService::CopyAsset so the scene source, metadata, and private fragments commit atomically.", srcPath);
                return true;
            }
        }
        const bool destinationExists = FileSystem::FileExists(dstPath);
        if (destinationExists || FileSystem::DirectoryExists(dstPath))
        {
            if (!overwrite || !destinationExists)
            {
                LOG(Warning, "Clone destination already exists.");
                return true;
            }
        }

        // Existing assets are replaced from an independently prepared storage file. Never open the
        // copied bytes through the destination storage before changing the ID: that storage can still
        // contain the old package layout and repacking it would corrupt assets with different chunks.
        if (destinationExists)
        {
            const String stagingPath = String(dstPath) + TEXT(".replace-stage-") + Guid::New().ToString(Guid::FormatType::N);
            const String backupPath = String(dstPath) + TEXT(".replace-backup-") + Guid::New().ToString(Guid::FormatType::N);
            bool preserveBackup = false;
            SCOPE_EXIT
            {
                FileSystem::DeleteFile(stagingPath);
                if (!preserveBackup)
                    FileSystem::DeleteFile(backupPath);
            };

            const bool isJson = JsonStorageProxy::IsValidExtension(FileSystem::GetExtension(srcPath).ToLower());

            // Prepare and validate the complete replacement without touching the destination.
            if (FileSystem::CopyFile(stagingPath, srcPath))
            {
                LOG(Warning, "Cannot copy asset to replacement staging file.");
                return true;
            }
            if (isJson)
            {
                if (JsonStorageProxy::ChangeId(stagingPath, dstId))
                {
                    LOG(Warning, "Cannot change staged asset ID.");
                    return true;
                }
            }
            else
            {
                auto stagingStorage = ContentStorageManager::GetStorage(stagingPath);
                if (stagingStorage == nullptr)
                {
                    LOG(Warning, "Cannot open replacement staging storage.");
                    return true;
                }
                if (stagingStorage->GetEntriesCount() < 1)
                {
                    LOG(Warning, "Replacement staging storage has no entries.");
                    return true;
                }
                FlaxStorage::Entry e;
                stagingStorage->GetEntry(0, e);
                if (stagingStorage->ChangeAssetID(e, dstId) || !stagingStorage->HasAsset(dstId))
                {
                    LOG(Warning, "Cannot change or verify staged asset ID.");
                    return true;
                }
                if (stagingStorage->CloseFileHandles())
                {
                    LOG(Warning, "Cannot close replacement staging storage.");
                    return true;
                }
            }

            // Lock readers, preserve the exact old bytes, then atomically publish the staged file.
            FlaxStorageReference destinationStorage;
            if (ContentStorageManager::LockFileAccess(dstPath, destinationStorage))
                return true;
            bool destinationStorageLocked = destinationStorage != nullptr;
            SCOPE_EXIT
            {
                if (destinationStorageLocked)
                    destinationStorage->UnlockFileAccess();
            };
            const bool reloadDestinationStorage = destinationStorage && destinationStorage->IsLoaded();
            if (FileSystem::CopyFile(backupPath, dstPath))
            {
                LOG(Warning, "Cannot create replacement backup for asset '{0}'.", dstPath);
                return true;
            }
            if (FileSystem::MoveFile(dstPath, stagingPath, true))
            {
                LOG(Warning, "Cannot commit staged replacement for asset '{0}'.", dstPath);
                return true;
            }

            // Reload cannot run while the file-access mutation lock is held.
            if (destinationStorageLocked)
            {
                destinationStorage->UnlockFileAccess();
                destinationStorageLocked = false;
            }

            bool validationFailed = false;
            if (!isJson)
            {
                auto committedStorage = destinationStorage;
                if (reloadDestinationStorage)
                    validationFailed = committedStorage->Reload();
                else
                    committedStorage = ContentStorageManager::GetStorage(dstPath);
                validationFailed |= committedStorage == nullptr || committedStorage->GetEntriesCount() < 1 || !committedStorage->HasAsset(dstId);
            }
            if (validationFailed)
            {
                LOG(Error, "Replacement validation failed for asset '{0}'. Restoring the original bytes.", dstPath);
                auto storageToRestore = ContentStorageManager::EnsureAccess(dstPath);
                if (FileSystem::MoveFile(dstPath, backupPath, true))
                {
                    preserveBackup = true;
                    LOG(Error, "Cannot restore replacement backup '{0}'. The backup has been preserved for manual recovery.", backupPath);
                    return true;
                }
                if (storageToRestore && storageToRestore->IsLoaded() && storageToRestore->Reload())
                    LOG(Error, "Original asset bytes were restored but its cached storage could not be reloaded: '{0}'.", dstPath);
                return true;
            }

            LOG(Info, "Committed and validated asset replacement at '{0}'.", dstPath);
            return false;
        }

        bool destinationCreated = false;
        SCOPE_EXIT
        {
            if (!destinationCreated)
                FileSystem::DeleteFile(dstPath);
        };

        // Special case for json resources
        if (JsonStorageProxy::IsValidExtension(FileSystem::GetExtension(srcPath).ToLower()))
        {
            if (FileSystem::CopyFile(dstPath, srcPath))
            {
                LOG(Warning, "Cannot copy file to destination.");
                return true;
            }
            if (JsonStorageProxy::ChangeId(dstPath, dstId))
            {
                LOG(Warning, "Cannot change asset ID.");
                return true;
            }
            destinationCreated = true;
            return false;
        }

        // Use quick file copy and remove any partial output on failure.
        if (FileSystem::CopyFile(dstPath, srcPath))
        {
            LOG(Warning, "Cannot copy file to destination.");
            return true;
        }

        // Validate storage before reading entry zero.
        auto storage = ContentStorageManager::GetStorage(dstPath);
        if (storage == nullptr)
        {
            LOG(Warning, "Cannot open cloned asset storage.");
            return true;
        }
        if (storage->GetEntriesCount() < 1)
        {
            LOG(Warning, "Cloned asset storage has no entries.");
            return true;
        }
        FlaxStorage::Entry e;
        storage->GetEntry(0, e);
        if (storage->ChangeAssetID(e, dstId))
        {
            LOG(Warning, "Cannot change asset ID.");
            return true;
        }
        destinationCreated = true;
    }
    else
    {
        CloneAssetFileTask* task = New<CloneAssetFileTask>();
        task->dstId = dstId;
        task->dstPath = dstPath;
        task->srcPath = srcPath;
        task->overwrite = overwrite;

        bool result = false;
        task->output = &result;
        task->Start();
        task->Wait();

        return result;
    }

    return false;
}

#endif

void Content::UnloadAsset(Asset* asset)
{
    if (asset == nullptr)
        return;
    asset->DeleteObject();
}

Asset* Content::CreateVirtualAsset(const MClass* type)
{
    CHECK_RETURN(type, nullptr);
    const auto scriptingType = Scripting::FindScriptingType(type->GetFullName());
    if (scriptingType)
        return CreateVirtualAsset(scriptingType);
    LOG(Error, "Failed to find asset type '{0}'.", String(type->GetFullName()));
    return nullptr;
}

Asset* Content::CreateVirtualAsset(const ScriptingTypeHandle& type)
{
    PROFILE_CPU();
    PROFILE_MEM(Content);
    auto& assetType = type.GetType();

    // Init mock asset info
    AssetInfo info;
    info.ID = Guid::New();
    info.TypeName = String(assetType.Fullname.Get(), assetType.Fullname.Length());
    info.Path = CreateTemporaryAssetPath();

    // Find asset factory based in its type
    auto factory = GetAssetFactory(info);
    if (factory == nullptr)
    {
        LOG(Error, "Cannot find virtual asset factory.");
        return nullptr;
    }
    if (!factory->SupportsVirtualAssets())
    {
        LOG(Error, "Cannot create virtual asset of type '{0}'.", info.TypeName);
        return nullptr;
    }

    // Create asset object
    PROFILE_MEM_BEGIN(ContentAssets);
    auto asset = factory->NewVirtual(info);
    PROFILE_MEM_END();
    if (asset == nullptr)
    {
        LOG(Error, "Cannot create virtual asset object.");
        return nullptr;
    }
    asset->RegisterObject();

    // Call initializer function
    PROFILE_MEM_BEGIN(ContentAssets);
    asset->InitAsVirtual();
    PROFILE_MEM_END();

    // Register asset
    AssetsLocker.Lock();
    ASSERT(!Assets.ContainsKey(asset->GetPersistentObjectId()));
    Assets.Add(asset->GetPersistentObjectId(), asset);
    AssetsLocker.Unlock();

    return asset;
}

void Content::WaitForTask(ContentLoadTask* loadingTask, double timeoutInMilliseconds)
{
    // Check if call is made from the Loading Thread and task has not been taken yet
    auto thread = ThisLoadThread;
    if (thread != nullptr)
    {
        // Note: to reproduce this case just include material into material (use layering).
        // So during loading first material it will wait for child materials loaded calling this function

        const double timeoutInSeconds = timeoutInMilliseconds * 0.001;
        const double startTime = Platform::GetTimeSeconds();
        int32 loopCounter = 0;
        Task* task = loadingTask;
        Array<ContentLoadTask*, InlinedAllocation<64>> localQueue;
#define CHECK_CONDITIONS() (!Engine::ShouldExit() && (timeoutInSeconds <= 0.0 || Platform::GetTimeSeconds() - startTime < timeoutInSeconds))
        do
        {
#if PLATFORM_THREADS_LIMIT > 1
            // Give opportunity for other threads to use the current core
            if (loopCounter == 0)
                ; // First run is fast
            else if (loopCounter < 10)
                Platform::Yield();
            else
                Platform::Sleep(1);
            loopCounter++;

            // Try to execute content tasks
            while (task->IsQueued() && CHECK_CONDITIONS())
            {
                // Dequeue task from the loading queue
                ContentLoadTask* tmp;
                if (LoadTasks.try_dequeue(tmp))
                {
                    if (tmp == task)
                    {
                        if (localQueue.Count() != 0)
                        {
                            // Put back queued tasks
                            LoadTasks.enqueue_bulk(localQueue.Get(), localQueue.Count());
                            localQueue.Clear();
                        }

                        PROFILE_CPU_NAMED("Inline");
                        ZoneColor(0xffaaaaaa);
                        thread->Run(tmp);
                    }
                    else
                    {
                        localQueue.Add(tmp);
                    }
                }
                else
                {
                    // No task in queue but it's queued so other thread could have stolen it into own local queue
                    break;
                }
            }
            if (localQueue.Count() != 0)
            {
                // Put back queued tasks
                LoadTasks.enqueue_bulk(localQueue.Get(), localQueue.Count());
                localQueue.Clear();
            }
#else
            // Try to execute content tasks
            if (task->IsQueued() && CHECK_CONDITIONS() && !LoadTasks.Remove((ContentLoadTask*)task))
            {
                PROFILE_CPU_NAMED("Inline");
                ZoneColor(0xffaaaaaa);
                thread->Run((ContentLoadTask*)task);
            }
            while (!task->IsQueued() && CHECK_CONDITIONS() && LoadTasks.HasItems())
            {
                // Find a task that can be executed (some tasks may be waiting for other tasks to finish so they are not queued yet)
                int32 index = 0;
                for (int32 i = 0; i < LoadTasks.Count(); i++)
                {
                    if (LoadTasks[i]->GetContinueWithTask() == task)
                    {
                        index = i;
                        break;
                    }
                }
                ContentLoadTask* tmp = LoadTasks[index];
                LoadTasks.RemoveAt(index);

                PROFILE_CPU_NAMED("Inline");
                ZoneColor(0xffaaaaaa);
                thread->Run(tmp);
            }
#endif

            // Check if task is done
            if (task->IsEnded())
            {
                // If was fine then wait for the next task
                if (task->IsFinished())
                {
                    task = task->GetContinueWithTask();
                    if (!task)
                        break;
                }
                else
                {
                    // Failed or cancelled so this wait also fails
                    break;
                }
            }
        } while (CHECK_CONDITIONS());
#undef CHECK_CONDITIONS
    }
    else
    {
        // Wait for task end
        loadingTask->Wait(timeoutInMilliseconds);
    }
}

void Content::tryCallOnLoaded(Asset* asset)
{
    ScopeLock lock(LoadedAssetsToInvokeLocker);
    const int32 index = LoadedAssetsToInvoke.Find(asset);
    if (index != -1)
    {
        LoadedAssetsToInvoke.RemoveAtKeepOrder(index);
        asset->onLoaded_MainThread();
#if USE_EDITOR
        onAddDependencies(asset);
#endif
    }
}

void Content::onAssetLoaded(Asset* asset)
{
    // This is called by the asset on loading end
    ScopeLock locker(LoadedAssetsToInvokeLocker);
    LoadedAssetsToInvoke.Add(asset);
}

void Content::onAssetUnload(Asset* asset)
{
    // This is called by the asset on unloading
    LoadedObjects.Remove(asset->GetPersistentObjectId(), asset);
    ScopeLock locker(AssetsLocker);
    Assets.Remove(asset->GetPersistentObjectId());
    UnloadQueue.Remove(asset);
    LoadedAssetsToInvoke.Remove(asset);
#if USE_EDITOR
    for (auto& e : PendingDependencies)
        e.Value.Remove(asset);
#endif
}

void Content::onAssetChangeId(Asset* asset, const Guid& oldId, const Guid& newId)
{
    ScopeLock locker(AssetsLocker);
    const AssetObjectId oldObjectId = asset->_internalObjectId;
    Assets.Remove(asset->_persistentObjectId);
    asset->_persistentObjectId = newId;
    asset->_internalObjectId = AssetObjectId::Main(AssetGuid(newId));
    Assets.Add(asset->_persistentObjectId, asset);
#if USE_EDITOR
    if (PendingDependencies.ContainsKey(oldId))
    {
        auto deps = MoveTemp(PendingDependencies[oldId]);
        PendingDependencies.Remove(oldId);
        PendingDependencies.Add(newId, MoveTemp(deps));
    }
#endif
}

#if USE_EDITOR

void Content::onAssetDepend(BinaryAsset* asset, const Guid& otherId)
{
    ScopeLock locker(AssetsLocker);
    PendingDependencies[otherId].Add(asset);
}

void Content::onAddDependencies(Asset* asset)
{
    auto it = PendingDependencies.Find(asset->GetID());
    if (it.IsNotEnd())
    {
        auto& dependencies = it->Value;
        auto binaryAsset = Asset::Cast<BinaryAsset>(asset);
        if (binaryAsset)
        {
            for (const auto& e : dependencies)
                binaryAsset->_dependantAssets.Add(e.Item);
        }
        PendingDependencies.Remove(it);
    }
}

#endif

bool Content::IsAssetTypeIdInvalid(const ScriptingTypeHandle& type, const ScriptingTypeHandle& assetType)
{
    // Skip if no restrictions for the type
    if (!type || !assetType)
        return false;

#if BUILD_DEBUG || FLAX_TESTS
    // Peek types for debugging
    const auto& typeObj = type.GetType();
    const auto& assetTypeObj = assetType.GetType();
#endif

    // Early out if type matches
    if (type == assetType)
        return false;

    // Check if the asset type inherited from the requested type
    ScriptingTypeHandle it = assetType.GetType().GetBaseType();
    while (it)
    {
        if (type == it)
            return false;
        it = it.GetType().GetBaseType();
    }

    return true;
}

void Content::BeginPassiveLoad()
{
    PassiveLoadDepth++;
}

void Content::EndPassiveLoad()
{
    ASSERT(PassiveLoadDepth > 0);
    PassiveLoadDepth--;
}

bool Content::IsPassiveLoad()
{
    return PassiveLoadDepth > 0;
}

Asset* Content::LoadAsyncPreview(const Guid& objectId, const ScriptingTypeHandle& type)
{
    BeginPassiveLoad();
    Asset* result = LoadAssetAsync(objectId, type);
    EndPassiveLoad();
    return result;
}

Asset* Content::LoadAssetObjectAsyncInternal(const Guid& objectId, const ScriptingTypeHandle& type)
{
    if (!objectId.IsValid())
        return nullptr;
    PROFILE_MEM(Content);
    const bool passiveLoad = IsPassiveLoad();
    const Guid persistentObjectId = objectId;

    // Check if asset has been already loaded
    Asset* result = nullptr;
    AssetsLocker.Lock();
    Assets.TryGet(persistentObjectId, result);
    if (result)
    {
        AssetsLocker.Unlock();

        // Validate type
        if (IsAssetTypeIdInvalid(type, result->GetTypeHandle()) && !result->Is(type))
        {
            LOG(Warning, "Different loaded asset type! Asset: \'{0}\'. Expected type: {1}", result->ToString(), type.ToString());
            LogContext::Print(LogType::Warning);
            return nullptr;
        }
        return result;
    }

#if PLATFORM_THREADS_LIMIT > 1
    // Check if that asset is during loading
    if (LoadCallAssets.Contains(persistentObjectId))
    {
        AssetsLocker.Unlock();

        // Wait for loading end by other thread
        bool contains = true;
        while (contains)
        {
            Platform::Sleep(1);
            AssetsLocker.Lock();
            contains = LoadCallAssets.Contains(persistentObjectId);
            AssetsLocker.Unlock();
        }
        {
            ScopeLock lock(AssetsLocker);
            Assets.TryGet(persistentObjectId, result);
        }
        return result;
    }

    // Mark asset as loading and release lock so other threads can load other assets
    LoadCallAssets.Add(persistentObjectId);
#define LOAD_FAILED() AssetsLocker.Lock(); LoadCallAssets.Remove(persistentObjectId); AssetsLocker.Unlock(); return nullptr
#else
#define LOAD_FAILED() return nullptr
#endif

    AssetsLocker.Unlock();

    // Built-ins and editor-private transient packages retain their dedicated immutable roots.
    // Every canonical project object uses the exact artifact/catalog object loader.
    AssetInfo assetInfo;
    AssetLoadLocation loadLocation;
    bool hasDirectPackageLocation = false;
#if USE_EDITOR
    if (BuiltinAssetCatalog::Get().TryGet(objectId, assetInfo))
    {
        loadLocation = AssetLoadLocation::Package(assetInfo);
        hasDirectPackageLocation = true;
    }
    AssetRecord canonicalRecord;
    const bool isCanonicalProjectObject = AssetDatabase::Get().TryGetRecord(objectId, canonicalRecord);
    const String transientPath = ObjectRegistry.GetEditorObjectPath(objectId);
    if (!hasDirectPackageLocation && !isCanonicalProjectObject &&
        AssetPathPolicy::IsSameOrChild(transientPath, Globals::ProjectCacheFolder) &&
        ObjectRegistry.FindObject(objectId, assetInfo))
    {
        loadLocation = AssetLoadLocation::Package(assetInfo);
        hasDirectPackageLocation = true;
    }
#endif

    if (hasDirectPackageLocation)
    {
        if (assetInfo.ObjectID != persistentObjectId)
        {
            LOG(Error, "Resolved asset object identity changed from {0} to {1}.", objectId, assetInfo.ObjectID);
            LOAD_FAILED();
        }
#if ASSETS_LOADING_EXTRA_VERIFICATION
        if (!FileSystem::FileExists(loadLocation.Artifact.StoragePath.Get()))
        {
            LOG(Error, "Cannot find asset storage '{0}' for asset '{1}'", loadLocation.Artifact.StoragePath.Get(), assetInfo.Path);
            LOAD_FAILED();
        }
#endif

        IAssetFactory* factory = GetAssetFactory(assetInfo);
        if (!factory)
        {
            LOG(Error, "Cannot find asset factory. Info: {0}", assetInfo.ToString());
            LOAD_FAILED();
        }
        PROFILE_MEM_BEGIN(ContentAssets);
        result = factory->New(loadLocation);
        PROFILE_MEM_END();
    }
    else
    {
        AssetObjectLoadResult objectResult;
        AssetPipelineDiagnostic diagnostic;
        if (ObjectLoader.Load(persistentObjectId, objectResult, diagnostic))
        {
            if (!passiveLoad)
                LOG(Error, "{0}: {1} Asset: {2}, path: '{3}'.", GetAssetPipelineDiagnosticCodeName(diagnostic.Code), diagnostic.Message, objectId, diagnostic.SourcePath);
            LOAD_FAILED();
        }
        result = static_cast<Asset*>(objectResult.Instance);
        assetInfo = AssetInfo(result->GetID(), result->GetPersistentObjectId(), result->GetTypeName(), result->GetPath(), objectResult.Revision);
    }
    if (!result)
    {
        LOG(Error, "Cannot create asset object. Info: {0}", assetInfo.ToString());
        LOAD_FAILED();
    }
    result->_persistentObjectId = assetInfo.ObjectID;
    result->_isPassiveLoad = passiveLoad;
    ASSERT(result->GetID() == assetInfo.ID);
    ASSERT(result->GetPersistentObjectId() == assetInfo.ObjectID);
#if ASSETS_LOADING_EXTRA_VERIFICATION
    if (IsAssetTypeIdInvalid(type, result->GetTypeHandle()) && !result->Is(type))
    {
        LOG(Warning, "Different loaded asset type! Asset: '{0}'. Expected type: {1}", assetInfo.ToString(), type.ToString());
        result->DeleteObject();
        LOAD_FAILED();
    }
#endif
    if (!result->IsInternalType())
        result->RegisterObject();

    // Register asset
    AssetsLocker.Lock();
#if ASSETS_LOADING_EXTRA_VERIFICATION
    ASSERT(!Assets.ContainsKey(persistentObjectId));
#endif
    Assets.Add(persistentObjectId, result);

    // Start asset loading
    result->startLoading();

    // Remove from the loading queue and release lock
#if PLATFORM_THREADS_LIMIT > 1
    LoadCallAssets.Remove(persistentObjectId);
#endif
    AssetsLocker.Unlock();

#undef LOAD_FAILED

    return result;
}
