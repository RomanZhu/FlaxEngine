// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetBuildService.h"
#include "Engine/Content/Build/AssetProcessor.h"

class AssetImportContext;

enum class AssetImporterFallback : byte
{
    None,
    Text,
    Binary,
};
using AssetImporterSourcePredicate = Function<bool(const StringView&)>;
using AssetImporterCallback = Function<bool(AssetImportContext&, AssetPipelineDiagnostic&)>;
using AssetImporterBuildRequest = Function<bool(const Guid&, bool, const Guid&, uint32, AssetPipelineDiagnostic&)>;
using AssetImporterBuildStatus = Function<AssetBuildJobStatus(const Guid&, AssetPipelineDiagnostic&)>;

/// <summary>Public importer registration contract layered over artifact processor execution.</summary>
struct FLAXENGINE_API AssetImporterDescriptor
{
    String ID;
    String ProviderID;
    uint32 ImporterVersion = 1;
    uint32 SettingsSchemaVersion = 1;
    ContentHash ImplementationHash;
    AssetProcessorProviderKind ProviderKind = AssetProcessorProviderKind::Native;
    Array<String> Extensions;
    int32 Priority = 0;
    bool SupportsOverride = true;
    bool ProducesMainObject = true;
    bool ProducesSubObjects = false;
    bool SupportsParallelImport = true;
    bool ProcessSafe = false;
    bool RequiresMainThread = false;
    /// <summary>
    /// Dedicated protocol-compatible executable for a process-safe third-party native importer.
    /// Managed importers leave this empty and use the restricted editor worker host.
    /// </summary>
    String WorkerExecutable;
    uint64 MaximumMemoryBytes = 1024ull * 1024ull * 1024ull;
    uint64 MaximumOutputBytes = 4ull * 1024ull * 1024ull * 1024ull;
    int32 MaximumOutputFiles = 4096;
    uint32 ImportTimeoutMilliseconds = 300000;
    AssetImporterFallback Fallback = AssetImporterFallback::None;
    AssetImporterSourcePredicate MatchesSource;
    AssetImporterCallback Import;
    AssetImporterBuildRequest RequestBuild;
    AssetImporterBuildStatus GetBuildStatus;
    AssetProcessorDescriptor Processor;

    /// <summary>Creates the public importer contract for one private build-stage implementation.</summary>
    static AssetImporterDescriptor FromBuildImplementation(const AssetProcessorDescriptor& processor, int32 priority = 0);
};
