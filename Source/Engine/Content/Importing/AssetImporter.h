// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

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

/// <summary>Public importer registration contract layered over artifact processor execution.</summary>
struct FLAXENGINE_API AssetImporterDescriptor
{
    String ID;
    uint32 ImporterVersion = 1;
    ContentHash ImplementationHash;
    Array<String> Extensions;
    int32 Priority = 0;
    bool SupportsOverride = true;
    bool ProducesMainObject = true;
    bool ProducesSubObjects = false;
    bool SupportsParallelImport = true;
    bool RequiresMainThread = false;
    bool PathSensitive = true;
    AssetImporterFallback Fallback = AssetImporterFallback::None;
    AssetImporterSourcePredicate MatchesSource;
    AssetImporterCallback Import;
    AssetProcessorDescriptor Processor;

    static AssetImporterDescriptor FromProcessor(const AssetProcessorDescriptor& processor, int32 priority = 0);
};
