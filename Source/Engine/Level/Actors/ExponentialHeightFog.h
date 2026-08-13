// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "../Actor.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include "Engine/Renderer/DrawCall.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Graphics/GPUPipelineStatePermutations.h"

/// <summary>
/// Used to create fogging effects such as clouds but with a density that is related to the height of the fog.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Visuals/Lighting & PostFX/Exponential Height Fog\"), ActorToolbox(\"Visuals\")")
class FLAXENGINE_API ExponentialHeightFog : public Actor, public IFogRenderer
{
    DECLARE_SCENE_OBJECT(ExponentialHeightFog);
private:
    AssetReference<Shader> _shader;
    GPUPipelineStatePermutationsPs<2> _psFog;
    int32 _sceneRenderingKey = -1;

public:
    /// <summary>
    /// The fog density factor.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), DefaultValue(0.02f), Limit(0.0000001f, 100.0f, 0.001f), EditorDisplay(\"Exponential Height Fog\")")
    float FogDensity = 0.02f;

    /// <summary>
    /// The fog height density factor that controls how the density increases as height decreases. Smaller values produce a more visible transition layer.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), DefaultValue(0.2f), Limit(0.0001f, 10.0f, 0.001f), EditorDisplay(\"Exponential Height Fog\")")
    float FogHeightFalloff = 0.2f;

    /// <summary>
    /// Color of the fog.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), DefaultValue(typeof(Color), \"0.448,0.634,1.0\"), EditorDisplay(\"Exponential Height Fog\")")
    Color FogInscatteringColor = Color(0.448f, 0.634f, 1.0f);

    /// <summary>
    /// Maximum opacity of the fog.
    /// A value of 1 means the fog can become fully opaque at a distance and replace scene color completely.
    /// A value of 0 means the fog color will not be factored in at all.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), DefaultValue(1.0f), Limit(0, 1, 0.001f), EditorDisplay(\"Exponential Height Fog\")")
    float FogMaxOpacity = 1.0f;

    /// <summary>
    /// Distance from the camera that the fog will start, in world units.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(50), DefaultValue(0.0f), Limit(0), EditorDisplay(\"Exponential Height Fog\")")
    float StartDistance = 0.0f;

    /// <summary>
    /// Scene elements past this distance will not have fog applied. This is useful for excluding skyboxes which already have fog baked in. Setting this value to 0 disables it. Negative value sets the cutoff distance relative to the far plane of the camera.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(60), DefaultValue(0.0f), EditorDisplay(\"Exponential Height Fog\")")
    float FogCutoffDistance = 0.0f;

public:
    /// <summary>
    /// Directional light used for Directional Inscattering.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(200), DefaultValue(null), EditorDisplay(\"Directional Inscattering\", \"Light\")")
    ScriptingObjectReference<DirectionalLight> DirectionalInscatteringLight;

    /// <summary>
    /// Controls the size of the directional inscattering cone, which is used to approximate inscattering from a directional light.
    /// Note: there must be a directional light enabled for DirectionalInscattering to be used. Range: 2-64.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(210), DefaultValue(4.0f), Limit(2, 64, 0.1f), EditorDisplay(\"Directional Inscattering\", \"Exponent\")")
    float DirectionalInscatteringExponent = 4.0f;

    /// <summary>
    /// Controls the start distance from the viewer of the directional inscattering, which is used to approximate inscattering from a directional light.
    /// Note: there must be a directional light enabled for DirectionalInscattering to be used.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(220), DefaultValue(10000.0f), Limit(0), EditorDisplay(\"Directional Inscattering\", \"Start Distance\")")
    float DirectionalInscatteringStartDistance = 10000.0f;

    /// <summary>
    /// Controls the color of the directional inscattering, which is used to approximate inscattering from a directional light.
    /// Note: there must be a directional light enabled for DirectionalInscattering to be used.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(230), DefaultValue(typeof(Color), \"0.25,0.25,0.125\"), EditorDisplay(\"Directional Inscattering\", \"Color\")")
    Color DirectionalInscatteringColor = Color(0.25, 0.25f, 0.125f);

public:
    /// <summary>
    /// Whether to enable Volumetric fog. Graphics quality settings control the resolution of the fog simulation.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(300), DefaultValue(false), EditorDisplay(\"Volumetric Fog\", \"Enable\")")
    bool VolumetricFogEnable = false;

