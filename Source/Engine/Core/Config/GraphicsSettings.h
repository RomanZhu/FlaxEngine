// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config/Settings.h"
#include "Engine/Graphics/Enums.h"
#include "Engine/Graphics/PostProcessSettings.h"

class FontAsset;

/// <summary>
/// Probe-ray tracing backend used by Dynamic Diffuse Global Illumination.
/// </summary>
API_ENUM() enum class DDGITraceBackend
{
    /// <summary>
    /// Cross-platform Global SDF and Global Surface Atlas tracing.
    /// </summary>
    SoftwareGlobalSDF = 0,

    /// <summary>
    /// Optional hardware ray tracing backend. Unsupported devices fall back to software tracing.
    /// </summary>
    HardwareRayTracing = 1,
};

/// <summary>
/// Ray tracing backend used by Global Distance Field Global Illumination (GDFGI).
/// </summary>
API_ENUM() enum class GDFGITraceBackend
{
    /// <summary>
    /// Standard Global SDF and Global Surface Atlas ray marching.
    /// </summary>
    GlobalSDF = 0,

    /// <summary>
    /// Derived Hierarchical Digital Differential Analyzer (HDDA) tracing.
    /// </summary>
    DerivedHDDA = 1,
};

/// <summary>
/// Execution stages for GDFGI developer isolation, step-by-step validation, and DX11 stabilization.
/// </summary>
API_ENUM() enum class GDFGIDebugExecutionStage
{
    /// <summary>
    /// Normal production GDFGI execution.
    /// </summary>
    Disabled = 0,

    /// <summary>
    /// Allocates and clears GDFGI textures only (no compute dispatch).
    /// </summary>
    AllocateOnly = 1,

    /// <summary>
    /// Runs probe classification and relocation kernel only.
    /// </summary>
    Classify = 2,

    /// <summary>
    /// Runs probe classification and directional radiance ray tracing.
    /// </summary>
    Trace = 3,

    /// <summary>
    /// Runs up to temporal history update.
    /// </summary>
    Temporal = 4,

    /// <summary>
    /// Runs up to diffuse convolution.
    /// </summary>
    Convolve = 5,

    /// <summary>
    /// Runs up to deferred indirect lighting composition.
    /// </summary>
    Composite = 6,

    /// <summary>
    /// Full GDFGI pipeline execution with multibounce.
    /// </summary>
    Full = 7,
};

/// <summary>
/// Graphics rendering settings.
/// </summary>
API_CLASS(sealed, Namespace="FlaxEditor.Content.Settings", NoConstructor) class FLAXENGINE_API GraphicsSettings : public SettingsBase
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(GraphicsSettings);

public:
    /// <summary>
    /// List of pixel formats that can be used by the rendering pipeline (for light buffer and post-processing).
    /// </summary>
    API_ENUM(Attributes="EnumDisplay(EnumDisplayAttribute.FormatMode.None)")
    enum class RenderColorFormats
    {
        // HDR 32-bit buffer without alpha channel support. Offers good performance but might result in colors banding or shift towards yellowish colors due to low data precision.
        R11G11B10,
        // LDR 32-bit buffer with alpha channel support. Offers good performance but doesn't support High Dynamic Range rendering.
        R8G8B8A8,
        // HDR 64-bit buffer with alpha channel support. Offers very good quality for wide range of colors but requires more memory.
        R16G16B16A16,
    };

public:
    /// <summary>
    /// Enables rendering synchronization with the refresh rate of the display device to avoid "tearing" artifacts.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), DefaultValue(false), EditorDisplay(\"General\", \"Use V-Sync\")")
    bool UseVSync = false;

    /// <summary>
    /// Anti Aliasing quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1000), DefaultValue(Quality.Medium), EditorDisplay(\"Quality\", \"AA Quality\")")
    Quality AAQuality = Quality::Medium;

    /// <summary>
    /// Screen Space Reflections quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1100), DefaultValue(Quality.Medium), EditorDisplay(\"Quality\", \"SSR Quality\")")
    Quality SSRQuality = Quality::Medium;

    /// <summary>
    /// Screen Space Ambient Occlusion quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1200), DefaultValue(Quality.Medium), EditorDisplay(\"Quality\", \"SSAO Quality\")")
    Quality SSAOQuality = Quality::Medium;

    /// <summary>
    /// Volumetric Fog quality setting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1250), DefaultValue(Quality.High), EditorDisplay(\"Quality\")")
    Quality VolumetricFogQuality = Quality::High;

    /// <summary>
    /// The shadows quality.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1300), DefaultValue(Quality.Medium), EditorDisplay(\"Quality\")")
    Quality ShadowsQuality = Quality::Medium;

    /// <summary>
    /// The shadow maps quality (textures resolution).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1310), DefaultValue(Quality.Medium), EditorDisplay(\"Quality\")")
    Quality ShadowMapsQuality = Quality::Medium;

    /// <summary>
    /// Enables cascades splits blending for directional light shadows.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1320), DefaultValue(false), EditorDisplay(\"Quality\", \"Allow CSM Blending\")")
    bool AllowCSMBlending = false;

    /// <summary>
    /// Default probes cubemap resolution (use for Environment Probes, can be overriden per-actor).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1500), EditorDisplay(\"Quality\")")
    ProbeCubemapResolution DefaultProbeResolution = ProbeCubemapResolution::_128;

    /// <summary>
    /// If checked, Environment Probes will use HDR texture format. Improves quality in very bright scenes at cost of higher memory usage.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(1502), EditorDisplay(\"Quality\")")
    bool UseHDRProbes = false;

    /// <summary>
    /// If checked, enables Global SDF rendering. This can be used in materials, shaders, and particles.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2000), EditorDisplay(\"Global SDF\")")
    bool EnableGlobalSDF = false;

    /// <summary>
    /// Draw distance of the Global SDF. Actual value can be larger when using DDGI.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2001), EditorDisplay(\"Global SDF\"), Limit(1000), ValueCategory(Utils.ValueCategory.Distance)")
    float GlobalSDFDistance = 15000.0f;

    /// <summary>
    /// The Global SDF quality. Controls the volume texture resolution and amount of cascades to use.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2005), DefaultValue(Quality.High), EditorDisplay(\"Global SDF\"), CustomEditorAlias(\"FlaxEditor.CustomEditors.Editors.GlobalIlluminationQualityEditor\")")
    Quality GlobalSDFQuality = Quality::High;

