// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "BoxVolume.h"
#include "Engine/Core/Math/Int3.h"

/// <summary>
/// Update policy for a placed DDGI volume.
/// </summary>
API_ENUM() enum class DDGIVolumeUpdateMode
{
    /// <summary>
    /// Probe lighting is updated continuously.
    /// </summary>
    Dynamic = 0,

    /// <summary>
    /// Probe lighting can be retained at runtime until an explicit invalidation.
    /// </summary>
    RuntimeStatic = 1,

    /// <summary>
    /// Probe lighting is intended to be populated by editor baking.
    /// </summary>
    EditorBaked = 2,
};

/// <summary>
/// Backend policy for a placed DDGI volume.
/// </summary>
API_ENUM() enum class DDGIVolumeBackendOverride
{
    /// <summary>
    /// Use the project DDGI backend policy.
    /// </summary>
    Inherit = 0,

    /// <summary>
    /// Force the software Global SDF backend.
    /// </summary>
    SoftwareGlobalSDF = 1,

    /// <summary>
    /// Request hardware ray tracing. Unsupported devices fall back to software.
    /// </summary>
    HardwareRayTracing = 2,
};

/// <summary>
/// A placed DDGI quality volume. The first software implementation reuses the
/// camera-centered clipmap and applies the selected volume's density, trace
/// range, priority, and backend policy while the view is inside it. Clipmap
/// topology changes are discrete because continuously blending probe spacing
/// or trace range would invalidate probe history every frame.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Visuals/Lighting & PostFX/DDGI Volume\"), ActorToolbox(\"Visuals\")")
class FLAXENGINE_API DDGIVolume : public BoxVolume
{
    DECLARE_SCENE_OBJECT(DDGIVolume);
private:
    float _probeSpacing;
    Int3 _probeCounts;
    int32 _updatePriority;
    int32 _lightingPriority;
    float _blendDistance;
    float _blackCutoffDistance;
    float _maxTraceDistance;

public:
    /// <summary>
    /// Enables this volume.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(0)")
    bool Enabled = true;

    /// <summary>
    /// Probe update policy.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(10)")
    DDGIVolumeUpdateMode UpdateMode = DDGIVolumeUpdateMode::Dynamic;

    /// <summary>
    /// Backend policy for this volume.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(20)")
    DDGIVolumeBackendOverride BackendOverride = DDGIVolumeBackendOverride::Inherit;

public:
    /// <summary>
    /// Gets the desired probe spacing in world units.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(30)")
    FORCE_INLINE float GetProbeSpacing() const
    {
        return _probeSpacing;
    }

    /// <summary>
    /// Sets the desired probe spacing in world units.
    /// </summary>
    API_PROPERTY() void SetProbeSpacing(float value);

    /// <summary>
    /// Gets the desired probe counts for the volume density estimate.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(40)")
    FORCE_INLINE Int3 GetProbeCounts() const
    {
        return _probeCounts;
    }

    /// <summary>
    /// Sets the desired probe counts for the volume density estimate.
    /// </summary>
    API_PROPERTY() void SetProbeCounts(const Int3& value);

    /// <summary>
    /// Gets the update priority. Higher values are scheduled first.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(50)")
    FORCE_INLINE int32 GetUpdatePriority() const
    {
        return _updatePriority;
    }

    /// <summary>
    /// Sets the update priority.
    /// </summary>
    API_PROPERTY() FORCE_INLINE void SetUpdatePriority(int32 value)
    {
        _updatePriority = value;
    }

    /// <summary>
    /// Gets the lighting priority used when overlapping volumes are selected.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(60)")
    FORCE_INLINE int32 GetLightingPriority() const
    {
        return _lightingPriority;
    }

    /// <summary>
    /// Sets the lighting priority used when overlapping volumes are selected.
    /// </summary>
    API_PROPERTY() FORCE_INLINE void SetLightingPriority(int32 value)
    {
        _lightingPriority = value;
    }

    /// <summary>
    /// Gets the edge blend distance in world units.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(70)")
    FORCE_INLINE float GetBlendDistance() const
    {
        return _blendDistance;
    }

    /// <summary>
    /// Sets the edge blend distance in world units.
    /// </summary>
    API_PROPERTY() void SetBlendDistance(float value);

    /// <summary>
    /// Gets the distance below which a local result is considered black-cutoff.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(80)")
    FORCE_INLINE float GetBlackCutoffDistance() const
    {
        return _blackCutoffDistance;
    }

    /// <summary>
    /// Sets the black cutoff distance.
    /// </summary>
    API_PROPERTY() void SetBlackCutoffDistance(float value);

    /// <summary>
    /// Gets the maximum probe trace distance in world units.
    /// </summary>
    API_PROPERTY(Attributes="EditorDisplay(\"DDGI Volume\"), EditorOrder(90)")
    FORCE_INLINE float GetMaxTraceDistance() const
    {
        return _maxTraceDistance;
    }

    /// <summary>
    /// Sets the maximum probe trace distance in world units.
    /// </summary>
    API_PROPERTY() void SetMaxTraceDistance(float value);

    /// <summary>
    /// Gets the smooth influence of the volume at a world-space position.
    /// </summary>
    float GetInfluence(const Vector3& worldPosition) const;

public:
    // [BoxVolume]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
#if USE_EDITOR
    Color GetWiresColor() override;
#endif
};
