// Copyright (c) Wojciech Figat. All rights reserved.

#include "ValidateStep.h"
#include "Engine/Core/Config/GameSettings.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseFacade.h"
#include "Engine/Content/AssetDatabase/MigrationInventory.h"
#include "Engine/Content/AssetPipeline/AssetPipelineSettings.h"
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
    data.AddRootAsset(Globals::ProjectContentFolder / TEXT("GameSettings.json"));

    // Validate game settings
    auto gameSettings = GameSettings::Get();
    if (gameSettings == nullptr)
    {
        data.Error(TEXT("Missing game settings."));
        return true;
    }
    {
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

        AssetInfo info;
        if (!Content::GetAssetInfo(gameSettings->FirstScene, info))
        {
            data.Error(TEXT("Missing first scene. Set it in the game settings."));
            return true;
        }
    }

    const auto* pipelineSettings = AssetPipelineSettings::Get();
    if (pipelineSettings && pipelineSettings->UseNewAssetDatabase)
    {
        data.StepProgress(TEXT("Validating asset metadata"), 0.5f);
        if (AssetDatabaseFacade::Scan(true))
        {
            data.Error(TEXT("Canonical asset metadata scan failed."));
            return true;
        }
        const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseFacade::GetDiagnostics();
        for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
        {
            if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
            {
                data.Error(String::Format(TEXT("Asset metadata validation failed: {0}"), diagnostic.Message));
                return true;
            }
        }
        const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
        LOG(Info, "Cook asset database revision {0}, records {1}", snapshot.Revision, snapshot.Records.Count());
        Array<MigrationInventoryEntry> inventory;
        MigrationInventory::Build(snapshot.Records, inventory);
        if (MigrationInventory::HasBlockingConflict(inventory))
        {
            data.Error(TEXT("Mixed-mode cook refused because legacy and canonical records conflict."));
            return true;
        }
        for (const AssetRecord& record : snapshot.Records)
        {
            if (IsBlockingRecordStatus(record.Status))
            {
                data.Error(String::Format(TEXT("Canonical record {0} is not cookable (status {1})."), record.ID, static_cast<int32>(record.Status)));
                return true;
            }
        }
    }

    return false;
}