#if USE_EDITOR
    /// <summary>
    /// If checked, the 'Generate SDF' option will be checked on model import options by default. Use it if your project uses Global SDF (eg. for Global Illumination or particles).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2010), EditorDisplay(\"Global SDF\")")
    bool GenerateSDFOnModelImport = false;
#endif

    /// <summary>
    /// The Global Illumination quality. Controls the quality of the GI effect.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2100), DefaultValue(Quality.High), EditorDisplay(\"Global Illumination\"), CustomEditorAlias(\"FlaxEditor.CustomEditors.Editors.GlobalIlluminationQualityEditor\")")
    Quality GIQuality = Quality::High;

    /// <summary>
    /// The requested DDGI probe-ray tracing backend. Software Global SDF tracing is the portable default; hardware ray tracing is opt-in and falls back to software when unavailable.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2110), DefaultValue(DDGITraceBackend.SoftwareGlobalSDF), EditorDisplay(\"Global Illumination\")")
    DDGITraceBackend TraceBackend = DDGITraceBackend::SoftwareGlobalSDF;

    /// <summary>
    /// The global spacing between Global Illumination probes (in world units). Smaller values improve interior detail at a higher GPU cost. Values around 100-150 are a useful starting point for mixed interiors and exteriors; adjust to 200-500 for mostly outdoor scenes and lower-frequency GI. Changing this value recreates the DDGI probe resources and can change the automatic cascade layout.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2120), Limit(50, 1000), EditorDisplay(\"Global Illumination\")")
    float GIProbesSpacing = 100;

    /// <summary>
    /// Maximum number of DDGI probe rays scheduled per view and frame. Zero uses the renderer's safe default budget.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2121), Limit(0, 16777216), EditorDisplay(\"Global Illumination\")")
    uint32 DDGIProbeRayBudget = 65536;

    /// <summary>
    /// Near-field Global SDF half-extent in world units when DDGI is enabled. The outer SDF cascades still cover the full GI range.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2122), Limit(100, 100000), ValueCategory(Utils.ValueCategory.Distance), EditorDisplay(\"Global Illumination\")")
    float DDGINearFieldDistance = 2000.0f;

    /// <summary>
    /// Expands Global SDF surfaces during DDGI+ software ray hit detection to preserve geometry thinner than the selected SDF voxel size. The effective expansion is bounded to one voxel per SDF cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2123), Limit(0, 1000), ValueCategory(Utils.ValueCategory.Distance), EditorDisplay(\"Global Illumination\")")
    float DDGIThinGeometryExpansion = 0.0f;

    /// <summary>
    /// Logs a diagnostic when the software DDGI path has no valid Global SDF or Surface Atlas coverage.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2124), DefaultValue(true), EditorDisplay(\"Global Illumination\")")
    bool DDGIWarnMissingSDF = true;

    /// <summary>
    /// Exponent used by the DDGI distance moment convolution. Higher values preserve depth discontinuities more strongly.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2126), Limit(1, 100), EditorDisplay(\"Global Illumination\")")
    float DDGIDistanceExponent = 50.0f;

    /// <summary>
    /// Enables smooth blending between Global Illumination cascade splits. If disabled, the transition uses dithering intended for temporal anti-aliasing. Smooth blending can expose rounded cascade boundaries when adjacent cascades contain significantly different lighting.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2125), DefaultValue(false), EditorDisplay(\"Global Illumination\", \"GI Cascades Blending\")")
    bool GICascadesBlending = false;

    /// <summary>
    /// The Global Surface Atlas resolution. Adjust it if atlas `flickers` due to overflow (eg. to 4096).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2130), Limit(256, 8192), EditorDisplay(\"Global Illumination\")")
    int32 GlobalSurfaceAtlasResolution = 2048;

    /// <summary>
    /// Maximum number of GDFGI probe rays scheduled per view and frame. Zero uses the renderer's safe default budget.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2140), Limit(0, 16777216), EditorDisplay(\"Global Illumination\", \"GDFGI Probe Ray Budget\")")
    uint32 GDFGIProbeRayBudget = 3200;

    /// <summary>
    /// Number of temporal history frames in the GDFGI moving average ring buffer.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2141), Limit(1, 32), EditorDisplay(\"Global Illumination\", \"GDFGI History Frames\")")
    uint32 GDFGIHistoryFrames = 8;

    /// <summary>
    /// Inactive probe update interval in frames for GDFGI.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2142), Limit(1, 64), EditorDisplay(\"Global Illumination\", \"GDFGI Inactive Probe Update Frames\")")
    uint32 GDFGIInactiveProbeUpdateFrames = 4;

    /// <summary>
    /// Dynamic invalidation radius around dirty bounding boxes in probe spacing units.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2143), Limit(0.5f, 10.0f), EditorDisplay(\"Global Illumination\", \"GDFGI Dynamic Dirty Radius (Probe Spacings)\")")
    float GDFGIDynamicDirtyRadiusInProbeSpacings = 3.0f;

    /// <summary>
    /// Expands Global SDF surfaces during GDFGI ray hit detection to preserve thin geometry.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2144), Limit(0, 1000), ValueCategory(Utils.ValueCategory.Distance), EditorDisplay(\"Global Illumination\", \"GDFGI Thin Geometry Expansion\")")
    float GDFGIThinGeometryExpansion = 0.0f;

    /// <summary>
    /// Enables directional specular indirect reflections sampled from GDFGI directional radiance bins.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2145), DefaultValue(false), EditorDisplay(\"Global Illumination\", \"GDFGI Enable Directional Specular\")")
    bool GDFGIEnableDirectionalSpecular = false;

    /// <summary>
    /// Ray tracing backend to use for GDFGI.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2146), DefaultValue(GDFGITraceBackend.GlobalSDF), EditorDisplay(\"Global Illumination\", \"GDFGI Trace Backend\")")
    GDFGITraceBackend GDFGITraceBackend = GDFGITraceBackend::GlobalSDF;

    /// <summary>
    /// Developer-only debug execution stage for GDFGI pipeline isolation and DX11 validation.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(2147), DefaultValue(GDFGIDebugExecutionStage.Disabled), EditorDisplay(\"Global Illumination\", \"GDFGI Debug Execution Stage\")")
    GDFGIDebugExecutionStage GDFGIDebugStage = GDFGIDebugExecutionStage::Disabled;

