// Copyright (c) Wojciech Figat. All rights reserved.

#include "BuiltinAssetCatalog.h"
#include "BuiltinAssetCatalogFormat.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#if USE_EDITOR
#include "Editor/Editor.h"
#include "Editor/ProjectInfo.h"
#endif

namespace
{
    const Char* CatalogFileName = TEXT("BuiltinAssetCatalog.bin");

    struct CatalogRoot
    {
        String Path;
        String UriPrefix;

        bool operator<(const CatalogRoot& other) const
        {
            const int32 prefixComparison = UriPrefix.Compare(other.UriPrefix);
            return prefixComparison != 0 ? prefixComparison < 0 : Path.Compare(other.Path) < 0;
        }
    };

    bool IsEngineRoot(const CatalogRoot& root)
    {
        return root.UriPrefix == TEXT("builtin://Engine/");
    }

    String NormalizeLookupKey(const StringView& value)
    {
        String result(value);
        result.Replace(TEXT('\\'), TEXT('/'));
        return result.ToLower();
    }

    String MakeEngineUri(const StringView& relativePath)
    {
        const String pathWithoutExtension(StringUtils::GetPathWithoutExtension(relativePath));
        if (pathWithoutExtension.StartsWith(TEXT("Editor/"), StringSearchCase::IgnoreCase) ||
            pathWithoutExtension.StartsWith(TEXT("Engine/"), StringSearchCase::IgnoreCase))
            return String(TEXT("builtin://")) + pathWithoutExtension;
        return String(TEXT("builtin://Engine/")) + pathWithoutExtension;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const Guid& id,
        const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.AssetGuid = id;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool ScanRoot(const CatalogRoot& root, Array<BuiltinAssetCatalogSerializedEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, root.Path, TEXT("*.flax"), DirectorySearchOption::AllDirectories))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, Guid::Empty, root.Path,
                TEXT("Cannot enumerate read-only built-in content."));
        Sorting::QuickSort(files);

        for (String& path : files)
        {
            ContentStorageManager::FormatPath(path);
            const FlaxStorageReference storage = ContentStorageManager::GetStorage(path);
            if (!storage)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, path,
                    TEXT("Cannot read a built-in asset storage file."));

            Array<FlaxStorage::Entry> storageEntries;
            storage->GetEntries(storageEntries);
            if (storageEntries.IsEmpty())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, path,
                    TEXT("Built-in asset storage contains no objects."));

            String relativePath = FileSystem::ConvertAbsolutePathToRelative(root.Path, path);
            relativePath.Replace(TEXT('\\'), TEXT('/'));
            if (relativePath.IsEmpty() || relativePath.Contains(TEXT("\\")) ||
                FileSystem::GetExtension(relativePath).ToLower() != TEXT("flax"))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, Guid::Empty, path,
                    TEXT("Built-in storage has a non-portable relative path."));
            const String baseUri = IsEngineRoot(root)
                ? MakeEngineUri(relativePath)
                : root.UriPrefix + String(StringUtils::GetPathWithoutExtension(relativePath));

            for (const FlaxStorage::Entry& storageEntry : storageEntries)
            {
                if (!storageEntry.ID.IsValid() || storageEntry.TypeName.IsEmpty())
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, storageEntry.ID, path,
                        TEXT("Built-in asset storage has an invalid object entry."));
                BuiltinAssetCatalogSerializedEntry entry;
                entry.ObjectID = AssetObjectId::Main(AssetGuid(storageEntry.ID));
                entry.TypeName = storageEntry.TypeName;
                entry.RelativePath = relativePath;
                entry.Uri = baseUri;
                if (storageEntries.Count() > 1)
                    entry.Uri += String::Format(TEXT("#{0}"), storageEntry.ID);
                entries.Add(MoveTemp(entry));
            }
        }
        return false;
    }

    bool SaveRoot(const CatalogRoot& root, const Array<BuiltinAssetCatalogSerializedEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> catalog;
        if (BuiltinAssetCatalogFormat::ToBytes(entries, catalog, diagnostic))
            return true;

        const String destination = root.Path / CatalogFileName;
        const String staging = destination + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { FileSystem::DeleteFile(staging); };
        if (File::WriteAllBytes(staging, catalog.Get(), catalog.Count()) || FileSystem::MoveFile(destination, staging, true))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, Guid::Empty, destination,
                TEXT("Built-in catalog could not be written atomically."));
        return false;
    }

    enum class LoadRootResult
    {
        Success,
        LegacyVersion,
        Failure,
    };

    LoadRootResult LoadRoot(const CatalogRoot& root, Array<BuiltinAssetCatalogSerializedEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        const String catalogPath = root.Path / CatalogFileName;
        BytesContainer bytes;
        if (File::ReadAllBytes(catalogPath, bytes) || bytes.Length() > MAX_int32)
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                TEXT("Built-in catalog file is missing, unreadable, or truncated."));
            return LoadRootResult::Failure;
        }

        const Span<byte> input(bytes.Get(), static_cast<int32>(bytes.Length()));
        if (BuiltinAssetCatalogFormat::IsLegacyVersion(input))
            return LoadRootResult::LegacyVersion;

        if (BuiltinAssetCatalogFormat::FromBytes(input, entries, diagnostic))
        {
            diagnostic.SourcePath = catalogPath;
            return LoadRootResult::Failure;
        }
        for (const BuiltinAssetCatalogSerializedEntry& entry : entries)
        {
            const bool uriMatchesRoot = entry.Uri.StartsWith(root.UriPrefix, StringSearchCase::IgnoreCase) ||
                (IsEngineRoot(root) &&
                    entry.Uri.StartsWith(TEXT("builtin://Editor/"), StringSearchCase::IgnoreCase));
            if (!uriMatchesRoot)
            {
                Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, entry.ObjectID.Asset.Value, catalogPath,
                    TEXT("Built-in catalog URI does not belong to its immutable root."));
                return LoadRootResult::Failure;
            }
        }
        return LoadRootResult::Success;
    }
}

