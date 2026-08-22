// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetPipelineDiagnostics.h"
#include "Engine/Core/Config/Settings.h"

/// <summary>
/// Project settings and temporary rollout controls for the new asset pipeline.
/// </summary>
API_CLASS(sealed, Namespace="FlaxEditor.Content.Settings") class FLAXENGINE_API AssetPipelineSettings : public SettingsBase
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetPipelineSettings);
    API_AUTO_SERIALIZATION();

public:
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Rollout\")")
    bool UseNewAssetDatabase = false;

    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Rollout\")")
    bool UseLibraryArtifacts = false;

    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Rollout\")")
    bool UseTextGraphAssets = false;

    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Rollout\")")
    bool StrictAssetMetadata = false;

    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Rollout\")")
    bool AutoCreateMetaInEditor = false;

    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Rollout\")")
    bool AllowLastGoodArtifacts = false;

    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Rollout\")")
    bool LockConvertedTypeAuthoring = false;

public:
    API_FIELD(Attributes="EditorOrder(100), EditorDisplay(\"Library\"), Limit(1, 1048576)")
    int32 DiskQuotaGigabytes = 100;

    API_FIELD(Attributes="EditorOrder(110), EditorDisplay(\"Library\"), Limit(0, 1048576)")
    int32 MinimumFreeSpaceGigabytes = 5;

    API_FIELD(Attributes="EditorOrder(120), EditorDisplay(\"Library\"), Limit(0, 87600)")
    int32 GarbageCollectionGracePeriodHours = 24;

    API_FIELD(Attributes="EditorOrder(130), EditorDisplay(\"Library\"), Limit(0, 100)")
    int32 RetainedLastGoodCount = 1;

    API_FIELD(Attributes="EditorOrder(140), EditorDisplay(\"Library\"), Limit(1, 3650)")
    int32 LogRetentionDays = 14;

    API_FIELD(Attributes="EditorOrder(200), EditorDisplay(\"Build Limits\"), Limit(0, 1024)")
    int32 WorkerLimit = 0;

    API_FIELD(Attributes="EditorOrder(210), EditorDisplay(\"Build Limits\"), Limit(128, 1048576)")
    int32 MemoryLimitMegabytes = 4096;

public:
    /// <summary>
    /// Gets the active settings asset, or a legacy-default instance when the project has none.
    /// </summary>
    static AssetPipelineSettings* Get();

    /// <summary>
    /// Validates supported rollout combinations and numeric limits.
    /// </summary>
    bool IsValid(AssetPipelineDiagnostic& diagnostic) const;

    // [SettingsBase]
    void Apply() override;
};