public:
    /// <summary>
    /// If checked, color space workflow will use Gamma instead of Linear. Gamma color space defines colors with an applied a gamma curve (sRGB) so they are perceptually linear.
    /// This makes sense when the output of the rendering represent final color values that will be presented to a non-HDR screen.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3000), EditorDisplay(\"Colors\")")
    bool GammaColorSpace = true;

    /// <summary>
    /// Pixel format used by the rendering pipeline (for light buffer and post-processing).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(3010), EditorDisplay(\"Colors\")")
    RenderColorFormats RenderColorFormat = RenderColorFormats::R11G11B10;

public:
    /// <summary>
    /// The default Post Process settings. Can be overriden by PostFxVolume on a level locally, per camera or for a whole map.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10000), EditorDisplay(\"Post Process Settings\", EditorDisplayAttribute.InlineStyle)")
    PostProcessSettings PostProcessSettings;

public:
    /// <summary>
    /// The list of fallback fonts used for text rendering. Ignored if empty.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(5000), EditorDisplay(\"Text\")")
    Array<AssetReference<FontAsset>> FallbackFonts;

private:
    /// <summary>
    /// Renamed UeeHDRProbes into UseHDRProbes
    /// [Deprecated on 12.10.2022, expires on 12.10.2024]
    /// </summary>
    API_PROPERTY(Attributes="Serialize, Obsolete, NoUndo") DEPRECATED("Use UseHDRProbes instead.") bool GetUeeHDRProbes() const { return UseHDRProbes; }
    API_PROPERTY(Attributes="Serialize, Obsolete, NoUndo") DEPRECATED("Use UseHDRProbes instead.") void SetUeeHDRProbes(bool value);
    API_FUNCTION(Attributes="OnDeserializing", Hidden) void OnDeserializing(const CallbackContext& context);

public:
    /// <summary>
    /// Gets the instance of the settings asset (default value if missing). Object returned by this method is always loaded with valid data to use.
    /// </summary>
    static GraphicsSettings* Get();

    // [SettingsBase]
    void Apply() override;
};
