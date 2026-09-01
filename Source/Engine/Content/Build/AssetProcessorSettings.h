// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/AssetMeta.h"

/// <summary>Tracked settings schema contract, independent of processor implementation/output versions.</summary>
struct FLAXENGINE_API AssetProcessorSettingsSchema
{
    String ProcessorID;
    int32 CurrentVersion = 1;
    StringAnsi NormalizedDefaults = "{}\n";

    /// <summary>Applies explicit normalized defaults once during sidecar creation.</summary>
    /// <returns>True on failure.</returns>
    bool InitializeNewMeta(AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const;

};