BuiltinAssetCatalog& BuiltinAssetCatalog::Get()
{
    static BuiltinAssetCatalog instance;
    return instance;
}

bool BuiltinAssetCatalog::Initialize(AssetPipelineDiagnostic& diagnostic)
{
    Dispose();
    diagnostic = AssetPipelineDiagnostic();
#if !USE_EDITOR
    return false;
#else
    Array<CatalogRoot> roots;
    CatalogRoot engineRoot;
    engineRoot.Path = Globals::EngineContentFolder;
    engineRoot.UriPrefix = TEXT("builtin://Engine/");
    roots.Add(MoveTemp(engineRoot));

    for (ProjectInfo* project : ProjectInfo::ProjectsCache)
    {
        if (!project || project == Editor::Project)
            continue;
        const String contentRoot = project->ProjectFolderPath / TEXT("Content");
        if (FileSystem::AreFilePathsEquivalent(contentRoot, Globals::EngineContentFolder))
            continue;
        CatalogRoot pluginRoot;
        pluginRoot.Path = contentRoot;
        String pluginName = project->Name;
        pluginName.Replace(TEXT('/'), TEXT('_'));
        pluginName.Replace(TEXT('\\'), TEXT('_'));
        pluginRoot.UriPrefix = String(TEXT("builtin://Plugin/")) + pluginName + TEXT("/");
        roots.Add(MoveTemp(pluginRoot));
    }
    Sorting::QuickSort(roots);

    for (const CatalogRoot& root : roots)
    {
        if (!FileSystem::DirectoryExists(root.Path))
        {
            if (FileSystem::AreFilePathsEquivalent(root.Path, Globals::EngineContentFolder))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, Guid::Empty, root.Path,
                    TEXT("Engine built-in content root is missing."));
            continue;
        }

        String normalizedRoot = root.Path;
        ContentStorageManager::FormatPath(normalizedRoot);
        _roots.Add(MoveTemp(normalizedRoot));

        Array<BuiltinAssetCatalogSerializedEntry> rootEntries;
        const String catalogPath = root.Path / CatalogFileName;
        if (FileSystem::FileExists(catalogPath))
        {
            const LoadRootResult loadResult = LoadRoot(root, rootEntries, diagnostic);
            if (loadResult == LoadRootResult::Failure)
                return true;
            if (loadResult == LoadRootResult::LegacyVersion)
            {
                if (ScanRoot(root, rootEntries, diagnostic))
                    return true;
            }
            else
            {
                _prebuiltRoots++;
            }
        }
        else
        {
            if (ScanRoot(root, rootEntries, diagnostic))
                return true;
            if (rootEntries.HasItems())
            {
                if (SaveRoot(root, rootEntries, diagnostic))
                    return true;
                _generatedRoots++;
            }
        }

        for (const BuiltinAssetCatalogSerializedEntry& serialized : rootEntries)
        {
            String path = root.Path / serialized.RelativePath;
            ContentStorageManager::FormatPath(path);
            if (!FileSystem::FileExists(path))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, serialized.ObjectID.Asset.Value, path,
                    TEXT("Built-in catalog refers to a missing immutable artifact."));

            BuiltinAssetCatalogEntry entry;
            entry.Info = AssetInfo(serialized.ObjectID.ToRuntimeObjectGuid(), serialized.ObjectID, serialized.TypeName, path);
            entry.Uri = serialized.Uri;
            const String pathKey = NormalizeLookupKey(path);
            const String uriKey = NormalizeLookupKey(entry.Uri);
            if (_byObject.ContainsKey(entry.Info.ObjectID) || _byUri.ContainsKey(uriKey))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::DuplicateGuid, entry.Info.ID, path,
                    TEXT("Built-in catalog contains a duplicate object ID or URI."));

            const int32 catalogIndex = _entries.Count();
            _entries.Add(MoveTemp(entry));
            _byObject.Add(_entries[catalogIndex].Info.ObjectID, catalogIndex);
            _byUri.Add(uriKey, catalogIndex);
            if (!_byPath.ContainsKey(pathKey))
                _byPath.Add(pathKey, catalogIndex);
        }
    }
    return false;
