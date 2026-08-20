// Copyright (c) Wojciech Figat. All rights reserved.

#include "BoxBrush.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Content/Content.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/CSG/CSGHierarchy.h"
#include "Engine/Level/Actors/CSGModel.h"

namespace
{
    CSG::CSGCompiledData* FindCompiledCSGData(const Actor* actor, Transform* outTargetTransform = nullptr)
    {
        if (actor == nullptr)
            return nullptr;

        Actor* outputScope = CSG::CSGHierarchy::FindOwningOutputScope(actor);
        if (outputScope != nullptr)
        {
            auto model = dynamic_cast<CSGModel*>(outputScope);
            if (model != nullptr)
            {
                if (outTargetTransform)
                    *outTargetTransform = model->GetTransform();
                return &model->CSGData;
            }
        }

        Scene* scene = actor->GetScene();
        if (scene != nullptr)
        {
            if (outTargetTransform)
                *outTargetTransform = Transform::Identity;
            return &scene->CSGData;
        }

        return nullptr;
    }
}

void BrushSurface::Serialize(SerializeStream& stream, const void* otherObj)
{
    SERIALIZE_GET_OTHER_OBJ(BrushSurface);

    SERIALIZE(Material);
    SERIALIZE_MEMBER(Offset, TexCoordOffset);
    SERIALIZE_MEMBER(Scale, TexCoordScale);
    SERIALIZE_MEMBER(Rotation, TexCoordRotation);
    SERIALIZE_MEMBER(ScaleInLightmap, ScaleInLightmap);
}

void BrushSurface::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    DESERIALIZE(Material);
    DESERIALIZE_MEMBER(Offset, TexCoordOffset);
    DESERIALIZE_MEMBER(Scale, TexCoordScale);
    DESERIALIZE_MEMBER(Rotation, TexCoordRotation);
    DESERIALIZE_MEMBER(ScaleInLightmap, ScaleInLightmap);
}

BoxBrush::BoxBrush(const SpawnParams& params)
    : Actor(params)
    , _center(Vector3::Zero)
    , _size(100.0f)
    , _mode(CSG::Mode::Additive)
    , _flipNormals(false)
{
    for (uint32 i = 0; i < ARRAY_COUNT(Surfaces); i++)
    {
        auto& surface = Surfaces[i];
        surface.Brush = this;
        surface.Index = i;
    }
}

Array<BrushSurface> BoxBrush::GetSurfaces() const
{
    Array<BrushSurface> value;
    value.Set(Surfaces, ARRAY_COUNT(Surfaces));
    return value;
}

void BoxBrush::SetSurfaces(const Array<BrushSurface>& value)
{
    CHECK(value.Count() == ARRAY_COUNT(Surfaces));
    for (int32 i = 0; i < ARRAY_COUNT(Surfaces); i++)
    {
        auto& destination = Surfaces[i];
        const auto& source = value[i];
        destination.Brush = this;
        destination.Index = i;
        destination.Material = source.Material;
        destination.TexCoordScale = source.TexCoordScale;
        destination.TexCoordOffset = source.TexCoordOffset;
        destination.TexCoordRotation = source.TexCoordRotation;
        destination.ScaleInLightmap = source.ScaleInLightmap;
    }
    OnBrushModified();
}

void BoxBrush::SetMode(BrushMode value)
{
    if (_mode != value)
    {
        _mode = value;
        OnBrushModified();
    }
}

void BoxBrush::SetFlipNormals(bool value)
{
    if (_flipNormals != value)
    {
        _flipNormals = value;
        OnBrushModified();
    }
}

void BoxBrush::SetCenter(const Vector3& value)
{
    if (value == _center)
        return;

    _center = value;

    // Fire events
    UpdateBounds();
    OnBrushModified();
}

void BoxBrush::SetSize(const Vector3& value)
{
    if (value == _size)
        return;

    _size = value;

    // Fire events
    UpdateBounds();
    OnBrushModified();
}