    /// <summary>
    /// Controls the scattering phase function - how much incoming light scatters in various directions.
    /// A distribution value of 0 scatters equally in all directions, while 0.9 scatters predominantly in the light direction.
    /// In order to have visible volumetric fog light shafts from the side, the distribution will need to be closer to 0. Range: -0.9-0.9.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(310), DefaultValue(0.2f), Limit(-0.9f, 0.9f, 0.001f), EditorDisplay(\"Volumetric Fog\", \"Forward Scattering Distribution\")")
    float VolumetricFogScatteringDistribution = 0.2f;

    /// <summary>
    /// Enables independent control of scattered light without changing fog extinction.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(311), DefaultValue(false), EditorDisplay(\"Volumetric Fog\", \"Independent Scattering\")")
    bool VolumetricFogIndependentScatteringEnable = false;

    /// <summary>
    /// Scales light scattered by the volumetric medium independently of extinction. Values above one strengthen light shafts without making the fog more opaque.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(312), DefaultValue(1.0f), Limit(0, 10, 0.01f), VisibleIf(nameof(VolumetricFogIndependentScatteringEnable)), EditorDisplay(\"Volumetric Fog\", \"Scattering Intensity\")")
    float VolumetricFogScatteringIntensity = 1.0f;

    /// <summary>
    /// Enables blending of independently controlled forward and backward phase-function lobes.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(313), DefaultValue(false), EditorDisplay(\"Volumetric Fog\", \"Dual-Lobe Phase Function\")")
    bool VolumetricFogDualLobePhaseEnable = false;

    /// <summary>
    /// Relative contribution of the forward scattering lobe.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(314), DefaultValue(1.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogDualLobePhaseEnable)), EditorDisplay(\"Volumetric Fog\", \"Forward Scattering Weight\")")
    float VolumetricFogForwardScatteringWeight = 1.0f;

    /// <summary>
    /// Controls the distribution of the secondary backward scattering lobe. Range: -0.9-0.9.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(315), DefaultValue(-0.2f), Limit(-0.9f, 0.9f, 0.001f), VisibleIf(nameof(VolumetricFogDualLobePhaseEnable)), EditorDisplay(\"Volumetric Fog\", \"Backward Scattering Distribution\")")
    float VolumetricFogBackwardScatteringDistribution = -0.2f;

    /// <summary>
    /// Relative contribution of the backward scattering lobe. A value of zero preserves the legacy single-lobe phase function.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(316), DefaultValue(0.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogDualLobePhaseEnable)), EditorDisplay(\"Volumetric Fog\", \"Backward Scattering Weight\")")
    float VolumetricFogBackwardScatteringWeight = 0.0f;

public:
    /// <summary>
    /// Enables artistic presentation controls for directional-light volumetric shadows without changing the base fog density.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(317), DefaultValue(false), EditorDisplay(\"Volumetric Fog Shadows\", \"Enable\")")
    bool VolumetricFogShadowPresentationEnable = false;

    /// <summary>
    /// Shapes the directional volumetric shadow transition. Values above one produce darker, more prominent partial shadows.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(318), DefaultValue(1.0f), Limit(0.1f, 8, 0.01f), VisibleIf(nameof(VolumetricFogShadowPresentationEnable)), EditorDisplay(\"Volumetric Fog Shadows\", \"Contrast\")")
    float VolumetricFogShadowContrast = 1.0f;

    /// <summary>
    /// Scales extinction in fully shadowed fog. Values above one make shadowed regions more obscuring without thickening illuminated fog.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(319), DefaultValue(1.0f), Limit(0, 8, 0.01f), VisibleIf(nameof(VolumetricFogShadowPresentationEnable)), EditorDisplay(\"Volumetric Fog Shadows\", \"Shadow Extinction Multiplier\")")
    float VolumetricFogShadowExtinctionMultiplier = 1.0f;

