// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineBootstrap.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Deterministic, read-only project migration and hard-cut validation result.</summary>
API_STRUCT() struct FLAXENGINE_API AssetProjectValidationResult
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetProjectValidationResult);

    API_FIELD() bool Valid = false;
    API_FIELD() bool RequiresMigration = false;
    API_FIELD() bool ReadOnly = false;
    API_FIELD() int32 SourceFiles = 0;
    API_FIELD() int32 SourceFolders = 0;
    API_FIELD() int32 MetadataFiles = 0;
    API_FIELD() String SourceTreeFingerprint;
    API_FIELD() String ReportJson;
    API_FIELD() AssetPipelineBootstrapSnapshot Bootstrap;
    API_FIELD() Array<AssetPipelineDiagnostic> Diagnostics;
};

/// <summary>Read-only preflight used before migration commit and by headless validation.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetProjectValidator
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetProjectValidator);

public:
    /// <summary>Validates the project marker, canonical settings role, source tree, and adjacent identities without writes.</summary>
    API_FUNCTION() static AssetProjectValidationResult Validate(const StringView& projectDescriptorPath, const StringView& contentRoot);
};
