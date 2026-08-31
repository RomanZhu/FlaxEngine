// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSaveService.h"
#include "SubAsset.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

namespace
{
    const Char* AuthoredProcessorID = TEXT("Flax.AuthoredObject");
    const Char* AuthoredDocumentType = TEXT("Flax.AuthoredObject");

    bool Fail(AssetSaveResult& result, const StringView& path, const StringView& message, bool conflict = false)
    {
        result = AssetSaveResult();
        result.SourcePath = path;
        result.Message = message;
        result.Conflict = conflict;
        return true;
    }

    void Succeed(AssetSaveResult& result, const StringView& path, const Guid& id, bool saved, uint64 revision = 0)
    {
        result.Succeeded = true;
        result.Saved = saved;
        result.SourcePath = path;
        result.AssetID = id;
        result.EditRevision = revision;
    }

    bool ReadAndHash(const StringView& path, Array<byte>& bytes, String& hash)
    {
        if (File::ReadAllBytes(path, bytes))
            return true;
        hash = String(ContentHash::Compute(bytes.Get(), bytes.Count()).ToString());
        return false;
    }
}

AssetSaveService::AssetSaveService(const StringView& projectRoot, const StringView& contentRoot, const StringView& journalRoot, const StringView& recoveryRoot)
    : _mutations(projectRoot, contentRoot, journalRoot, recoveryRoot)
{
}

String AssetSaveService::PathKey(const StringView& path)
{
    String result(path);
    StringUtils::PathRemoveRelativeParts(result);
    result.Replace((Char)'\\', (Char)'/');
#if PLATFORM_WINDOWS || PLATFORM_UWP || PLATFORM_XBOX_ONE || PLATFORM_XBOX_SCARLETT
    result = result.ToLower();
#endif
    return result;
}

bool AssetSaveService::ReadAuthored(const StringView& path, AuthoredSourceDocument& document, AssetMeta& meta,
    String& sourceHash, String& metaHash, String& error) const
{
    AssetMutationResult validation;
    if (_mutations.Validate(AssetMutationOperation::ReplaceAsset, path, StringView(), validation))
    {
        error = validation.Message;
        return true;
    }
    const String source(validation.SourcePath);
    const String metaPath = source + TEXT(".meta");
    if (!FileSystem::FileExists(source) || !FileSystem::FileExists(metaPath) || FileSystem::DirectoryExists(source))
    {
        error = TEXT("Authored source or adjacent metadata is missing.");
        return true;
    }
    if (FileSystem::IsReadOnly(source) || FileSystem::IsReadOnly(metaPath))
    {
        error = TEXT("Authored source or adjacent metadata is read-only.");
        return true;
    }
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(metaPath, meta, diagnostic))
    {
        error = diagnostic.Message;
        return true;
    }
    if (meta.SourceKind != AssetSourceKind::TextDocument || meta.Processor.ID != AuthoredProcessorID)
    {
        error = meta.SourceKind == AssetSourceKind::ImportedSource
            ? TEXT("Imported outputs are read-only. Extract or duplicate an authored copy before editing.")
            : TEXT("This source is not a generic authored multi-object document.");
        return true;
    }
    Array<byte> sourceBytes;
    Array<byte> metaBytes;
    if (ReadAndHash(source, sourceBytes, sourceHash) || ReadAndHash(metaPath, metaBytes, metaHash))
    {
        error = TEXT("Authored source pair could not be read for editing.");
        return true;
    }
    if (AuthoredSourceDocument::Parse(StringAnsiView((const char*)sourceBytes.Get(), sourceBytes.Count()), document, error))
        return true;
    if (document.DocumentType != AuthoredDocumentType)
    {
        error = TEXT("The authored source document type is not owned by the generic serializer.");
        return true;
    }
    return false;
}

bool AssetSaveService::SynchronizeMeta(const AuthoredSourceDocument& document, AssetMeta& meta, String& error)
{
    const AuthoredSourceObject* root = document.FindObject(1);
    if (!root || !meta.ID.IsValid() || meta.FolderAsset)
    {
        error = TEXT("Generic authored documents require a stable compatibility root object with local ID one.");
        return true;
    }
    meta.AssetType = root->TypeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = AuthoredProcessorID;
    meta.Processor.SettingsVersion = 1;
    if (meta.Processor.SettingsJson.IsEmpty())
        meta.Processor.SettingsJson = "{}\n";
    meta.SubAssets.Clear();
    for (const AuthoredSourceObject& object : document.Objects)
    {
        if (object.LocalId == 1)
            continue;
        SubAssetMeta sub;
        sub.LocalId = object.LocalId;
        sub.TypeName = object.TypeName;
        sub.DisplayName = object.Name;
        meta.SubAssets.Add(object.StableKey, MoveTemp(sub));
    }
    for (const AuthoredSourceTombstone& tombstone : document.Tombstones)
    {
        SubAssetMeta sub;
        sub.LocalId = tombstone.LocalId;
        sub.TypeName = tombstone.TypeName;
        sub.DisplayName = tombstone.Name;
        sub.Removed = true;
        meta.SubAssets.Add(tombstone.StableKey, MoveTemp(sub));
    }
    return false;
}

