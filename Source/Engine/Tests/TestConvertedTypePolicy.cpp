// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetPipeline/ConvertedTypePolicy.h"
#include "Engine/Content/AssetPipeline/AssetPipelineSettings.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Converted type lockout forbids new flax authoring and keeps legacy exceptions")
{
    AssetPipelineSettings settings;
    CHECK_FALSE(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Material"), TEXT("Content/M.flax")));
    CHECK(ConvertedTypePolicy::IsConvertedGraphType(TEXT("FlaxEngine.VisualScript")));
    CHECK_FALSE(ConvertedTypePolicy::IsLegacyExceptionType(TEXT("FlaxEngine.Shader")));
    CHECK_FALSE(ConvertedTypePolicy::IsConvertedGraphType(TEXT("FlaxEngine.Shader")));
    CHECK(ConvertedTypePolicy::IsConvertedImportedType(TEXT("FlaxEngine.Shader")));
    CHECK(ConvertedTypePolicy::IsConvertedAssetType(TEXT("FlaxEngine.Texture")));

    AssetPipelineDiagnostic diagnostic;
    CHECK(settings.IsValid(diagnostic));
    CHECK_FALSE(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Material"), TEXT("Content/M.flax")));
    CHECK(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Material"), TEXT("Content/M.material")));
    CHECK(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Scene"), TEXT("Content/Level.scene")));
    CHECK(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Prefab"), TEXT("Content/Item.prefab")));
    CHECK_FALSE(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.MaterialInstance"), TEXT("Content/I.flax")));
    CHECK(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.MaterialInstance"), TEXT("Content/I.materialinstance")));
    CHECK_FALSE(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Texture"), TEXT("Content/T.flax")));
    CHECK(ConvertedTypePolicy::AllowsLegacyBinaryAuthoring(settings, TEXT("FlaxEngine.Texture"), TEXT("Content/T.png")));
}
