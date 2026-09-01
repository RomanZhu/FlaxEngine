// Copyright (c) Wojciech Figat. All rights reserved.

#include "ValidateStep.h"
#include "Engine/Core/Config/GameSettings.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    bool IsBlockingRecordStatus(AssetRecordStatus status)
    {
        switch (status)
        {
        case AssetRecordStatus::DuplicateGuid:
        case AssetRecordStatus::UnsupportedProcessor:
        case AssetRecordStatus::MetaUpgradeRequired:
        case AssetRecordStatus::SubAssetReconciliationRequired:
        case AssetRecordStatus::MissingMeta:
        case AssetRecordStatus::MalformedMeta:
        case AssetRecordStatus::OrphanMeta:
        case AssetRecordStatus::PathCollision:
            return true;
        default:
            return false;
        }
    }
}

bool ValidateStep::Perform(CookingData& data)
{
    data.StepProgress(TEXT("Performing validation"), 0);

    // Ensure output and cache directories exist
    if (!FileSystem::DirectoryExists(data.NativeCodeOutputPath) && FileSystem::CreateDirectory(data.NativeCodeOutputPath))
    {
        data.Error(TEXT("Failed to create build output directory."));
        return true;
    }
    if (!FileSystem::DirectoryExists(data.ManagedCodeOutputPath) && FileSystem::CreateDirectory(data.ManagedCodeOutputPath))
    {
        data.Error(TEXT("Failed to create build output directory."));
        return true;
    }
    if (!FileSystem::DirectoryExists(data.DataOutputPath) && FileSystem::CreateDirectory(data.DataOutputPath))
    {
        data.Error(TEXT("Failed to create build output directory."));
        return true;
    }
    if (!FileSystem::DirectoryExists(data.CacheDirectory) && FileSystem::CreateDirectory(data.CacheDirectory))
    {
        data.Error(TEXT("Failed to create build cache directory."));
        return true;
    }

#if OFFICIAL_BUILD
    // Validate that platform data is installed
    if (!FileSystem::DirectoryExists(data.GetGameBinariesPath()))
    {
        data.Error(TEXT("Missing platform data tools for the target platform. Use Flax Launcher and download the required package."));
        return true;
    }
#endif

    // Load game settings (may be modified via editor)
    if (GameSettings::Load())
    {
        data.Error(TEXT("Failed to load game settings."));
        return true;
    }
    // Validate game settings
    auto gameSettings = GameSettings::Get();
    if (gameSettings == nullptr)
    {
        data.Error(TEXT("Missing game settings."));
        return true;
    }
    if (gameSettings->ProductName.IsEmpty())
    {
        data.Error(TEXT("Missing product name."));
        return true;
    }

    if (gameSettings->CompanyName.IsEmpty())
    {
        data.Error(TEXT("Missing company name."));
        return true;
    }

    if (!gameSettings->FirstScene.ID.IsValid())
    {
        data.Error(TEXT("Missing first scene. Set it in the game settings."));
        return true;
    }

    data.StepProgress(TEXT("Validating asset metadata"), 0.5f);
    if (AssetPipelineService::Scan(true))
    {
        data.Error(TEXT("Canonical asset metadata scan failed."));
        return true;
    }
    const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
    for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
        {
            data.Error(String::Format(TEXT("Asset metadata validation failed: {0}"), diagnostic.Message));
            return true;
        }
    }

    // Publish settings artifacts before freezing the cooker snapshot so dependency
    // discovery consumes only processor-recorded composite runtime references.
    const AssetDatabaseSnapshot settingsSnapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : settingsSnapshot.Records)
    {
        if (record.IsMainAsset() && record.ProcessorID == TEXT("Flax.Settings") &&
            AssetPipelineService::BuildAsset(record.ID, false, true))
        {
            const Array<AssetPipelineDiagnostic> buildDiagnostics = AssetDatabaseQueryService::GetDiagnostics();
            const String message = buildDiagnostics.HasItems() ? buildDiagnostics.Last().Message : TEXT("Unknown settings artifact build error.");
            data.Error(String::Format(TEXT("Failed to build settings artifact {0}: {1}"), record.ID, message));
            return true;
        }
    }
    gameSettings = GameSettings::Get();
    if (gameSettings == nullptr)
    {
        data.Error(TEXT("Game settings became unavailable after publishing settings artifacts."));
        return true;
    }
    data.DatabaseSnapshot = AssetDatabase::Get().GetSnapshot();
    const AssetDatabaseSnapshot& snapshot = data.DatabaseSnapshot;
    LOG(Info, "Cook asset database revision {0}, records {1}", snapshot.Revision, snapshot.Records.Count());
    if (snapshot.Revision == 0)
    {
        data.Error(TEXT("Cannot cook without a versioned asset database snapshot."));
        return true;
    }

    const Guid gameSettingsObject = GameSettings::GetGameSettingsObjectId();
    if (!gameSettingsObject.IsValid())
    {
        data.Error(TEXT("The project settings index does not resolve a GameSettings object."));
        return true;
    }
    bool foundGameSettings = false;
    AssetRecord gameSettingsRecord;
    AssetRecord firstSceneRecord;
    bool foundFirstScene = false;
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.ID == gameSettingsObject)
        {
            if (!record.IsMainAsset() || record.TypeName != JsonAsset::TypeName || record.ProcessorID != TEXT("Flax.Settings"))
            {
                data.Error(TEXT("The project settings index GameSettings reference is not a Flax.Settings JsonAsset."));
                return true;
            }
            gameSettingsRecord = record;
            foundGameSettings = true;
        }
        if (record.ID == gameSettings->FirstScene.ID)
        {
            firstSceneRecord = record;
            foundFirstScene = true;
        }
    }
    if (!foundGameSettings)
    {
        data.Error(TEXT("The runtime game settings object is not registered in the frozen asset database."));
        return true;
    }
    if (!foundFirstScene || !firstSceneRecord.IsMainAsset() || firstSceneRecord.TypeName != TEXT("FlaxEngine.SceneAsset"))
    {
        data.Error(String::Format(TEXT("The first scene {0} must resolve to a registered main SceneAsset object (found={1}, type={2})."),
            gameSettings->FirstScene.ID, foundFirstScene, firstSceneRecord.TypeName));
        return true;
    }
    data.AddRootAsset(AssetObjectId(AssetGuid(gameSettingsRecord.SourceAssetID), gameSettingsRecord.LocalId));
    data.AddRootAsset(AssetObjectId(AssetGuid(firstSceneRecord.SourceAssetID), firstSceneRecord.LocalId));

    for (const AssetRecord& record : snapshot.Records)
    {
        if (IsBlockingRecordStatus(record.Status))
        {
            data.Error(String::Format(TEXT("Canonical record {0} is not cookable (status {1})."), record.ID, static_cast<int32>(record.Status)));
            return true;
        }
    }
    return false;
}