void BoxBrush::GetSurfaces(CSG::Surface surfaces[6])
{
    // Init normals
    surfaces[0].Normal = Vector3::Right;
    surfaces[1].Normal = Vector3::Left;
    surfaces[2].Normal = Vector3::Up;
    surfaces[3].Normal = Vector3::Down;
    surfaces[4].Normal = Vector3::Forward;
    surfaces[5].Normal = Vector3::Backward;

    // Calculate final transformation
    const auto transform = _transform.LocalToWorld(Transform(_center, Quaternion::Identity, _size));

    // Expand subtractive input geometry by a tiny build-only tolerance. Keeping
    // this out of the actor Size and Transform preserves exact grid-authored
    // values while preventing coplanar cuts from leaving a surface behind.
    const Real csgOverlap = _mode == CSG::Mode::Subtractive ? 0.01f : 0.0f;
    surfaces[0].D = surfaces[1].D = transform.Scale.X / 2 + csgOverlap;
    surfaces[2].D = surfaces[3].D = transform.Scale.Y / 2 + csgOverlap;
    surfaces[4].D = surfaces[5].D = transform.Scale.Z / 2 + csgOverlap;

    // Add rotation
    Matrix rotation;
    Matrix::RotationQuaternion(transform.Orientation, rotation);
    for (int32 i = 0; i < 6; i++)
        Vector3::TransformNormal(surfaces[i].Normal, rotation, surfaces[i].Normal);

    // Add translation
    for (int32 i = 0; i < 6; i++)
        surfaces[i].Translate(transform.Translation);

    // Copy per surface properties
    for (int32 i = 0; i < 6; i++)
    {
        auto& dst = surfaces[i];
        auto& src = Surfaces[i];

        dst.Material = src.Material.GetID();
        dst.TexCoordScale = src.TexCoordScale;
        dst.TexCoordOffset = src.TexCoordOffset;
        dst.TexCoordRotation = src.TexCoordRotation;
        dst.ScaleInLightmap = src.ScaleInLightmap;
    }
}

void BoxBrush::SetMaterial(int32 surfaceIndex, MaterialBase* material)
{
    CHECK(Math::IsInRange(surfaceIndex, 0, 5));
    Surfaces[surfaceIndex].Material = material;
    OnBrushModified();
}

bool BoxBrush::Intersects(int32 surfaceIndex, const Ray& ray, Real& distance, Vector3& normal) const
{
    distance = MAX_Real;
    normal = Vector3::Up;

    Transform targetTransform;
    CSG::CSGCompiledData* csgData = FindCompiledCSGData(this, &targetTransform);
    if (csgData == nullptr)
        return false;

    CSG::CSGCompiledData::SurfaceData surfaceData;
    if (!csgData->TryGetSurfaceData(GetBrushID(), surfaceIndex, surfaceData))
        return false;

    if (!targetTransform.IsIdentity())
    {
        Ray localRay;
        localRay.Position = targetTransform.WorldToLocal(ray.Position);
        localRay.Direction = targetTransform.WorldToLocalVector(ray.Direction).Normalized();

        Real localDistance;
        Vector3 localNormal;
        if (surfaceData.Intersects(localRay, localDistance, localNormal))
        {
            normal = targetTransform.LocalToWorldVector(localNormal).Normalized();
            Vector3 hitLocal = localRay.Position + localRay.Direction * localDistance;
            Vector3 hitWorld = targetTransform.LocalToWorld(hitLocal);
            distance = Vector3::Distance(ray.Position, hitWorld);
            return true;
        }
        return false;
    }

    return surfaceData.Intersects(ray, distance, normal);
}

void BoxBrush::GetVertices(int32 surfaceIndex, Array<Vector3>& outputData) const
{
    Transform targetTransform;
    CSG::CSGCompiledData* csgData = FindCompiledCSGData(this, &targetTransform);
    if (csgData == nullptr)
        return;

    CSG::CSGCompiledData::SurfaceData surfaceData;
    if (csgData->TryGetSurfaceData(GetBrushID(), surfaceIndex, surfaceData))
    {
        if (!targetTransform.IsIdentity())
        {
            for (int32 i = 0; i < surfaceData.Triangles.Count(); i++)
            {
                outputData.Add(targetTransform.LocalToWorld(surfaceData.Triangles[i].V0));
                outputData.Add(targetTransform.LocalToWorld(surfaceData.Triangles[i].V1));
                outputData.Add(targetTransform.LocalToWorld(surfaceData.Triangles[i].V2));
            }
        }
        else
        {
            outputData.Add((Vector3*)surfaceData.Triangles.Get(), 3 * surfaceData.Triangles.Count());
        }
    }
}

void BoxBrush::Serialize(SerializeStream& stream, const void* otherObj)
{
    // Base
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(BoxBrush);

    SERIALIZE_MEMBER(Mode, _mode);
    SERIALIZE_MEMBER(FlipNormals, _flipNormals);
    SERIALIZE_MEMBER(Center, _center);
    SERIALIZE_MEMBER(Size, _size);
    SERIALIZE(ScaleInLightmap);

    stream.JKEY("Surfaces");
    stream.StartArray();
    for (int32 i = 0; i < 6; i++)
    {
        stream.Object(&Surfaces[i], other ? &other->Surfaces[i] : nullptr);
    }
    stream.EndArray();
}

