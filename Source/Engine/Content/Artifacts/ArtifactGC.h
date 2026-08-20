// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactManifest.h"
#include "Engine/Content/Build/PreparedAsset.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/Types/TimeSpan.h"

using ArtifactGCSpaceQuery = Function<bool(const StringView&, uint64&)>;

struct FLAXENGINE_API ArtifactGCOptions
{
    uint64 DiskQuotaBytes = MAX_uint64;
    uint64 MinimumFreeBytes = 0;
    TimeSpan GracePeriod = TimeSpan::FromHours(24);
    int32 MaximumDeletes = 256;
    uint64 MaximumDeleteBytes = 1024ull * 1024ull * 1024ull;
    AssetCancellationToken Cancellation;
    ArtifactGCSpaceQuery QueryFreeBytes;
    bool DryRun = false;
};

struct FLAXENGINE_API ArtifactGCResult
{
    uint64 TotalArtifactBytes = 0;
    uint64 ReachableBytes = 0;
    uint64 CandidateBytes = 0;
    uint64 ReclaimedBytes = 0;
    int32 ScannedFiles = 0;
    int32 ReachableFiles = 0;
    int32 LeasedFiles = 0;
    int32 CandidateFiles = 0;
    int32 DeletedFiles = 0;
    bool PressureDetected = false;
    bool WasCancelled = false;
    bool BlockedByInvalidManifest = false;
    Array<String> DeletedPaths;
    Array<AssetPipelineDiagnostic> Diagnostics;
};

/// <summary>Reachability, grace, lease, quota, and free-space aware immutable artifact collector.</summary>
class FLAXENGINE_API ArtifactGC
{
public:
    /// <summary>Runs one rate-limited collection pass. Returns true on infrastructure failure.</summary>
    static bool Run(const StringView& libraryRoot, const ArtifactGCOptions& options, ArtifactGCResult& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Rejects a build before staging when declared capacity would violate safe disk floors.</summary>
    static bool CheckBuildCapacity(const StringView& libraryRoot, uint64 estimatedAdditionalBytes, const ArtifactGCOptions& options, AssetPipelineDiagnostic& diagnostic);
};
