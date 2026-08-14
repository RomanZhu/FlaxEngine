// Copyright (c) Wojciech Figat. All rights reserved.

#include "DDGIVolume.h"
#include "Engine/Core/Math/Math.h"
#if USE_EDITOR
#include "Engine/Core/Math/Color.h"
#endif
#include "Engine/Serialization/Serialization.h"
#include "Engine/Level/Scene/SceneRendering.h"

DDGIVolume::DDGIVolume(const SpawnParams& params)
    : BoxVolume(params)
    , _probeSpacing(50.0f)
    , _probeCounts(16, 8, 16)
    , _updatePriority(0)
    , _lightingPriority(0)
    , _blendDistance(100.0f)
    , _blackCutoffDistance(0.0f)
    , _maxTraceDistance(10000.0f)
{
}

void DDGIVolume::SetProbeSpacing(float value)
{
    _probeSpacing = Math::Clamp(value, 10.0f, 1000.0f);
}

void DDGIVolume::SetProbeCounts(const Int3& value)
{
    _probeCounts = Int3(Math::Clamp(value.X, 2, 256), Math::Clamp(value.Y, 2, 256), Math::Clamp(value.Z, 2, 256));
}

void DDGIVolume::SetBlendDistance(float value)
{
    _blendDistance = Math::Clamp(value, 0.0f, 10000.0f);
}

void DDGIVolume::SetBlackCutoffDistance(float value)
{
    _blackCutoffDistance = Math::Clamp(value, 0.0f, 10000.0f);
}

void DDGIVolume::SetMaxTraceDistance(float value)
{
    _maxTraceDistance = Math::Clamp(value, 100.0f, 100000.0f);
}

float DDGIVolume::GetInfluence(const Vector3& worldPosition) const
{
    if (!Enabled)
        return 0.0f;

    Real distance;
    if (_bounds.Contains(worldPosition, &distance) != ContainmentType::Contains)
        return 0.0f;
    if (_blendDistance <= 0.0f)
        return 1.0f;
    return Math::Saturate((float)distance / _blendDistance);
}

void DDGIVolume::Serialize(SerializeStream& stream, const void* otherObj)
{
    BoxVolume::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(DDGIVolume);

    SERIALIZE_MEMBER(Enabled, Enabled);
    SERIALIZE_MEMBER(UpdateMode, UpdateMode);
    SERIALIZE_MEMBER(BackendOverride, BackendOverride);
    SERIALIZE_MEMBER(ProbeSpacing, _probeSpacing);
    SERIALIZE_MEMBER(ProbeCounts, _probeCounts);
    SERIALIZE_MEMBER(UpdatePriority, _updatePriority);
    SERIALIZE_MEMBER(LightingPriority, _lightingPriority);
    SERIALIZE_MEMBER(BlendDistance, _blendDistance);
    SERIALIZE_MEMBER(BlackCutoffDistance, _blackCutoffDistance);
    SERIALIZE_MEMBER(MaxTraceDistance, _maxTraceDistance);
}

void DDGIVolume::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    BoxVolume::Deserialize(stream, modifier);

    DESERIALIZE_MEMBER(Enabled, Enabled);
    DESERIALIZE_MEMBER(UpdateMode, UpdateMode);
    DESERIALIZE_MEMBER(BackendOverride, BackendOverride);
    DESERIALIZE_MEMBER(ProbeSpacing, _probeSpacing);
    DESERIALIZE_MEMBER(ProbeCounts, _probeCounts);
    DESERIALIZE_MEMBER(UpdatePriority, _updatePriority);
    DESERIALIZE_MEMBER(LightingPriority, _lightingPriority);
    DESERIALIZE_MEMBER(BlendDistance, _blendDistance);
    DESERIALIZE_MEMBER(BlackCutoffDistance, _blackCutoffDistance);
    DESERIALIZE_MEMBER(MaxTraceDistance, _maxTraceDistance);

    SetProbeSpacing(_probeSpacing);
    SetProbeCounts(_probeCounts);
    SetBlendDistance(_blendDistance);
    SetBlackCutoffDistance(_blackCutoffDistance);
    SetMaxTraceDistance(_maxTraceDistance);
}

void DDGIVolume::OnEnable()
{
    GetSceneRendering()->AddDDGIVolume(this);
    Actor::OnEnable();
}

void DDGIVolume::OnDisable()
{
    GetSceneRendering()->RemoveDDGIVolume(this);
    Actor::OnDisable();
}

#if USE_EDITOR

Color DDGIVolume::GetWiresColor()
{
    return Color::MediumAquamarine;
}

#endif
