// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Prepared canonical settings document state.</summary>
class FLAXENGINE_API SettingsPreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SourceHash;

    uint64 GetMemoryUsage() const override
    {
        return sizeof(SettingsPreparedPayload);
    }
};

/// <summary>Packages authored settings JSON as a runtime JsonAsset artifact.</summary>
class FLAXENGINE_API SettingsProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 2;
    static constexpr uint32 RuntimeFormatVersion = 1;

    static const String& ProcessorID();
    static AssetProcessorDescriptor CreateDescriptor();
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
