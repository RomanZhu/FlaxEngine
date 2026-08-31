// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetPipelineDiagnostics.h"

const Char* GetAssetPipelineDiagnosticCodeName(AssetPipelineDiagnosticCode code)
{
    switch (code)
    {
    case AssetPipelineDiagnosticCode::None:
        return TEXT("ASSET_NONE");
    case AssetPipelineDiagnosticCode::InvalidSettingsCombination:
        return TEXT("ASSET_INVALID_SETTINGS_COMBINATION");
    case AssetPipelineDiagnosticCode::DuplicateGuid:
        return TEXT("ASSET_DUPLICATE_GUID");
    case AssetPipelineDiagnosticCode::MissingMeta:
        return TEXT("ASSET_META_MISSING");
    case AssetPipelineDiagnosticCode::MetaParseError:
        return TEXT("ASSET_META_PARSE_ERROR");
    case AssetPipelineDiagnosticCode::MetaUpgradeRequired:
        return TEXT("ASSET_META_UPGRADE_REQUIRED");
    case AssetPipelineDiagnosticCode::InvalidMeta:
        return TEXT("ASSET_INVALID_META");
    case AssetPipelineDiagnosticCode::PathCollision:
        return TEXT("ASSET_PATH_COLLISION");
    case AssetPipelineDiagnosticCode::SourceMissing:
        return TEXT("ASSET_SOURCE_MISSING");
    case AssetPipelineDiagnosticCode::SourceBusy:
        return TEXT("ASSET_SOURCE_BUSY");
    case AssetPipelineDiagnosticCode::ProcessorMissing:
        return TEXT("ASSET_PROCESSOR_MISSING");
    case AssetPipelineDiagnosticCode::SubAssetReconcileRequired:
        return TEXT("ASSET_SUBASSET_RECONCILE_REQUIRED");
    case AssetPipelineDiagnosticCode::LibraryPathInvalid:
        return TEXT("ASSET_LIBRARY_PATH_INVALID");
    case AssetPipelineDiagnosticCode::LibraryCreationFailed:
        return TEXT("ASSET_LIBRARY_CREATION_FAILED");
    case AssetPipelineDiagnosticCode::BuildFailed:
        return TEXT("ASSET_BUILD_FAILED");
    case AssetPipelineDiagnosticCode::BuildCycle:
        return TEXT("ASSET_BUILD_CYCLE");
    case AssetPipelineDiagnosticCode::ArtifactMissing:
        return TEXT("ASSET_ARTIFACT_MISSING");
    case AssetPipelineDiagnosticCode::ArtifactInvalid:
        return TEXT("ASSET_ARTIFACT_INVALID");
    case AssetPipelineDiagnosticCode::ArtifactRebuildRequired:
        return TEXT("ASSET_ARTIFACT_REBUILD_REQUIRED");
    case AssetPipelineDiagnosticCode::SnapshotInvalid:
        return TEXT("ASSET_SNAPSHOT_INVALID");
    case AssetPipelineDiagnosticCode::MigrationFailed:
        return TEXT("ASSET_MIGRATION_FAILED");
    case AssetPipelineDiagnosticCode::PrepareInvalidated:
        return TEXT("ASSET_PREPARE_INVALIDATED");
    case AssetPipelineDiagnosticCode::UndeclaredInput:
        return TEXT("ASSET_UNDECLARED_INPUT");
    case AssetPipelineDiagnosticCode::BuildCancelled:
        return TEXT("ASSET_BUILD_CANCELLED");
    case AssetPipelineDiagnosticCode::ResourceLimitExceeded:
        return TEXT("ASSET_RESOURCE_LIMIT_EXCEEDED");
    case AssetPipelineDiagnosticCode::ArtifactLockBusy:
        return TEXT("ASSET_ARTIFACT_LOCK_BUSY");
    case AssetPipelineDiagnosticCode::ArtifactIncompatible:
        return TEXT("ASSET_ARTIFACT_INCOMPATIBLE");
    case AssetPipelineDiagnosticCode::FutureMetaVersion:
        return TEXT("ASSET_FUTURE_META_VERSION");
    case AssetPipelineDiagnosticCode::RecoveryRequired:
        return TEXT("ASSET_RECOVERY_REQUIRED");
    default:
        return TEXT("ASSET_UNKNOWN_DIAGNOSTIC");
    }
}
