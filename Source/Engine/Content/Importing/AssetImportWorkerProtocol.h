// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImportContext.h"
#include "AssetImporter.h"
#include "Engine/Content/Build/PreparedAsset.h"

enum class AssetImportWorkerStatus : byte
{
    Succeeded,
    Failed,
    Cancelled,
    Crashed,
    TimedOut,
    Rejected,
};

struct FLAXENGINE_API AssetImportWorkerResourceLimits
{
    uint64 MaximumInputBytes = 1024ull * 1024ull * 1024ull;
    uint64 MaximumOutputBytes = 4ull * 1024ull * 1024ull * 1024ull;
    uint64 MaximumMemoryBytes = 1024ull * 1024ull * 1024ull;
    int32 MaximumOutputFiles = 4096;
    uint32 TimeoutMilliseconds = 300000;
};

struct FLAXENGINE_API AssetImportWorkerInput
{
    String Identity;
    String CanonicalPath;
    ContentHash Hash;
    Array<byte> Snapshot;
};

struct FLAXENGINE_API AssetImportWorkerTool
{
    String Name;
    ContentHash VersionHash;
};

struct FLAXENGINE_API AssetImportWorkerDescriptor
{
    String ID;
    AssetProcessorProviderKind ProviderKind = AssetProcessorProviderKind::Native;
    uint32 ImporterVersion = 1;
    uint32 SettingsSchemaVersion = 1;
    ContentHash ImplementationHash;
    bool ProducesMainObject = true;
    bool ProducesSubObjects = false;
    bool PathSensitive = true;
};

/// <summary>Self-contained immutable import job passed to an isolated worker process.</summary>
struct FLAXENGINE_API AssetImportJobRequest
{
    static constexpr uint32 CurrentProtocolVersion = 2;

    uint32 ProtocolVersion = CurrentProtocolVersion;
    Guid JobID;
    Guid Capability;
    AssetGuid Asset;
    uint64 SourceRevision = 0;
    String SourcePath;
    ContentHash SourceHash;
    Array<byte> SourceSnapshot;
    ContentHash MetaHash;
    Array<byte> MetaSnapshot;
    AssetImportWorkerDescriptor Importer;
    String Target;
    Array<AssetImportWorkerInput> AuthorizedInputs;
    Array<AssetImportWorkerTool> AllowedTools;
    String OutputStagingPath;
    AssetImportWorkerResourceLimits Limits;
};

struct FLAXENGINE_API AssetImportWorkerOutput
{
    String Name;
    StringAnsi Kind;
    String RelativePath;
    ContentHash Hash;
    uint64 Size = 0;
};

/// <summary>Untrusted worker response draft. The parent validates this before any publication.</summary>
struct FLAXENGINE_API AssetImportJobResult
{
    uint32 ProtocolVersion = AssetImportJobRequest::CurrentProtocolVersion;
    Guid JobID;
    Guid Capability;
    AssetImportWorkerStatus Status = AssetImportWorkerStatus::Failed;
    Array<AssetPipelineDiagnostic> Diagnostics;
    Array<AssetImportedObjectDeclaration> Objects;
    Array<AssetImportDependency> Dependencies;
    Array<AssetImportWorkerOutput> Outputs;
    StringAnsi OutputManifestDraft;
    Array<AssetImportWorkerTool> ObservedToolchain;
    uint64 PeakMemory = 0;
};

class FLAXENGINE_API AssetImportWorkerProtocol
{
public:
    static bool SaveRequest(const StringView& path, const AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic);
    static bool LoadRequest(const StringView& path, AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic);
    static bool SaveResult(const StringView& path, const AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic);
    static bool LoadResult(const StringView& path, AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic);
    static bool ValidateRequest(const AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic);
    static bool ValidateResult(const AssetImportJobRequest& request, AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Strict child-process runner. It never publishes artifacts or writes the asset database.</summary>
class FLAXENGINE_API AssetImportWorkerProcess
{
public:
    static bool Run(const StringView& executable, const AssetImportJobRequest& request,
                    const AssetCancellationToken& cancellation, AssetImportJobResult& result,
                    AssetPipelineDiagnostic& diagnostic);
};

using AssetImportWorkerAction = Function<bool(const AssetImportJobRequest&, AssetImportJobResult&, AssetPipelineDiagnostic&)>;

/// <summary>Worker-side protocol entry point for native importer executables.</summary>
class FLAXENGINE_API AssetImportWorkerHost
{
public:
    static int32 Run(const StringView& requestPath, const StringView& resultPath, const StringView& capability,
                     AssetImportWorkerAction action);
};