    /// <summary>
    /// Scales accumulated lighting in fog regions shadowed by the directional light. Lower values reduce ambient and local-light fill without increasing fog opacity.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(320), DefaultValue(1.0f), Limit(0, 4, 0.01f), VisibleIf(nameof(VolumetricFogShadowPresentationEnable)), EditorDisplay(\"Volumetric Fog Shadows\", \"Shadow Scattering Multiplier\")")
    float VolumetricFogShadowScatteringMultiplier = 1.0f;

    /// <summary>
    /// Minimum directional-light visibility retained in volumetric shadow. Raise it to prevent shadowed fog from becoming completely black.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(321), DefaultValue(0.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogShadowPresentationEnable)), EditorDisplay(\"Volumetric Fog Shadows\", \"Minimum Ambient Scattering\")")
    float VolumetricFogMinimumAmbientScattering = 0.0f;

    /// <summary>
    /// Blends between unshadowed directional fog lighting and the sampled volumetric shadow. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(322), DefaultValue(1.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogShadowPresentationEnable)), EditorDisplay(\"Volumetric Fog Shadows\", \"Directional Shadow Strength\")")
    float VolumetricFogDirectionalShadowStrength = 1.0f;

    /// <summary>
    /// The height fog particle reflectiveness used by volumetric fog.
    /// Water particles in air have an albedo near white, while dust has slightly darker value.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(320), DefaultValue(typeof(Color), \"1,1,1,1\"), EditorDisplay(\"Volumetric Fog\", \"Albedo\")")
    Color VolumetricFogAlbedo = Color::White;

    /// <summary>
    /// Light emitted by height fog. This is a density value so more light is emitted the further you are looking through the fog.
    /// In most cases using a Skylight is a better choice, however, it may be useful in certain scenarios.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(330), DefaultValue(typeof(Color), \"0,0,0,1\"), EditorDisplay(\"Volumetric Fog\", \"Emissive\")")
    Color VolumetricFogEmissive = Color::Black;

    /// <summary>
    /// Scales the height fog particle extinction amount used by volumetric fog.
    /// Values larger than 1 cause fog particles everywhere absorb more light. Range: 0.1-10.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(340), DefaultValue(1.0f), Limit(0.1f, 10, 0.1f), EditorDisplay(\"Volumetric Fog\", \"Extinction Scale\")")
    float VolumetricFogExtinctionScale = 1.0f;

    /// <summary>
    /// Distance over which volumetric fog should be computed. Larger values extend the effect into the distance but expose under-sampling artifacts in details.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(350), DefaultValue(6000.0f), Limit(0), EditorDisplay(\"Volumetric Fog\", \"Distance\")")
    float VolumetricFogDistance = 6000.0f;

    /// <summary>
    /// Controls the part of the volumetric fog distance used to smoothly fade volumetric scattering and extinction into the regular distance fog. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(360), DefaultValue(0.2f), Limit(0, 1, 0.01f), EditorDisplay(\"Volumetric Fog\", \"Distance Fade\")")
    float VolumetricFogDistanceFade = 0.2f;

    /// <summary>
    /// Enables jittered temporal reprojection for volumetric fog. Disable it to use stable cell-center sampling without temporal history.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(370), DefaultValue(true), EditorDisplay(\"Volumetric Fog\", \"Temporal Reprojection\")")
    bool VolumetricFogTemporalReprojection = true;

    /// <summary>
    /// Controls how much of the previous frame contributes to volumetric fog when temporal reprojection is enabled. Range: 0-0.99.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(380), DefaultValue(0.92f), Limit(0, 0.99f, 0.01f), EditorDisplay(\"Volumetric Fog\", \"History Weight\")")
    float VolumetricFogHistoryWeight = 0.92f;

    /// <summary>
    /// Reduces temporal history as the procedural density field moves faster to limit motion trails.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(381), DefaultValue(false), EditorDisplay(\"Volumetric Fog\", \"Reactive History\")")
    bool VolumetricFogReactiveHistory = false;

