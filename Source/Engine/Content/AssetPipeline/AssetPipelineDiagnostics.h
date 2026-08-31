// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

/// <summary>
/// Stable new asset pipeline diagnostic codes. Messages are explanatory and may change; codes do not.
/// </summary>
API_ENUM() enum class AssetPipelineDiagnosticCode
{
    None,
    InvalidSettingsCombination,
    DuplicateGuid,
    MissingMeta,
    MetaParseError,
    MetaUpgradeRequired,
    InvalidMeta,
    PathCollision,
    SourceMissing,
    SourceBusy,
    ProcessorMissing,
    SubAssetReconcileRequired,
    LibraryPathInvalid,
    LibraryCreationFailed,
    BuildFailed,
    BuildCycle,
    ArtifactMissing,
    ArtifactInvalid,
    ArtifactRebuildRequired,
    SnapshotInvalid,
    MigrationFailed,
    PrepareInvalidated,
    UndeclaredInput,
    BuildCancelled,
    ResourceLimitExceeded,
    ArtifactLockBusy,
    ArtifactIncompatible,
    FutureMetaVersion,
};

/// <summary>
/// New asset pipeline diagnostic severity.
/// </summary>
API_ENUM() enum class AssetPipelineDiagnosticSeverity
{
    Info,
    Warning,
    Error,
};

/// <summary>
/// New asset pipeline stage that produced a diagnostic.
/// </summary>
API_ENUM() enum class AssetPipelineDiagnosticStage
{
    Configuration,
    DatabaseScan,
    Prepare,
    Build,
    Publication,
    Resolution,
    Cook,
    Migration,
};

/// <summary>
/// Optional source location associated with an asset pipeline diagnostic.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AssetPipelineDiagnosticLocation
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetPipelineDiagnosticLocation);

    API_FIELD() String File;
    API_FIELD() int32 Line = -1;
    API_FIELD() int32 Column = -1;
    API_FIELD() String GraphNode;
    API_FIELD() String GraphPin;
};

/// <summary>
/// Structured new asset pipeline diagnostic record.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AssetPipelineDiagnostic
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetPipelineDiagnostic);

    API_FIELD() int32 SchemaVersion = 1;
    API_FIELD() AssetPipelineDiagnosticCode Code = AssetPipelineDiagnosticCode::None;
    API_FIELD() AssetPipelineDiagnosticSeverity Severity = AssetPipelineDiagnosticSeverity::Error;
    API_FIELD() AssetPipelineDiagnosticStage Stage = AssetPipelineDiagnosticStage::Configuration;
    API_FIELD() Guid AssetGuid = Guid::Empty;
    API_FIELD() String SourcePath;
    API_FIELD() String ProcessorId;
    API_FIELD() String Target;
    API_FIELD() String OutputKind;
    API_FIELD() AssetPipelineDiagnosticLocation Location;
    API_FIELD() String Message;
    API_FIELD() String Remediation;
    Array<String> Related;
};

/// <summary>
/// Converts a diagnostic code to its stable, non-localized ASSET_* token.
/// </summary>
FLAXENGINE_API const Char* GetAssetPipelineDiagnosticCodeName(AssetPipelineDiagnosticCode code);