#endif
}

void BuiltinAssetCatalog::Dispose()
{
    _entries.Clear();
    _roots.Clear();
    _byObject.Clear();
    _byPath.Clear();
    _byUri.Clear();
    _prebuiltRoots = 0;
    _generatedRoots = 0;
}

bool BuiltinAssetCatalog::TryGet(const AssetObjectId& objectId, AssetInfo& info) const
{
    const int32* index = _byObject.TryGet(objectId);
    if (!index)
        return false;
    info = _entries[*index].Info;
    return true;
}

bool BuiltinAssetCatalog::TryGet(const Guid& runtimeId, AssetInfo& info) const
{
    for (const BuiltinAssetCatalogEntry& entry : _entries)
    {
        if (entry.Info.ID == runtimeId)
        {
            info = entry.Info;
            return true;
        }
    }
    return false;
}

bool BuiltinAssetCatalog::TryGetByPath(const StringView& pathOrUri, AssetInfo& info) const
{
    const String key = NormalizeLookupKey(pathOrUri);
    const int32* index = pathOrUri.StartsWith(TEXT("builtin://"), StringSearchCase::IgnoreCase)
        ? _byUri.TryGet(key)
        : _byPath.TryGet(key);
    if (!index)
        return false;
    info = _entries[*index].Info;
    return true;
}

bool BuiltinAssetCatalog::IsReadOnlyPath(const StringView& pathOrUri) const
{
    if (pathOrUri.StartsWith(TEXT("builtin://"), StringSearchCase::IgnoreCase))
        return true;
    String path(pathOrUri);
    ContentStorageManager::FormatPath(path);
    for (const String& root : _roots)
    {
        if (FileSystem::AreFilePathsEquivalent(path, root))
            return true;
        if (path.Length() > root.Length() && path.StartsWith(root, StringSearchCase::IgnoreCase))
        {
            const Char separator = path[root.Length()];
            if (separator == '/' || separator == '\\')
                return true;
        }
    }
    return false;
}

StringView BuiltinAssetCatalog::GetStoragePath(const AssetObjectId& objectId) const
{
    const int32* index = _byObject.TryGet(objectId);
    return index ? StringView(_entries[*index].Info.Path) : StringView::Empty;
}

StringView BuiltinAssetCatalog::GetUri(const AssetObjectId& objectId) const
{
    const int32* index = _byObject.TryGet(objectId);
    return index ? StringView(_entries[*index].Uri) : StringView::Empty;
}

void BuiltinAssetCatalog::GetAll(Array<Guid>& result) const
{
    result.EnsureCapacity(result.Count() + _entries.Count());
    for (const BuiltinAssetCatalogEntry& entry : _entries)
        result.Add(entry.Info.ID);
}

void BuiltinAssetCatalog::GetAllByTypeName(const StringView& typeName, Array<Guid>& result) const
{
    for (const BuiltinAssetCatalogEntry& entry : _entries)
    {
        if (entry.Info.TypeName == typeName)
            result.Add(entry.Info.ID);
    }
}
