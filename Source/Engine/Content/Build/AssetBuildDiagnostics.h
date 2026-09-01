// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

struct FLAXENGINE_API AssetKeyDifference
{
    StringAnsi Name;
    StringAnsi Type;
    StringAnsi PreviousValue;
    StringAnsi CurrentValue;
};

/// <summary>Stable diagnostic JSON and typed key-difference helpers.</summary>
class FLAXENGINE_API AssetBuildDiagnostics
{
public:
    static bool DiagnosticToJson(const AssetPipelineDiagnostic& diagnostic, StringAnsi& json, AssetPipelineDiagnostic& error);
    static bool DiagnosticFromJson(const StringAnsiView& json, AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnostic& error);
    static void DiffKeyComponents(const Array<ArtifactKeyComponent>& previous, const Array<ArtifactKeyComponent>& current, Array<AssetKeyDifference>& differences);
    static StringAnsi RedactAbsolutePath(const StringAnsiView& value);
};

struct FLAXENGINE_API AssetBuildMetrics
{
    uint64 Requests = 0;
    uint64 DeduplicationHits = 0;
    uint64 BuildsStarted = 0;
    uint64 PublicationsStarted = 0;
    uint64 Succeeded = 0;
    uint64 Failed = 0;
    uint64 Cancelled = 0;
    uint64 QueueWaitMilliseconds = 0;
    uint64 MaximumQueueWaitMilliseconds = 0;
    uint64 BuildMilliseconds = 0;
    uint64 PublicationMilliseconds = 0;
    uint64 ActiveMemoryBytes = 0;
    uint64 PeakMemoryBytes = 0;
    int32 QueuedJobs = 0;
    int32 PeakQueuedJobs = 0;
    int32 ActiveExternalTools = 0;
    int32 PeakExternalTools = 0;
    int32 ActiveWorkers = 0;
    int32 PeakWorkers = 0;
    int32 PeakProcessorConcurrency = 0;
};

struct FLAXENGINE_API AssetBuildJobSummary
{
    Guid JobID = Guid::Empty;
    Guid AssetID = Guid::Empty;
    Guid RefreshId = Guid::Empty;
    uint32 Pass = 0;
    ArtifactKey Key;
    String ProcessorID;
    String Target;
    String RebuildReason;
    byte Status = 0;
    AssetPipelineDiagnostic Diagnostic;
};
