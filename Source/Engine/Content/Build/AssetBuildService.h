// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetBuildDiagnostics.h"
#include "PreparedAsset.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/NonCopyable.h"
#include <memory>

/// <summary>Stable identity for one exact shared build plan.</summary>
struct FLAXENGINE_API AssetBuildJobKey
{
    ArtifactKey ExactPlan;

    bool operator==(const AssetBuildJobKey& other) const
    {
        return ExactPlan == other.ExactPlan;
    }

    bool IsValid() const
    {
        return !ExactPlan.IsZero();
    }
};

inline uint32 GetHash(const AssetBuildJobKey& key)
{
    return GetHash(key.ExactPlan);
}

enum class AssetBuildJobStatus : byte
{
    Invalid,
    Queued,
    Building,
    Publishing,
    Succeeded,
    Failed,
    Cancelled,
};

struct FLAXENGINE_API AssetBuildServiceLimits
{
    int32 MaximumWorkers = 1;
    uint64 MaximumMemoryBytes = 1024ull * 1024ull * 1024ull;
    int32 MaximumExternalTools = 1;
    int32 MaximumLogFiles = 512;
};

using AssetBuildJobAction = Function<bool(const AssetCancellationToken&, AssetPipelineDiagnostic&)>;

/// <summary>One staged build request. Publish is invoked only through the shutdown-safe publication gate.</summary>
struct FLAXENGINE_API AssetBuildJobRequest
{
    AssetBuildJobKey Key;
    Guid AssetID = Guid::Empty;
    String ProcessorClass;
    String ProcessorID;
    String Target;
    Array<StringAnsi> OutputKinds;
    Array<ArtifactKeyComponent> KeyComponents;
    String RebuildReason;
    uint64 MemoryBytes = 0;
    int32 ExternalToolSlots = 0;
    int32 ProcessorConcurrencyLimit = MAX_int32;
    // Disable when a repeated exact plan must run publication again because its mutable manifest may now point at another plan.
    bool AllowTerminalDeduplication = true;
    Array<AssetBuildJobKey> Dependencies;
    AssetBuildJobAction Build;
    AssetBuildJobAction Publish;
};

struct FLAXENGINE_API AssetBuildJobResult
{
    AssetBuildJobKey Key;
    Guid AssetID = Guid::Empty;
    Guid JobID = Guid::Empty;
    AssetBuildJobStatus Status = AssetBuildJobStatus::Invalid;
    AssetPipelineDiagnostic Diagnostic;
};

struct AssetBuildSharedState;
class AssetBuildService;

/// <summary>One requester view of a shared exact build job.</summary>
class FLAXENGINE_API AssetBuildRequestHandle
{
    friend AssetBuildService;

private:
    std::shared_ptr<AssetBuildSharedState> _state;
    uint64 _requester = 0;

    AssetBuildRequestHandle(const std::shared_ptr<AssetBuildSharedState>& state, uint64 requester)
        : _state(state)
        , _requester(requester)
    {
    }

public:
    AssetBuildRequestHandle() = default;

    bool IsValid() const;
    AssetBuildJobStatus GetStatus() const;
    bool Wait(uint32 timeoutMilliseconds = MAX_uint32) const;
    bool TryGetResult(AssetBuildJobResult& result) const;
};

/// <summary>Bounded exact-job scheduler with requester-independent cancellation and safe publication shutdown.</summary>
class FLAXENGINE_API AssetBuildService : public NonCopyable
{
private:
    class Impl;
    std::unique_ptr<Impl> _impl;

public:
    AssetBuildService();
    ~AssetBuildService();

    /// <summary>Starts workers and structured job logging. Returns true on failure.</summary>
    bool Initialize(const StringView& libraryRoot, const AssetBuildServiceLimits& limits, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Shares an existing exact job or queues a new one.</summary>
    AssetBuildRequestHandle Request(const AssetBuildJobRequest& request);

    /// <summary>Cancels only this requester; shared work is cancelled after its final requester leaves.</summary>
    void CancelRequester(const AssetBuildRequestHandle& handle);

    /// <summary>Closes the publication gate, cancels outstanding work, and joins all workers.</summary>
    void Shutdown();

    bool IsRunning() const;
    AssetBuildMetrics GetMetrics() const;
    void GetJobs(Array<AssetBuildJobSummary>& jobs) const;
};
