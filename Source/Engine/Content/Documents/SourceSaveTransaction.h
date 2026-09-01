// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Types/String.h"

/// <summary>Whether a source save requires an existing durable registration.</summary>
enum class SourceSaveRegistrationMode : byte
{
    RequireTracked,
    AllowUnregistered,
    UntrackedLocal,
};

/// <summary>Terminal source save result.</summary>
enum class SourceSaveOutcome : byte
{
    None,
    Unchanged,
    Committed,
    ActivatedDurabilityUncertain,
    Conflict,
    Rejected,
    Failed,
};

/// <summary>Stable source-save failure-injection boundaries.</summary>
enum class SourceSaveFailurePoint : byte
{
    BeforeStagingWrite,
    AfterStagingWrite,
    BeforeReplace,
    AfterReplaceActivation,
};

/// <summary>How a save handles disk bytes that have advanced beyond durable database observation.</summary>
enum class SourceSaveConflictPolicy : byte
{
    Strict,
    AdoptCurrent,
};

/// <summary>Durable registration lookup result; failure is distinct from an authoritative miss.</summary>
enum class SourceSaveRevisionLookup : byte
{
    Found,
    NotFound,
    Failed,
};

/// <summary>Exact source identity captured before an authored save.</summary>
struct FLAXENGINE_API SourceSaveRevision
{
    Guid SourceAssetID = Guid::Empty;
    String SourcePath;
    uint64 SourceRevision = 0;
    /// <summary>Observed bytes at SourcePath.</summary>
    ContentHash SourceHash;
    /// <summary>Source hash stored by the exact durable database revision.</summary>
    ContentHash DurableSourceHash;
    bool Exists = false;
    bool IsTracked = false;
};

/// <summary>Canonical bytes and exact source revision required by one save.</summary>
struct FLAXENGINE_API SourceSaveRequest
{
    SourceSaveRegistrationMode RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
    SourceSaveConflictPolicy ConflictPolicy = SourceSaveConflictPolicy::Strict;
    SourceSaveRevision Expected;
    StringAnsi CanonicalBytes;
};

/// <summary>One exact write emitted for filesystem-journal suppression.</summary>
struct FLAXENGINE_API SourceSaveSelfWrite
{
    Guid TransactionID = Guid::Empty;
    String Path;
    ContentHash Content;
};

/// <summary>Explicit source save result, including conflict state and committed self-write.</summary>
struct FLAXENGINE_API SourceSaveResult
{
    SourceSaveOutcome Outcome = SourceSaveOutcome::None;
    Guid TransactionID = Guid::Empty;
    SourceSaveRevision Current;
    SourceSaveSelfWrite SelfWrite;
};

/// <summary>Provides durable tracked-source identity without coupling tests to the global database.</summary>
class FLAXENGINE_API ISourceSaveRevisionProvider
{
public:
    virtual ~ISourceSaveRevisionProvider() = default;

    /// <summary>Looks up the exact path and propagates durable database failures.</summary>
    virtual SourceSaveRevisionLookup LookupTrackedSource(const StringView& path, SourceSaveRevision& result,
        AssetPipelineDiagnostic& diagnostic) const = 0;
};

/// <summary>Optional mutation authorization invoked without holding the source save reservation.</summary>
class FLAXENGINE_API ISourceSaveCallback
{
public:
    virtual ~ISourceSaveCallback() = default;

    /// <returns>True to reject the save.</returns>
    virtual bool BeforeCommit(const SourceSaveRequest& request, AssetPipelineDiagnostic& diagnostic) = 0;
};

/// <summary>Optional deterministic source-save failure injection used by native tests.</summary>
class FLAXENGINE_API ISourceSaveFailureInjector
{
public:
    virtual ~ISourceSaveFailureInjector() = default;
    virtual bool ShouldFail(SourceSaveFailurePoint point) = 0;
};

/// <summary>Optimistic authored-source transaction with exact revalidation and durable atomic replace.</summary>
class FLAXENGINE_API SourceSaveTransaction
{
private:
    const ISourceSaveRevisionProvider* _revisionProvider;

public:
    explicit SourceSaveTransaction(const ISourceSaveRevisionProvider* revisionProvider = nullptr);

    /// <summary>Captures the exact tracked identity, durable revision, existence, and source hash.</summary>
    /// <returns>True on failure.</returns>
    bool Capture(const StringView& path, SourceSaveRegistrationMode registrationMode,
        SourceSaveRevision& result, AssetPipelineDiagnostic& diagnostic,
        SourceSaveConflictPolicy conflictPolicy = SourceSaveConflictPolicy::Strict) const;

    /// <summary>Commits canonical bytes if the exact captured source is still current.</summary>
    /// <returns>True on rejection, conflict, or I/O failure.</returns>
    bool Commit(const SourceSaveRequest& request, SourceSaveResult& result,
        AssetPipelineDiagnostic& diagnostic, ISourceSaveCallback* callback = nullptr,
        ISourceSaveFailureInjector* failureInjector = nullptr) const;
};
