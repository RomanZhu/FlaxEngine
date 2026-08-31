// Copyright (c) Wojciech Figat. All rights reserved.

#include "BuiltinAssetCatalog.h"
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
    constexpr uint32 CatalogMagic = 0x43494246; // FBIC
    constexpr uint32 CatalogVersion = 1;
    constexpr int32 CatalogHeaderSize = sizeof(uint32) * 3 + sizeof(ContentHash);
    constexpr uint32 MaximumCatalogEntries = 1000000;
    constexpr uint32 MaximumCatalogStringBytes = 1024 * 1024;
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

    struct SerializedBuiltinEntry
    {
        Guid RuntimeID;
        AssetObjectId ObjectID;
        String TypeName;
        String RelativePath;
        String Uri;
    };

    class CatalogWriter
    {
    public:
        Array<byte> Data;

        void WriteUInt32(uint32 value)
        {
            const byte bytes[] =
            {
                static_cast<byte>(value), static_cast<byte>(value >> 8),
                static_cast<byte>(value >> 16), static_cast<byte>(value >> 24),
            };
            Data.Add(bytes, ARRAY_COUNT(bytes));
        }

        void WriteUInt64(uint64 value)
        {
            byte bytes[8];
            for (int32 i = 0; i < ARRAY_COUNT(bytes); i++)
                bytes[i] = static_cast<byte>(value >> (i * 8));
            Data.Add(bytes, ARRAY_COUNT(bytes));
        }

        void WriteGuid(const Guid& value)
        {
            WriteUInt32(value.A);
            WriteUInt32(value.B);
            WriteUInt32(value.C);
            WriteUInt32(value.D);
        }

        void WriteObject(const AssetObjectId& value)
        {
            WriteGuid(value.Asset.Value);
            WriteUInt64(static_cast<uint64>(value.LocalId));
        }

        void WriteHash(const ContentHash& value)
        {
            Data.Add(value.Bytes, ARRAY_COUNT(value.Bytes));
        }

        void WriteString(const StringView& value)
        {
            const StringAnsi utf8(value);
            WriteUInt32(utf8.Length());
            if (utf8.HasChars())
                Data.Add(reinterpret_cast<const byte*>(utf8.Get()), utf8.Length());
        }
    };

    class CatalogReader
    {
        const byte* _data;
        uint32 _length;
        uint32 _position = 0;

    public:
        CatalogReader(const byte* data, uint32 length)
            : _data(data)
            , _length(length)
        {
        }

        bool ReadBytes(void* output, uint32 length)
        {
            if (length > _length - _position)
                return true;
            Platform::MemoryCopy(output, _data + _position, length);
            _position += length;
            return false;
        }

        bool ReadUInt32(uint32& value)
        {
            byte bytes[4];
            if (ReadBytes(bytes, ARRAY_COUNT(bytes)))
                return true;
            value = static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
                    (static_cast<uint32>(bytes[2]) << 16) | (static_cast<uint32>(bytes[3]) << 24);
            return false;
        }

        bool ReadUInt64(uint64& value)
        {
            byte bytes[8];
            if (ReadBytes(bytes, ARRAY_COUNT(bytes)))
                return true;
            value = 0;
            for (int32 i = 0; i < ARRAY_COUNT(bytes); i++)
                value |= static_cast<uint64>(bytes[i]) << (i * 8);
            return false;
        }

        bool ReadGuid(Guid& value)
        {
            return ReadUInt32(value.A) || ReadUInt32(value.B) || ReadUInt32(value.C) || ReadUInt32(value.D);
        }

        bool ReadObject(AssetObjectId& value)
        {
            Guid guid;
            uint64 localId;
            if (ReadGuid(guid) || ReadUInt64(localId))
                return true;
            value = AssetObjectId(AssetGuid(guid), static_cast<int64>(localId));
            return false;
        }

        bool ReadHash(ContentHash& value)
        {
            return ReadBytes(value.Bytes, ARRAY_COUNT(value.Bytes));
        }

        bool ReadString(String& value)
        {
            uint32 length;
            if (ReadUInt32(length) || length > MaximumCatalogStringBytes || length > _length - _position)
                return true;
            value = String(StringAnsiView(reinterpret_cast<const char*>(_data + _position), length));
            _position += length;
            return false;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };

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

    bool IsPortableRelativePath(const StringView& value)
    {
        if (value.IsEmpty() || value.StartsWith(TEXT("/")) || value.StartsWith(TEXT("\\")) || value.Contains(TEXT(":")) ||
            value.Contains(TEXT("\\")) || value.Contains(TEXT("//")) || value == TEXT(".") || value == TEXT("..") ||
            value.StartsWith(TEXT("../")) || value.Contains(TEXT("/../")) || value.EndsWith(TEXT("/..")))
            return false;
        return FileSystem::GetExtension(value).ToLower() == TEXT("flax");
    }

    bool ScanRoot(const CatalogRoot& root, Array<SerializedBuiltinEntry>& entries, AssetPipelineDiagnostic& diagnostic)
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
            if (!IsPortableRelativePath(relativePath))
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
                SerializedBuiltinEntry entry;
                entry.RuntimeID = storageEntry.ID;
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

    bool SaveRoot(const CatalogRoot& root, const Array<SerializedBuiltinEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        CatalogWriter payload;
        payload.WriteUInt32(entries.Count());
        for (const SerializedBuiltinEntry& entry : entries)
        {
            payload.WriteGuid(entry.RuntimeID);
            payload.WriteObject(entry.ObjectID);
            payload.WriteString(entry.TypeName);
            payload.WriteString(entry.RelativePath);
            payload.WriteString(entry.Uri);
        }

        CatalogWriter catalog;
        catalog.WriteUInt32(CatalogMagic);
        catalog.WriteUInt32(CatalogVersion);
        catalog.WriteUInt32(payload.Data.Count());
        catalog.WriteHash(ContentHash::Compute(payload.Data.Get(), payload.Data.Count()));
        catalog.Data.Add(payload.Data.Get(), payload.Data.Count());

        const String destination = root.Path / CatalogFileName;
        const String staging = destination + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { FileSystem::DeleteFile(staging); };
        if (File::WriteAllBytes(staging, catalog.Data.Get(), catalog.Data.Count()) || FileSystem::MoveFile(destination, staging, true))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, Guid::Empty, destination,
                TEXT("Built-in catalog could not be written atomically."));
        return false;
    }

    bool LoadRoot(const CatalogRoot& root, Array<SerializedBuiltinEntry>& entries, AssetPipelineDiagnostic& diagnostic)
    {
        const String catalogPath = root.Path / CatalogFileName;
        BytesContainer bytes;
        if (File::ReadAllBytes(catalogPath, bytes) || bytes.Length() > MAX_int32 || bytes.Length() < CatalogHeaderSize)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                TEXT("Built-in catalog file is missing, unreadable, or truncated."));

        CatalogReader header(bytes.Get(), static_cast<uint32>(bytes.Length()));
        uint32 magic;
        uint32 version;
        uint32 payloadSize;
        ContentHash payloadHash;
        if (header.ReadUInt32(magic) || header.ReadUInt32(version) || header.ReadUInt32(payloadSize) || header.ReadHash(payloadHash) ||
            magic != CatalogMagic || version != CatalogVersion || payloadSize != bytes.Length() - CatalogHeaderSize)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                TEXT("Built-in catalog header, version, or payload size is invalid."));

        const byte* payloadBytes = bytes.Get() + CatalogHeaderSize;
        if (ContentHash::Compute(payloadBytes, payloadSize) != payloadHash)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                TEXT("Built-in catalog payload checksum is invalid."));

        CatalogReader payload(payloadBytes, payloadSize);
        uint32 count;
        if (payload.ReadUInt32(count) || count == 0 || count > MaximumCatalogEntries)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                TEXT("Built-in catalog entry count is invalid."));
        entries.Resize(count, false);
        for (SerializedBuiltinEntry& entry : entries)
        {
            if (payload.ReadGuid(entry.RuntimeID) || payload.ReadObject(entry.ObjectID) || payload.ReadString(entry.TypeName) ||
                payload.ReadString(entry.RelativePath) || payload.ReadString(entry.Uri))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                    TEXT("Built-in catalog contains a truncated entry."));
            const bool uriMatchesRoot = entry.Uri.StartsWith(root.UriPrefix, StringSearchCase::IgnoreCase) ||
                (IsEngineRoot(root) &&
                    entry.Uri.StartsWith(TEXT("builtin://Editor/"), StringSearchCase::IgnoreCase));
            if (!entry.RuntimeID.IsValid() || !entry.ObjectID.IsValid() || entry.ObjectID.ToRuntimeObjectGuid() != entry.RuntimeID ||
                entry.TypeName.IsEmpty() || !IsPortableRelativePath(entry.RelativePath) || !uriMatchesRoot)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, entry.RuntimeID, catalogPath,
                    TEXT("Built-in catalog contains an invalid identity, type, path, or URI."));
        }
        if (!payload.AtEnd())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, Guid::Empty, catalogPath,
                TEXT("Built-in catalog contains unexpected trailing data."));
        return false;
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

        Array<SerializedBuiltinEntry> rootEntries;
        const String catalogPath = root.Path / CatalogFileName;
        if (FileSystem::FileExists(catalogPath))
        {
            if (LoadRoot(root, rootEntries, diagnostic))
                return true;
            _prebuiltRoots++;
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

        for (const SerializedBuiltinEntry& serialized : rootEntries)
        {
            String path = root.Path / serialized.RelativePath;
            ContentStorageManager::FormatPath(path);
            if (!FileSystem::FileExists(path))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, serialized.RuntimeID, path,
                    TEXT("Built-in catalog refers to a missing immutable artifact."));

            BuiltinAssetCatalogEntry entry;
            entry.Info = AssetInfo(serialized.RuntimeID, serialized.ObjectID, serialized.TypeName, path);
            entry.Uri = serialized.Uri;
            const String pathKey = NormalizeLookupKey(path);
            const String uriKey = NormalizeLookupKey(entry.Uri);
            if (_byObject.ContainsKey(entry.Info.ObjectID) || _byRuntimeId.ContainsKey(entry.Info.ID) || _byUri.ContainsKey(uriKey))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::DuplicateGuid, entry.Info.ID, path,
                    TEXT("Built-in catalog contains a duplicate object ID, runtime ID, or URI."));

            const int32 catalogIndex = _entries.Count();
            _entries.Add(MoveTemp(entry));
            _byObject.Add(_entries[catalogIndex].Info.ObjectID, catalogIndex);
            _byRuntimeId.Add(_entries[catalogIndex].Info.ID, catalogIndex);
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
    _byRuntimeId.Clear();
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
    const int32* index = _byRuntimeId.TryGet(runtimeId);
    if (!index)
        return false;
    info = _entries[*index].Info;
    return true;
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

StringView BuiltinAssetCatalog::GetStoragePath(const Guid& runtimeId) const
{
    const int32* index = _byRuntimeId.TryGet(runtimeId);
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
