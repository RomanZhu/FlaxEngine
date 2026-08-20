// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGCompiledData.h"
#include "Engine/Core/Log.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Core/Math/CollisionsHelper.h"
#include "Engine/CSG/CSGData.h"

using namespace CSG;

CSGCompiledData::CSGCompiledData()
{
    Data.Loaded.Bind<CSGCompiledData, &CSGCompiledData::OnDataChanged>(this);
    Data.Changed.Bind<CSGCompiledData, &CSGCompiledData::OnDataChanged>(this);
}

CSGCompiledData::~CSGCompiledData()
{
    Data.Loaded.Unbind<CSGCompiledData, &CSGCompiledData::OnDataChanged>(this);
    Data.Changed.Unbind<CSGCompiledData, &CSGCompiledData::OnDataChanged>(this);
}

void CSGCompiledData::PublishPreviewModel(::Model* model)
{
    _hasPreviewOverride = true;
    PreviewModel = model;
}

void CSGCompiledData::PublishPreviewSurfaceData(const RawData& rawData)
{
    _previewSurfaceData.Clear();
    _previewSurfaceData.EnsureCapacity(rawData.Brushes.Count());

    for (auto i = rawData.Brushes.Begin(); i.IsNotEnd(); ++i)
    {
        const Guid brushId = i->Key;
        const RawData::BrushData& srcBrush = i->Value;
        auto& dstSurfaces = _previewSurfaceData[brushId];
        dstSurfaces.Resize(srcBrush.Surfaces.Count());

        for (int32 surfaceIndex = 0; surfaceIndex < srcBrush.Surfaces.Count(); surfaceIndex++)
        {
            const auto& srcTriangles = srcBrush.Surfaces[surfaceIndex].Triangles;
            auto& dstTriangles = dstSurfaces[surfaceIndex].Triangles;
            dstTriangles.Resize(srcTriangles.Count());

            for (int32 triangleIndex = 0; triangleIndex < srcTriangles.Count(); triangleIndex++)
            {
                dstTriangles[triangleIndex].V0 = srcTriangles[triangleIndex].V[0];
                dstTriangles[triangleIndex].V1 = srcTriangles[triangleIndex].V[1];
                dstTriangles[triangleIndex].V2 = srcTriangles[triangleIndex].V[2];
            }
        }
    }
}

void CSGCompiledData::PublishEmptyPreview()
{
    _hasPreviewOverride = true;
    PreviewModel = nullptr;
    PreviewModelCache = nullptr;
    _previewSurfaceData.Clear();
}

void CSGCompiledData::ClearTransientPreview()
{
    _hasPreviewOverride = false;
    PreviewModel = nullptr;
    PreviewModelCache = nullptr;
    _previewSurfaceData.Clear();
}

void CSGCompiledData::ClearAll()
{
    ClearTransientPreview();
    Model = nullptr;
    Data = nullptr;
    CollisionData = nullptr;
    DataBrushLocations.Clear();
}

bool CSGCompiledData::SurfaceData::Intersects(const Ray& ray, Real& distance, Vector3& normal) const
{
    bool result = false;
    Real minDistance = MAX_Real;
    Vector3 minDistanceNormal = Vector3::Up;
    for (int32 i = 0; i < Triangles.Count(); i++)
    {
        const auto& e = Triangles[i];
        if (CollisionsHelper::RayIntersectsTriangle(ray, e.V0, e.V1, e.V2, distance, normal) && distance < minDistance)
        {
            minDistance = distance;
            minDistanceNormal = normal;
            result = true;
        }
    }
    distance = minDistance;
    normal = minDistanceNormal;
    return result;
}

bool CSGCompiledData::TryGetSurfaceData(const Guid& brushId, int32 brushSurfaceIndex, SurfaceData& outData)
{
    if (_hasPreviewOverride)
    {
        Array<SurfaceData>* surfaces = _previewSurfaceData.TryGet(brushId);
        if (surfaces == nullptr)
            return false;
        if (!Math::IsInRange(brushSurfaceIndex, 0, surfaces->Count() - 1))
            return false;
        outData = (*surfaces)[brushSurfaceIndex];
        return true;
    }

    if (Data == nullptr || !Data->IsLoaded() || Data->Data.IsEmpty())
    {
        return false;
    }

    MemoryReadStream stream(Data->Data);

    if (DataBrushLocations.IsEmpty())
    {
        int32 version;
        stream.ReadInt32(&version);
        if (version == 1)
        {
            int32 brushesCount;
            stream.ReadInt32(&brushesCount);
            if (brushesCount < 0)
            {
                return false;
            }
            DataBrushLocations.EnsureCapacity(brushesCount);
            for (int32 i = 0; i < brushesCount; i++)
            {
                Guid id;
                int32 pos;
                stream.Read(id);
                stream.ReadInt32(&pos);
                DataBrushLocations.Add(id, pos);
            }
        }
        else
        {
            LOG(Warning, "Unknown version for CSG surface data (or corrupted file).");
            return false;
        }
    }

    int32 brushLocation;
    if (!DataBrushLocations.TryGet(brushId, brushLocation))
    {
        return false;
    }

    stream.SetPosition(brushLocation);

    int32 trianglesCount;
    while (brushSurfaceIndex-- > 0)
    {
        stream.ReadInt32(&trianglesCount);
        if (trianglesCount < 0 || trianglesCount > 100)
        {
            return false;
        }
        stream.Move(trianglesCount * sizeof(Float3) * 3);
    }

    stream.ReadInt32(&trianglesCount);
    if (trianglesCount < 0 || trianglesCount > 100)
    {
        return false;
    }
    outData.Triangles.Clear();
    outData.Triangles.Resize(trianglesCount);
    Float3* src = stream.Move<Float3>(trianglesCount * 3);
    Vector3* dst = (Vector3*)outData.Triangles.Get();
    for (int32 i = 0; i < trianglesCount * 3; i++)
        *dst++ = *src++;
    return true;
}

void CSGCompiledData::OnDataChanged()
{
    DataBrushLocations.Clear();
}

void CSGCompiledData::Serialize(SerializeStream& stream, const void* otherObj)
{
    SERIALIZE_GET_OTHER_OBJ(CSGCompiledData);

    SERIALIZE(Model);
    SERIALIZE(Data);
    SERIALIZE(CollisionData);
}

void CSGCompiledData::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    DESERIALIZE(Model);
    DESERIALIZE(Data);
    DESERIALIZE(CollisionData);
}
