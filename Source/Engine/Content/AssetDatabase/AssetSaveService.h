// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetMutationService.h"
#include "Engine/Content/Documents/AuthoredSourceDocument.h"
#include "Engine/Platform/CriticalSection.h"

/// <summary>Per-source dirty snapshot owned by the authored save service.</summary>
struct FLAXENGINE_API DirtyAuthoredSource
{
    Guid AssetID;
    uint64 EditRevision = 0;
    String BaseSourceHash;
    Array<int64> DirtyObjects;
    Array<String> Reasons;
};

/// <summary>Result of one generic authored source save or structural edit.</summary>
struct FLAXENGINE_API AssetSaveResult
{
    bool Succeeded = false;
    bool Saved = false;
    bool Conflict = false;
    Guid AssetID;
    uint64 EditRevision = 0;
    String SourcePath;
    String Message;
    AssetMutationResult Mutation;
};

/// <summary>
/// Owns generic authored document edits, dirty revisions, conflict checks, and journaled source/meta commits.
/// Imported outputs and derived artifact paths are always read-only here.
/// </summary>
class FLAXENGINE_API AssetSaveService
{
public:
    AssetSaveService(const StringView& projectRoot, const StringView& contentRoot, const StringView& journalRoot, const StringView& recoveryRoot);

    bool CreateAsset(const StringView& path, const StringView& stableKey, const StringView& typeName,
        const StringView& name, const StringAnsiView& dataJson, AssetSaveResult& result);
    bool AddObjectToAsset(const StringView& path, const StringView& stableKey, const StringView& typeName,
        const StringView& name, const StringAnsiView& dataJson, int64& localId, AssetSaveResult& result);
    bool RemoveObjectFromAsset(const StringView& path, int64 localId, AssetSaveResult& result);
    bool SetMainObject(const StringView& path, int64 localId, AssetSaveResult& result);

    /// <summary>Stages an object payload in memory and increments the source edit revision.</summary>
    bool StageObjectData(const StringView& path, int64 localId, const StringAnsiView& dataJson,
        const StringView& reason, AssetSaveResult& result);

    bool IsDirty(const StringView& path, DirtyAuthoredSource* state = nullptr) const;
    void GetDirtyPaths(Array<String>& paths) const;
    bool SaveAssetIfDirty(const StringView& path, AssetSaveResult& result);
    bool SaveAssets(const Array<String>& paths, Array<AssetSaveResult>& results);
    bool ForceReserialize(const StringView& path, bool includeMetadata, AssetSaveResult& result);

    AssetMutationService& GetMutationService() { return _mutations; }

private:
    struct PendingDocument
    {
        DirtyAuthoredSource State;
        AuthoredSourceDocument Document;
        AssetMeta Meta;
        String BaseMetaHash;
    };

    mutable CriticalSection _locker;
    Dictionary<String, PendingDocument> _dirty;
    AssetMutationService _mutations;

    static String PathKey(const StringView& path);
    bool ReadAuthored(const StringView& path, AuthoredSourceDocument& document, AssetMeta& meta,
        String& sourceHash, String& metaHash, String& error) const;
    static bool SynchronizeMeta(const AuthoredSourceDocument& document, AssetMeta& meta, String& error);
    static bool Canonicalize(const AuthoredSourceDocument& document, StringAnsi& sourceJson, String& error);
};