    /// <summary>
    /// Density-field speed in world units per second that halves the temporal history weight.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(382), DefaultValue(1000.0f), Limit(1), VisibleIf(nameof(VolumetricFogReactiveHistory)), EditorDisplay(\"Volumetric Fog\", \"Reactive History Velocity Scale\")")
    float VolumetricFogReactiveHistoryVelocityScale = 1000.0f;

public:
    /// <summary>
    /// Enables per-froxel temporal rejection for changed extinction, neighborhood mismatch, and camera motion.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(383), DefaultValue(false), EditorDisplay(\"Volumetric Fog Temporal Stability\", \"Local History Rejection\")")
    bool VolumetricFogLocalHistoryRejectionEnable = false;

    /// <summary>
    /// Relative current-versus-history extinction difference that begins reducing temporal history. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(384), DefaultValue(0.1f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogLocalHistoryRejectionEnable)), EditorDisplay(\"Volumetric Fog Temporal Stability\", \"Extinction Difference Threshold\")")
    float VolumetricFogHistoryExtinctionDifferenceThreshold = 0.1f;

    /// <summary>
    /// Clamps reprojected extinction to the current six-froxel neighborhood. Zero disables neighborhood sampling; one applies the full clamp. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(385), DefaultValue(1.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogLocalHistoryRejectionEnable)), EditorDisplay(\"Volumetric Fog Temporal Stability\", \"Neighborhood Clamp Strength\")")
    float VolumetricFogHistoryNeighborhoodClampStrength = 1.0f;

    /// <summary>
    /// Reduces history when reprojection moves across the screen due to camera rotation or translation. Zero disables camera-motion response.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(386), DefaultValue(1.0f), Limit(0, 4, 0.01f), VisibleIf(nameof(VolumetricFogLocalHistoryRejectionEnable)), EditorDisplay(\"Volumetric Fog Temporal Stability\", \"Camera Motion Response\")")
    float VolumetricFogHistoryCameraMotionResponse = 1.0f;

    /// <summary>
    /// Minimum history weight retained after local rejection. Keep at zero to allow complete rejection in changed or disoccluded fog. Range: 0-0.99.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(387), DefaultValue(0.0f), Limit(0, 0.99f, 0.01f), VisibleIf(nameof(VolumetricFogLocalHistoryRejectionEnable)), EditorDisplay(\"Volumetric Fog Temporal Stability\", \"Minimum History Weight\")")
    float VolumetricFogMinimumHistoryWeight = 0.0f;

public:
    /// <summary>
    /// Enables a second analytical exponential density layer for independent ground mist or atmospheric haze.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(390), DefaultValue(false), EditorDisplay(\"Volumetric Fog Layer 2\", \"Enable\")")
    bool VolumetricFogSecondLayerEnable = false;

    /// <summary>
    /// Density of the second volumetric layer.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(391), DefaultValue(0.01f), Limit(0, 100, 0.001f), VisibleIf(nameof(VolumetricFogSecondLayerEnable)), EditorDisplay(\"Volumetric Fog Layer 2\", \"Density\")")
    float VolumetricFogSecondLayerDensity = 0.01f;

    /// <summary>
    /// Base-height offset of the second layer relative to this Actor, in world units.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(392), DefaultValue(0.0f), VisibleIf(nameof(VolumetricFogSecondLayerEnable)), EditorDisplay(\"Volumetric Fog Layer 2\", \"Height Offset\")")
    float VolumetricFogSecondLayerHeightOffset = 0.0f;

    /// <summary>
    /// Height falloff of the second layer. Smaller values produce a broader transition.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(393), DefaultValue(0.05f), Limit(0.0001f, 10, 0.001f), VisibleIf(nameof(VolumetricFogSecondLayerEnable)), EditorDisplay(\"Volumetric Fog Layer 2\", \"Height Falloff\")")
    float VolumetricFogSecondLayerHeightFalloff = 0.05f;

