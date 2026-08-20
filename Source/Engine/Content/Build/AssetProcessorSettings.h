// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/AssetMeta.h"

/// <summary>
/// Deterministic callback that upgrades one tracked processor-settings version to the next.
/// Returns true on failure.
/// </summary>
typedef bool (*AssetProcessorSettingsUpgradeCallback)(int32 fromVersion, const StringAnsiView& input, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);

/// <summary>Tracked settings schema contract, independent of processor implementation/output versions.</summary>
struct FLAXENGINE_API AssetProcessorSettingsSchema
{
    String ProcessorID;
    int32 CurrentVersion = 1;
    StringAnsi NormalizedDefaults = "{}\n";
    String ImplementationVersion;
    AssetProcessorSettingsUpgradeCallback Upgrade = nullptr;

    /// <summary>Applies explicit normalized defaults once during sidecar creation.</summary>
    /// <returns>True on failure.</returns>
    bool InitializeNewMeta(AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Stages deterministic tracked upgrades, or returns a blocking no-write diagnostic.</summary>
    /// <returns>True on failure.</returns>
    bool PreviewUpgrade(const AssetMeta& current, bool mayStageTrackedChanges, AssetMeta& staged, AssetPipelineDiagnostic& diagnostic) const;
};
