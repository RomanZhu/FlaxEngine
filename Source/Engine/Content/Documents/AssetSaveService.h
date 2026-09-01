// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "DirtySourceRegistry.h"
#include "SourceSaveTransaction.h"

/// <summary>Whether an authored save should reconcile the tracked source database.</summary>
enum class AssetSaveRefreshMode : byte
{
    None,
    TrackedSource,
};

/// <summary>Optional importer execution after source reconciliation.</summary>
enum class AssetSaveImportMode : byte
{
    None,
    Asynchronous,
    Synchronous,
};

/// <summary>Terminal targeted-refresh state for one authored save.</summary>
enum class AssetSaveRefreshOutcome : byte
{
    NotRequired,
    Succeeded,
    Failed,
};

/// <summary>Terminal importer state for one authored save.</summary>
enum class AssetSaveImportOutcome : byte
{
    NotRequested,
    Succeeded,
    Failed,
    Blocked,
};

/// <summary>Canonical source bytes plus exact save and follow-up policy.</summary>
struct FLAXENGINE_API AssetSaveRequest
{
    String SourcePath;
    StringAnsi CanonicalBytes;
    SourceSaveRegistrationMode RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
    SourceSaveConflictPolicy ConflictPolicy = SourceSaveConflictPolicy::Strict;
    ContentHash ExpectedSourceHash;
    bool HasExpectedSourceHash = false;
    AssetSaveRefreshMode RefreshMode = AssetSaveRefreshMode::TrackedSource;
    AssetSaveImportMode ImportMode = AssetSaveImportMode::None;
    bool ForceImport = false;
    uint64 EditRevision = 0;
};

/// <summary>Independent source commit, refresh, import, and dirty-state outcomes.</summary>
struct FLAXENGINE_API AssetSaveResult
{
    SourceSaveResult Source;
    AssetSaveRefreshOutcome Refresh = AssetSaveRefreshOutcome::NotRequired;
    AssetSaveImportOutcome Import = AssetSaveImportOutcome::NotRequested;
    uint64 CommittedEditRevision = 0;
    bool DirtyCleared = false;

    bool IsSourceCommitted() const;
    bool IsSuccessful() const;
};

/// <summary>Injectable refresh/import boundary used by the save service and native tests.</summary>
class FLAXENGINE_API IAssetSavePipeline
{
public:
    virtual ~IAssetSavePipeline() = default;

    virtual bool RefreshSource(const StringView& path, AssetPipelineDiagnostic& diagnostic) = 0;
    virtual bool ImportSource(const Guid& sourceID, bool force, bool synchronous,
        AssetPipelineDiagnostic& diagnostic) = 0;
    virtual void RegisterSelfWrite(const SourceSaveSelfWrite& write) = 0;
};

/// <summary>Single native owner for authored source commit, dirty state, refresh, and import follow-up.</summary>
class FLAXENGINE_API AssetSaveService
{
private:
    const ISourceSaveRevisionProvider* _revisionProvider;
    IAssetSavePipeline* _pipeline;
    DirtySourceRegistry* _dirtyRegistry;

public:
    AssetSaveService(const ISourceSaveRevisionProvider* revisionProvider = nullptr,
        IAssetSavePipeline* pipeline = nullptr, DirtySourceRegistry* dirtyRegistry = nullptr);

    static AssetSaveService& Get();

    /// <returns>True when source commit, refresh, or requested import failed or was blocked.</returns>
    bool Save(const AssetSaveRequest& request, AssetSaveResult& result,
        AssetPipelineDiagnostic& diagnostic, ISourceSaveCallback* callback = nullptr,
        ISourceSaveFailureInjector* failureInjector = nullptr) const;
};