    /// <summary>
    /// Blends the second layer between uniform density and the existing procedural density field. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(394), DefaultValue(0.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogSecondLayerEnable)), EditorDisplay(\"Volumetric Fog Layer 2\", \"Density Noise Influence\")")
    float VolumetricFogSecondLayerDensityNoiseInfluence = 0.0f;

public:
    /// <summary>
    /// Enables world-space procedural density variation in the global volumetric fog.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(400), DefaultValue(false), EditorDisplay(\"Volumetric Fog Density\", \"Enable\")")
    bool VolumetricFogDensityNoiseEnable = false;

    /// <summary>
    /// Size of the repeating base density formation in world units. Larger values produce broader, smoother fog banks.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(410), DefaultValue(3000.0f), Limit(1), EditorDisplay(\"Volumetric Fog Density\", \"Scale\")")
    float VolumetricFogDensityNoiseScale = 3000.0f;

    /// <summary>
    /// Number of density-noise layers. More layers add detail at an increased GPU sampling cost. Range: 1-4.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(420), DefaultValue(4), Limit(1, 4), EditorDisplay(\"Volumetric Fog Density\", \"Octaves\")")
    int32 VolumetricFogDensityNoiseOctaves = 4;

    /// <summary>
    /// Frequency multiplier between consecutive density-noise layers. Range: 1-4.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(430), DefaultValue(2.0f), Limit(1, 4, 0.01f), EditorDisplay(\"Volumetric Fog Density\", \"Lacunarity\")")
    float VolumetricFogDensityNoiseLacunarity = 2.0f;

    /// <summary>
    /// Amplitude multiplier between consecutive density-noise layers. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(440), DefaultValue(0.35f), Limit(0, 1, 0.01f), EditorDisplay(\"Volumetric Fog Density\", \"Gain\")")
    float VolumetricFogDensityNoiseGain = 0.35f;

    /// <summary>
    /// Noise value remapped to zero density. Raising it creates larger clear regions. Must be lower than Maximum.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(450), DefaultValue(0.0f), Limit(0, 1, 0.01f), EditorDisplay(\"Volumetric Fog Density\", \"Minimum\")")
    float VolumetricFogDensityNoiseMin = 0.0f;

    /// <summary>
    /// Noise value remapped to full density. Lowering it produces denser, sharper fog banks. Must be greater than Minimum.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(460), DefaultValue(1.0f), Limit(0, 1, 0.01f), EditorDisplay(\"Volumetric Fog Density\", \"Maximum\")")
    float VolumetricFogDensityNoiseMax = 1.0f;

    /// <summary>
    /// Blends between uniform height fog and the procedural density field. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(470), DefaultValue(1.0f), Limit(0, 1, 0.01f), EditorDisplay(\"Volumetric Fog Density\", \"Influence\")")
    float VolumetricFogDensityNoiseInfluence = 1.0f;

    /// <summary>
    /// World-space velocity used to advect the density field, in world units per second.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(480), DefaultValue(typeof(Float3), \"0,0,0\"), EditorDisplay(\"Volumetric Fog Density\", \"Velocity\")")
    Float3 VolumetricFogDensityNoiseVelocity = Float3::Zero;

    /// <summary>
    /// Seed used to offset the procedural density field.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(490), DefaultValue(0), EditorDisplay(\"Volumetric Fog Density\", \"Seed\")")
    int32 VolumetricFogDensityNoiseSeed = 0;

    /// <summary>
    /// Rotates and offsets successive density-noise octaves to reduce aligned and repeating formations.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(500), DefaultValue(true), EditorDisplay(\"Volumetric Fog Density\", \"Decorrelate Octaves\")")
    bool VolumetricFogDensityNoiseDecorrelateOctaves = true;

    /// <summary>
    /// Enables inversion and contrast shaping of the remapped density-noise field.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(505), DefaultValue(false), EditorDisplay(\"Volumetric Fog Density\", \"Density Shaping\")")
    bool VolumetricFogDensityNoiseShapingEnable = false;

