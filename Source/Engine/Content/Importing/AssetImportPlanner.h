// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImporterRegistry.h"
#include "Engine/Content/AssetDatabase/Identity/AssetGuid.h"

struct FLAXENGINE_API AssetImportPlanRequest
{
    AssetGuid Asset;
    Guid RefreshId = Guid::Empty;
    uint32 Pass = 0;
    String SourcePath;
    String ExplicitImporterID;
    String Reason;
    ArtifactTarget Target;
    uint64 SourceRevision = 0;
    ContentHash SourceHash;
    ContentHash MetadataHash;
    ContentHash EffectivePostprocessorHash;
    ContentHash ProjectSettingsHash;
    uint32 EngineSerializationVersion = 1;
    uint32 ArtifactSchemaVersion = 1;
    bool PreferTextFallback = false;
    bool Force = false;
};

struct FLAXENGINE_API AssetImportPlan
{
    AssetImportPlanRequest Request;
    AssetImporterDescriptor Importer;
    std::shared_ptr<AssetImporterLease> ImporterLease;
    uint64 ImporterRegistryGeneration = 0;
    ArtifactKey StaticFingerprint;
    Array<ArtifactKeyComponent> KeyComponents;
};

/// <summary>Coalesces refresh requests and creates deterministic importer plans.</summary>
class FLAXENGINE_API AssetImportPlanner
{
    AssetImporterRegistry& _registry;

public:
    explicit AssetImportPlanner(AssetImporterRegistry& registry)
        : _registry(registry)
    {
    }

    bool Build(const Array<AssetImportPlanRequest>& requests, Array<AssetImportPlan>& plans, AssetPipelineDiagnostic& diagnostic);
};
