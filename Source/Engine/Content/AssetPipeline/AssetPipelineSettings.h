// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetPipelineDiagnostics.h"
#include "Engine/Core/Config/Settings.h"

/// <summary>
/// Project settings for Library storage and asset build limits.
/// </summary>
API_CLASS(sealed, Namespace="FlaxEditor.Content.Settings") class FLAXENGINE_API AssetPipelineSettings : public SettingsBase
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetPipelineSettings);
    API_AUTO_SERIALIZATION();

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
    /// Gets the active settings asset, or a default instance when the project has none.
    /// </summary>
    static AssetPipelineSettings* Get();

    /// <summary>
    /// Validates numeric Library and build limits.
    /// </summary>
    bool IsValid(AssetPipelineDiagnostic& diagnostic) const;

    // [SettingsBase]
    void Apply() override;
};
