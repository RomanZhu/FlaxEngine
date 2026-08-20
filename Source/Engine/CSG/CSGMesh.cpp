// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGMesh.h"

#if COMPILE_WITH_CSG_BUILDER

#include "Engine/Core/Collections/Array.h"

using namespace CSG;

bool CSG::Mesh::HasMode(Mode mode) const
{
    for (int32 i = 0; i < _brushesMeta.Count(); i++)
    {
        if (_brushesMeta[i].Mode == mode)
            return true;
    }

    return false;
}

void CSG::Mesh::Add(const Mesh* other)
{
    ASSERT(this != other && other);

    // Cache data
    int32 baseIndexVertices = _vertices.Count();
    int32 baseIndexSurfaces = _surfaces.Count();
    int32 baseIndexEdges = _edges.Count();
    int32 baseIndexPolygons = _polygons.Count();
    auto oVertices = other->GetVertices();
    auto oSurfaces = other->GetSurfaces();
    auto oEdges = other->GetEdges();
    auto oPolygons = other->GetPolygons();
    auto oMeta = &other->_brushesMeta;

    // Clone vertices
    for (int32 i = 0; i < oVertices->Count(); i++)
    {
        _vertices.Add(oVertices->At(i));
    }

    // Clone surfaces
    for (int32 i = 0; i < oSurfaces->Count(); i++)
    {
        _surfaces.Add(oSurfaces->At(i));
    }

    // Clone edges
    for (int32 i = 0; i < oEdges->Count(); i++)
    {
        HalfEdge edge = oEdges->At(i);
        edge.PolygonIndex += baseIndexPolygons;
        edge.TwinIndex += baseIndexEdges;
        edge.NextIndex += baseIndexEdges;
        edge.VertexIndex += baseIndexVertices;
        _edges.Add(edge);
    }

    // Clone polygons
    for (int32 i = 0; i < oPolygons->Count(); i++)
    {
        Polygon polygon = oPolygons->At(i);
        polygon.SurfaceIndex += baseIndexSurfaces;
        polygon.FirstEdgeIndex += baseIndexEdges;
        _polygons.Add(polygon);
    }

    // Clone meta
    for (int32 i = 0; i < oMeta->Count(); i++)
    {
        BrushMeta meta = oMeta->At(i);
        meta.StartSurfaceIndex += baseIndexSurfaces;
        _brushesMeta.Add(meta);
    }

    // Increase bounds
    _bounds.Add(other->GetBounds());
}

void CSG::Mesh::AppendResolvedGeometry(const Mesh* other)
{
    Add(other);
}

#endif