bool AssetSaveService::Canonicalize(const AuthoredSourceDocument& document, StringAnsi& sourceJson, String& error)
{
    return document.ToCanonicalJson(sourceJson, error);
}

bool AssetSaveService::CreateAsset(const StringView& path, const StringView& stableKey, const StringView& typeName,
    const StringView& name, const StringAnsiView& dataJson, AssetSaveResult& result)
{
    if (typeName.IsEmpty())
        return Fail(result, path, TEXT("Authored asset type is empty."));
    AuthoredSourceDocument document;
    document.DocumentType = AuthoredDocumentType;
    document.MainObjectLocalId = 1;
    AuthoredSourceObject object;
    object.LocalId = 1;
    object.StableKey = SubAssetPolicy::NormalizeKey(stableKey);
    object.TypeName = typeName;
    object.Name = name;
    object.DataJson = StringAnsi(dataJson);
    if (!SubAssetPolicy::IsKeyValid(object.StableKey))
        return Fail(result, path, TEXT("Authored asset stable key is invalid."));
    document.Objects.Add(MoveTemp(object));
    StringAnsi sourceJson;
    String error;
    if (Canonicalize(document, sourceJson, error))
        return Fail(result, path, error);
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = AuthoredProcessorID;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    AssetMutationResult mutation;
    if (_mutations.CreateAsset(path, sourceJson, meta, mutation))
    {
        Fail(result, path, mutation.Message);
        result.Mutation = mutation;
        return true;
    }
    Succeed(result, mutation.DestinationPath, meta.ID, true);
    result.Mutation = MoveTemp(mutation);
    return false;
}

bool AssetSaveService::AddObjectToAsset(const StringView& path, const StringView& stableKey, const StringView& typeName,
    const StringView& name, const StringAnsiView& dataJson, int64& localId, AssetSaveResult& result)
{
    AuthoredSourceDocument document;
    AssetMeta meta;
    String sourceHash;
    String metaHash;
    String error;
    if (ReadAuthored(path, document, meta, sourceHash, metaHash, error) ||
        document.AddObject(stableKey, typeName, name, dataJson, localId, error) || SynchronizeMeta(document, meta, error))
        return Fail(result, path, error);
    StringAnsi sourceJson;
    if (Canonicalize(document, sourceJson, error))
        return Fail(result, path, error);
    AssetMutationResult mutation;
    if (_mutations.ReplaceAsset(path, sourceJson, meta, mutation))
    {
        Fail(result, path, mutation.Message);
        result.Mutation = mutation;
        return true;
    }
    Succeed(result, mutation.SourcePath, meta.ID, true);
    result.Mutation = MoveTemp(mutation);
    return false;
}

bool AssetSaveService::RemoveObjectFromAsset(const StringView& path, int64 localId, AssetSaveResult& result)
{
    AuthoredSourceDocument document;
    AssetMeta meta;
    String sourceHash;
    String metaHash;
    String error;
    if (ReadAuthored(path, document, meta, sourceHash, metaHash, error) || document.RemoveObject(localId, error) ||
        SynchronizeMeta(document, meta, error))
        return Fail(result, path, error);
    StringAnsi sourceJson;
    if (Canonicalize(document, sourceJson, error))
        return Fail(result, path, error);
    AssetMutationResult mutation;
    if (_mutations.ReplaceAsset(path, sourceJson, meta, mutation))
    {
        Fail(result, path, mutation.Message);
        result.Mutation = mutation;
        return true;
    }
    Succeed(result, mutation.SourcePath, meta.ID, true);
    result.Mutation = MoveTemp(mutation);
    return false;
}

bool AssetSaveService::SetMainObject(const StringView& path, int64 localId, AssetSaveResult& result)
{
    AuthoredSourceDocument document;
    AssetMeta meta;
    String sourceHash;
    String metaHash;
    String error;
    if (ReadAuthored(path, document, meta, sourceHash, metaHash, error) || document.SetMainObject(localId, error) ||
        SynchronizeMeta(document, meta, error))
        return Fail(result, path, error);
    StringAnsi sourceJson;
    if (Canonicalize(document, sourceJson, error))
        return Fail(result, path, error);
    AssetMutationResult mutation;
    if (_mutations.ReplaceAsset(path, sourceJson, meta, mutation))
    {
        Fail(result, path, mutation.Message);
        result.Mutation = mutation;
        return true;
    }
    Succeed(result, mutation.SourcePath, meta.ID, true);
    result.Mutation = MoveTemp(mutation);
    return false;
}