    /// <summary>
    /// Inverts the remapped density-noise field to create hollow or complementary formations.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(510), DefaultValue(false), VisibleIf(nameof(VolumetricFogDensityNoiseShapingEnable)), EditorDisplay(\"Volumetric Fog Density\", \"Invert\")")
    bool VolumetricFogDensityNoiseInvert = false;

    /// <summary>
    /// Applies a power curve to the remapped density field. Values above one sharpen dense formations; values below one broaden them.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(520), DefaultValue(1.0f), Limit(0.01f, 8, 0.01f), VisibleIf(nameof(VolumetricFogDensityNoiseShapingEnable)), EditorDisplay(\"Volumetric Fog Density\", \"Contrast\")")
    float VolumetricFogDensityNoiseContrast = 1.0f;

    /// <summary>
    /// Enables altitude-based attenuation of procedural density variation.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(525), DefaultValue(false), EditorDisplay(\"Volumetric Fog Density\", \"Height-Dependent Noise\")")
    bool VolumetricFogDensityNoiseHeightEnable = false;

    /// <summary>
    /// Fades procedural variation above the fog Actor height. Zero disables the height-dependent fade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(530), DefaultValue(0.0f), Limit(0, 10, 0.001f), VisibleIf(nameof(VolumetricFogDensityNoiseHeightEnable)), EditorDisplay(\"Volumetric Fog Density\", \"Height Falloff\")")
    float VolumetricFogDensityNoiseHeightFalloff = 0.0f;

    /// <summary>
    /// Minimum procedural density influence retained high above the fog Actor. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(540), DefaultValue(0.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogDensityNoiseHeightEnable)), EditorDisplay(\"Volumetric Fog Density\", \"Minimum Height Influence\")")
    float VolumetricFogDensityNoiseHeightMinimumInfluence = 0.0f;

public:
    /// <summary>
    /// Enables smooth density attenuation around the camera for close-range readability.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(600), DefaultValue(false), EditorDisplay(\"Volumetric Fog Clarity\", \"Enable\")")
    bool VolumetricFogNearClarityEnable = false;

    /// <summary>
    /// Radius around the camera over which the minimum retained density is used.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(610), DefaultValue(200.0f), Limit(0), VisibleIf(nameof(VolumetricFogNearClarityEnable)), EditorDisplay(\"Volumetric Fog Clarity\", \"Clear Radius\")")
    float VolumetricFogNearClarityRadius = 200.0f;

    /// <summary>
    /// Distance over which density fades from the clear radius back to full strength.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(620), DefaultValue(500.0f), Limit(0.001f), VisibleIf(nameof(VolumetricFogNearClarityEnable)), EditorDisplay(\"Volumetric Fog Clarity\", \"Fade Distance\")")
    float VolumetricFogNearClarityFadeDistance = 500.0f;

    /// <summary>
    /// Fraction of density retained inside the clear radius. Range: 0-1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(630), DefaultValue(0.0f), Limit(0, 1, 0.01f), VisibleIf(nameof(VolumetricFogNearClarityEnable)), EditorDisplay(\"Volumetric Fog Clarity\", \"Minimum Density\")")
    float VolumetricFogNearClarityMinimumDensity = 0.0f;

public:
    /// <summary>
    /// Development visualization for inspecting volumetric fog density, lighting, history, and froxel resolution.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(700), DefaultValue(VolumetricFogDebugMode.None), EditorDisplay(\"Volumetric Fog Debug\", \"Mode\")")
    VolumetricFogDebugMode VolumetricFogDebug = VolumetricFogDebugMode::None;

private:
#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj)
    {
        _psFog.Release();
    }
#endif

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override
    {
        const Vector3 size(50);
        return BoundingBox(_transform.Translation - size, _transform.Translation + size);
    }
#endif
    void Draw(RenderContext& renderContext) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
    bool HasContentLoaded() const override;
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;

    // [IFogRenderer]
    void GetVolumetricFogOptions(VolumetricFogOptions& result) const override;
    void GetExponentialHeightFogData(const RenderView& view, ShaderExponentialHeightFogData& result) const override;
    void DrawFog(GPUContext* context, RenderContext& renderContext, GPUTextureView* output) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void OnTransformChanged() override;
};
