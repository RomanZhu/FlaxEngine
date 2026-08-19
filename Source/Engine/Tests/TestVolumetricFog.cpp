// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/ScopeExit.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Level/Actors/ExponentialHeightFog.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("VolumetricFogOptions")
{
    auto* fog = ExponentialHeightFog::Spawn(ScriptingObject::SpawnParams(Guid::New(), ExponentialHeightFog::TypeInitializer));
    REQUIRE(fog);
    SCOPE_EXIT
    {
        fog->DeleteObject();
    };

    VolumetricFogOptions options;
    fog->GetVolumetricFogOptions(options);
    CHECK(options.ScatteringIntensity == 1.0f);
    CHECK(options.ForwardScatteringWeight == 1.0f);
    CHECK(options.BackwardScatteringWeight == 0.0f);
    CHECK(options.PhaseDirectionality == 0.5f);
    CHECK_FALSE(options.ShadowPresentationEnable);
    CHECK(options.ShadowContrast == 1.0f);
    CHECK(options.ShadowExtinctionMultiplier == 1.0f);
    CHECK(options.ShadowScatteringMultiplier == 1.0f);
    CHECK(options.MinimumAmbientScattering == 0.0f);
    CHECK(options.DirectionalShadowStrength == 1.0f);
    CHECK_FALSE(options.LocalHistoryRejectionEnable);
    CHECK(options.HistoryExtinctionDifferenceThreshold == 1.0f);
    CHECK(options.HistoryNeighborhoodClampStrength == 0.0f);
    CHECK(options.HistoryCameraMotionResponse == 0.0f);
    CHECK(options.MinimumHistoryWeight == 0.0f);
    CHECK(options.FogLayer2Parameters == Float4::Zero);
    CHECK(options.DensityNoiseDecorrelateOctaves);
    CHECK_FALSE(options.DensityNoiseInvert);
    CHECK(options.DensityNoiseContrast == 1.0f);
    CHECK_FALSE(options.NearClarityEnable);
    CHECK(options.DebugMode == VolumetricFogDebugMode::None);

    fog->VolumetricFogEnable = true;
    fog->VolumetricFogScatteringIntensity = -2.0f;
    fog->VolumetricFogForwardScatteringWeight = 2.0f;
    fog->VolumetricFogBackwardScatteringDistribution = -2.0f;
    fog->VolumetricFogBackwardScatteringWeight = 0.25f;
    fog->VolumetricFogPhaseDirectionality = -1.0f;
    fog->VolumetricFogShadowContrast = 20.0f;
    fog->VolumetricFogShadowExtinctionMultiplier = 20.0f;
    fog->VolumetricFogShadowScatteringMultiplier = 20.0f;
    fog->VolumetricFogMinimumAmbientScattering = 2.0f;
    fog->VolumetricFogDirectionalShadowStrength = -1.0f;
    fog->VolumetricFogHistoryExtinctionDifferenceThreshold = -1.0f;
    fog->VolumetricFogHistoryNeighborhoodClampStrength = 2.0f;
    fog->VolumetricFogHistoryCameraMotionResponse = 8.0f;
    fog->VolumetricFogMinimumHistoryWeight = 2.0f;
    fog->VolumetricFogReactiveHistory = true;
    fog->VolumetricFogReactiveHistoryVelocityScale = 0.0f;
    fog->VolumetricFogSecondLayerEnable = true;
    fog->VolumetricFogSecondLayerDensity = 2.0f;
    fog->VolumetricFogSecondLayerHeightOffset = 300.0f;
    fog->VolumetricFogSecondLayerHeightFalloff = 0.5f;
    fog->VolumetricFogSecondLayerDensityNoiseInfluence = 2.0f;
    fog->VolumetricFogDensityNoiseDecorrelateOctaves = false;
    fog->VolumetricFogDensityNoiseInvert = true;
    fog->VolumetricFogDensityNoiseContrast = 20.0f;
    fog->VolumetricFogDensityNoiseHeightFalloff = 0.25f;
    fog->VolumetricFogDensityNoiseHeightMinimumInfluence = -1.0f;
    fog->VolumetricFogNearClarityEnable = true;
    fog->VolumetricFogNearClarityRadius = -10.0f;
    fog->VolumetricFogNearClarityFadeDistance = 0.0f;
    fog->VolumetricFogNearClarityMinimumDensity = 2.0f;
    fog->VolumetricFogDebug = VolumetricFogDebugMode::Density;

    // Non-neutral authored values remain inactive until their feature switch is enabled.
    fog->GetVolumetricFogOptions(options);
    CHECK(options.ScatteringIntensity == 1.0f);
    CHECK(options.ForwardScatteringWeight == 1.0f);
    CHECK(options.BackwardScatteringWeight == 0.0f);
    CHECK(options.PhaseDirectionality == 0.0f);
    CHECK(options.ShadowContrast == 1.0f);
    CHECK(options.ShadowExtinctionMultiplier == 1.0f);
    CHECK(options.ShadowScatteringMultiplier == 1.0f);
    CHECK(options.MinimumAmbientScattering == 0.0f);
    CHECK(options.DirectionalShadowStrength == 1.0f);
    CHECK_FALSE(options.LocalHistoryRejectionEnable);
    CHECK(options.HistoryExtinctionDifferenceThreshold == 1.0f);
    CHECK(options.HistoryNeighborhoodClampStrength == 0.0f);
    CHECK(options.HistoryCameraMotionResponse == 0.0f);
    CHECK(options.MinimumHistoryWeight == 0.0f);
    CHECK_FALSE(options.DensityNoiseInvert);
    CHECK(options.DensityNoiseContrast == 1.0f);
    CHECK(options.DensityNoiseHeightFalloff == 0.0f);

    fog->VolumetricFogIndependentScatteringEnable = true;
    fog->VolumetricFogDualLobePhaseEnable = true;
    fog->VolumetricFogShadowPresentationEnable = true;
    fog->VolumetricFogLocalHistoryRejectionEnable = true;
    fog->VolumetricFogDensityNoiseShapingEnable = true;
    fog->VolumetricFogDensityNoiseHeightEnable = true;
    fog->GetVolumetricFogOptions(options);

    CHECK(options.UseVolumetricFog());
    CHECK(options.ScatteringIntensity == 0.0f);
    CHECK(options.ForwardScatteringWeight == 1.0f);
    CHECK(options.BackwardScatteringDistribution == -0.9f);
    CHECK(options.BackwardScatteringWeight == 0.25f);
    CHECK(options.PhaseDirectionality == 0.0f);
    CHECK(options.ShadowPresentationEnable);
    CHECK(options.ShadowContrast == 8.0f);
    CHECK(options.ShadowExtinctionMultiplier == 8.0f);
    CHECK(options.ShadowScatteringMultiplier == 4.0f);
    CHECK(options.MinimumAmbientScattering == 1.0f);
    CHECK(options.DirectionalShadowStrength == 0.0f);
    CHECK(options.LocalHistoryRejectionEnable);
    CHECK(options.HistoryExtinctionDifferenceThreshold == 0.0f);
    CHECK(options.HistoryNeighborhoodClampStrength == 1.0f);
    CHECK(options.HistoryCameraMotionResponse == 4.0f);
    CHECK(options.MinimumHistoryWeight == 0.99f);
    CHECK(options.ReactiveHistory);
    CHECK(options.ReactiveHistoryVelocityScale == 1.0f);
    CHECK(Math::NearEqual(options.FogLayer2Parameters.X, 0.002f));
    CHECK(Math::NearEqual(options.FogLayer2Parameters.Z, 0.0005f));
    CHECK(options.FogLayer2Parameters.W == 1.0f);
    CHECK_FALSE(options.DensityNoiseDecorrelateOctaves);
    CHECK(options.DensityNoiseInvert);
    CHECK(options.DensityNoiseContrast == 8.0f);
    CHECK(Math::NearEqual(options.DensityNoiseHeightFalloff, 0.00025f));
    CHECK(options.DensityNoiseHeightMinimumInfluence == 0.0f);
    CHECK(options.NearClarityEnable);
    CHECK(options.NearClarityRadius == 0.0f);
    CHECK(Math::NearEqual(options.NearClarityFadeDistance, 0.001f));
    CHECK(options.NearClarityMinimumDensity == 1.0f);
    CHECK(options.DebugMode == VolumetricFogDebugMode::Density);
}

TEST_CASE("ExponentialHeightFogLargeWorldOrigin")
{
    auto* fog = ExponentialHeightFog::Spawn(ScriptingObject::SpawnParams(Guid::New(), ExponentialHeightFog::TypeInitializer));
    REQUIRE(fog);
    SCOPE_EXIT
    {
        fog->DeleteObject();
    };

    fog->SetPosition(Vector3(0.0, 12500.0, 0.0));

    RenderView view;
    view.Origin = Vector3(0.0, 10000.0, 0.0);
    view.Position = Float3(0.0f, 2000.0f, 0.0f);

    ShaderExponentialHeightFogData data;
    fog->GetExponentialHeightFogData(view, data);

    CHECK(data.FogHeight == 2500.0f);
    const float expectedFogAtViewPosition = data.FogDensity * Math::Pow(2.0f, -data.FogHeightFalloff * (view.Position.Y - data.FogHeight));
    CHECK(Math::NearEqual(data.FogAtViewPosition, expectedFogAtViewPosition));
}