bool AssetSaveService::StageObjectData(const StringView& path, int64 localId, const StringAnsiView& dataJson,
    const StringView& reason, AssetSaveResult& result)
{
    const String key = PathKey(path);
    ScopeLock lock(_locker);
    PendingDocument* pending = _dirty.TryGet(key);
    if (!pending)
    {
        PendingDocument value;
        String error;
        if (ReadAuthored(path, value.Document, value.Meta, value.State.BaseSourceHash, value.BaseMetaHash, error))
            return Fail(result, path, error);
        value.State.AssetID = value.Meta.ID;
        _dirty.Add(key, MoveTemp(value));
        pending = _dirty.TryGet(key);
    }
    String error;
    if (pending->Document.SetObjectData(localId, dataJson, error))
        return Fail(result, path, error);
    pending->State.EditRevision++;
    if (!pending->State.DirtyObjects.Contains(localId))
        pending->State.DirtyObjects.Add(localId);
    if (reason.HasChars() && !pending->State.Reasons.Contains(String(reason)))
        pending->State.Reasons.Add(String(reason));
    Succeed(result, path, pending->State.AssetID, false, pending->State.EditRevision);
    return false;
}

bool AssetSaveService::IsDirty(const StringView& path, DirtyAuthoredSource* state) const
{
    ScopeLock lock(_locker);
    const PendingDocument* pending = _dirty.TryGet(PathKey(path));
    if (!pending)
        return false;
    if (state)
        *state = pending->State;
    return true;
}

void AssetSaveService::GetDirtyPaths(Array<String>& paths) const
{
    paths.Clear();
    ScopeLock lock(_locker);
    for (const auto& item : _dirty)
        paths.Add(item.Key);
}

bool AssetSaveService::SaveAssetIfDirty(const StringView& path, AssetSaveResult& result)
{
    const String key = PathKey(path);
    PendingDocument snapshot;
    {
        ScopeLock lock(_locker);
        const PendingDocument* pending = _dirty.TryGet(key);
        if (!pending)
        {
            Succeed(result, path, Guid::Empty, false);
            return false;
        }
        snapshot = *pending;
    }
    Array<byte> sourceBytes;
    Array<byte> metaBytes;
    String currentSourceHash;
    String currentMetaHash;
    if (ReadAndHash(path, sourceBytes, currentSourceHash) || ReadAndHash(String(path) + TEXT(".meta"), metaBytes, currentMetaHash))
        return Fail(result, path, TEXT("Dirty authored source pair cannot be reread before save."));
    if (currentSourceHash != snapshot.State.BaseSourceHash || currentMetaHash != snapshot.BaseMetaHash)
        return Fail(result, path, TEXT("Authored source changed externally while dirty; save was not written."), true);
    String error;
    if (SynchronizeMeta(snapshot.Document, snapshot.Meta, error))
        return Fail(result, path, error);
    StringAnsi sourceJson;
    if (Canonicalize(snapshot.Document, sourceJson, error))
        return Fail(result, path, error);
    AssetMutationResult mutation;
    if (_mutations.ReplaceAsset(path, sourceJson, snapshot.Meta, mutation))
    {
        Fail(result, path, mutation.Message);
        result.Mutation = mutation;
        return true;
    }
    {
        ScopeLock lock(_locker);
        const PendingDocument* current = _dirty.TryGet(key);
        if (current && current->State.EditRevision == snapshot.State.EditRevision)
            _dirty.Remove(key);
    }
    Succeed(result, mutation.SourcePath, snapshot.State.AssetID, true, snapshot.State.EditRevision);
    result.Mutation = MoveTemp(mutation);
    return false;
}

bool AssetSaveService::SaveAssets(const Array<String>& paths, Array<AssetSaveResult>& results)
{
    results.Clear();
    Array<String> selected = paths;
    if (selected.IsEmpty())
    {
        ScopeLock lock(_locker);
        for (const auto& item : _dirty)
            selected.Add(item.Key);
    }
    bool failed = false;
    for (const String& path : selected)
    {
        AssetSaveResult result;
        failed |= SaveAssetIfDirty(path, result);
        results.Add(MoveTemp(result));
    }
    return failed;
}

bool AssetSaveService::ForceReserialize(const StringView& path, bool includeMetadata, AssetSaveResult& result)
{
    AuthoredSourceDocument document;
    AssetMeta meta;
    String sourceHash;
    String metaHash;
    String error;
    if (ReadAuthored(path, document, meta, sourceHash, metaHash, error))
        return Fail(result, path, error);
    if (includeMetadata && SynchronizeMeta(document, meta, error))
        return Fail(result, path, error);
    StringAnsi sourceJson;
    if (Canonicalize(document, sourceJson, error))
        return Fail(result, path, error);
    const ContentHash canonicalHash = ContentHash::Compute(sourceJson.Get(), sourceJson.Length());
    if (String(canonicalHash.ToString()) == sourceHash && !includeMetadata)
    {
        Succeed(result, path, meta.ID, false);
        return false;
    }
    AssetMutationResult mutation;
    const bool mutationFailed = includeMetadata
        ? _mutations.ReplaceAsset(path, sourceJson, meta, mutation)
        : _mutations.ReplaceContents(path, StringAnsiView(sourceJson), mutation);
    if (mutationFailed)
    {
        Fail(result, path, mutation.Message);
        result.Mutation = mutation;
        return true;
    }
    Succeed(result, mutation.SourcePath, meta.ID, true);
    result.Mutation = MoveTemp(mutation);
    return false;
}