void BoxBrush::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    // Base
    Actor::Deserialize(stream, modifier);

    DESERIALIZE_MEMBER(Mode, _mode);
    DESERIALIZE_MEMBER(FlipNormals, _flipNormals);
    DESERIALIZE_MEMBER(Center, _center);
    DESERIALIZE_MEMBER(Size, _size);
    DESERIALIZE(ScaleInLightmap);

    const DeserializeStream& surfaces = stream["Surfaces"];
    ASSERT(surfaces.IsArray() && surfaces.Size() == ARRAY_COUNT(Surfaces));
    for (rapidjson::SizeType i = 0; i < surfaces.Size(); i++)
    {
        Surfaces[i].Deserialize((DeserializeStream&)surfaces[i], modifier);
    }
}

bool BoxBrush::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    bool result = false;
    Real minDistance = MAX_float;
    Vector3 minDistanceNormal = Vector3::Up;
    if (_bounds.Intersects(ray, distance))
    {
        for (int32 surfaceIndex = 0; surfaceIndex < 6; surfaceIndex++)
        {
            if (Intersects(surfaceIndex, ray, distance, normal) && distance < minDistance)
            {
                minDistance = distance;
                minDistanceNormal = normal;
                result = true;
            }
        }
    }
    distance = minDistance;
    normal = minDistanceNormal;
    return result;
}

#if USE_EDITOR

#include "Engine/Debug/DebugDraw.h"

namespace
{
    void DrawSelectedBrushXRay(const OrientedBoundingBox& box)
    {
        static const int32 edges[] =
        {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7,
        };
        constexpr int32 dashCount = 8;
        constexpr float dashFraction = 0.55f;
        Vector3 corners[8];
        box.GetCorners(corners);
        const Color color(1.0f, 1.0f, 0.0f, 0.22f);
        for (int32 edgeIndex = 0; edgeIndex < ARRAY_COUNT(edges); edgeIndex += 2)
        {
            const Vector3 start = corners[edges[edgeIndex]];
            const Vector3 edge = corners[edges[edgeIndex + 1]] - start;
            for (int32 dash = 0; dash < dashCount; dash++)
            {
                const float from = static_cast<float>(dash) / dashCount;
                const float to = (dash + dashFraction) / dashCount;
                DEBUG_DRAW_LINE(start + edge * from, start + edge * to, color, 0, false);
            }
        }
    }
}

void BoxBrush::OnDebugDrawSelected()
{
    // Keep hidden extents readable without drawing solid lines through geometry.
    // The depth-tested pass covers the x-ray dashes wherever an edge is visible.
    DrawSelectedBrushXRay(_bounds);
    DEBUG_DRAW_WIRE_BOX(_bounds, Color::Yellow, 0, true);

    // Base
    Actor::OnDebugDrawSelected();
}

#endif

Scene* BoxBrush::GetBrushScene() const
{
    return GetScene();
}

Guid BoxBrush::GetBrushID() const
{
    return GetID();
}

bool BoxBrush::CanUseCSG() const
{
    return IsActiveInHierarchy();
}

CSG::Mode BoxBrush::GetBrushMode() const
{
    return _mode;
}

bool BoxBrush::GetBrushFlipNormals() const
{
    return _flipNormals;
}

void BoxBrush::GetSurfaces(Array<CSG::Surface>& surfaces)
{
    surfaces.Clear();
    surfaces.Resize(6, false);

    GetSurfaces(surfaces.Get());
}

int32 BoxBrush::GetSurfacesCount()
{
    return 6;
}

void BoxBrush::OnTransformChanged()
{
    // Base
    Actor::OnTransformChanged();

    // Fire events
    UpdateBounds();
    OnBrushModified();
}

void BoxBrush::OnActiveInTreeChanged()
{
    // Base
    Actor::OnActiveInTreeChanged();

    // Fire event
    OnBrushModified();
}

void BoxBrush::OnOrderInParentChanged()
{
    // Base
    Actor::OnOrderInParentChanged();

    // Fire event
    OnBrushModified();
}

void BoxBrush::OnParentChanged()
{
    // Base
    Actor::OnParentChanged();

    if (!IsDuringPlay())
        return;

    // Fire event
    OnBrushModified();
}
