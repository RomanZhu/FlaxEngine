// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGStackEvaluator.h"

#if COMPILE_WITH_CSG_BUILDER

using namespace CSG;

bool CSGStackEvaluator::Contains(const Operand& operand, const Vector3& point, Real tolerance)
{
    const int32 surfaceCount = operand.Surfaces.Count();
    if (surfaceCount < 4)
        return false;

    for (int32 i = 0; i < surfaceCount; i++)
    {
        const auto& surface = operand.Surfaces[i];
        if (surface.Distance(point) > tolerance)
            return false;
    }

    return true;
}

PointState CSGStackEvaluator::EvaluatePoint(const Vector3& point, Span<const Operand> operands, Real tolerance)
{
    PointState state;
    state.Solid = false;
    state.LastInfluencingOperation = -1;

    for (int32 i = 0; i < operands.Length(); i++)
    {
        const auto& op = operands[i];
        if (Contains(op, point, tolerance))
        {
            state.Solid = (op.Mode == Mode::Additive);
            state.LastInfluencingOperation = i;
        }
    }

    return state;
}

Real CSGStackEvaluator::CalculateSampleEpsilon(const Vector3& p, const Vector3& normal, Span<const Operand> operands)
{
    Real eps = Plane::DistanceEpsilon * 4.0f;
    if (eps < 0.001f)
        eps = 0.001f;

    for (int32 i = 0; i < operands.Length(); i++)
    {
        const auto& surfaces = operands[i].Surfaces;
        for (int32 j = 0; j < surfaces.Count(); j++)
        {
            const auto& surface = surfaces[j];
            const float dot = Math::Abs(Vector3::Dot(surface.Normal, normal));
            if (dot < 0.999f)
            {
                const Real dist = Math::Abs(surface.Distance(p));
                if (dist > Plane::DistanceEpsilon && dist * 0.25f < eps)
                {
                    eps = Math::Max(dist * 0.25f, (Real)0.0001f);
                }
            }
        }
    }
    return eps;
}

bool CSGStackEvaluator::EvaluateStack(Span<const Operand> operands, Mesh& outputMesh, StackBuildStats* stats)
{
    if (operands.IsEmpty())
        return true;

    if (stats)
    {
        stats->OperandCount = operands.Length();
        stats->CandidatePolygonCount = 0;
        stats->SplitCount = 0;
        stats->FinalFragmentCount = 0;
        stats->DiscardedInternalCount = 0;
        stats->DuplicateFragmentCount = 0;
    }

    // Step 1: Build source mesh for each operand
    Array<Mesh> operandMeshes;
    operandMeshes.Resize(operands.Length());

    for (int32 i = 0; i < operands.Length(); i++)
    {
        const auto& op = operands[i];
        if (op.Surfaces.Count() < 4)
            return false;

        operandMeshes[i].Build(op);
        if (operandMeshes[i].GetPolygons()->IsEmpty())
            return false;

        if (stats)
            stats->CandidatePolygonCount += operandMeshes[i].GetPolygons()->Count();
    }

    // Step 2: For each operand, partition its polygons by overlapping operands' cutting planes
    for (int32 i = 0; i < operands.Length(); i++)
    {
        auto& mesh = operandMeshes[i];
        const auto& opA = operands[i];

        for (int32 j = 0; j < operands.Length(); j++)
        {
            if (i == j)
                continue;

            const auto& opB = operands[j];
            if (AABB::IsOutside(opA.Bounds, opB.Bounds))
                continue;

            const int32 polyCountBefore = mesh.GetPolygons()->Count();
            mesh.PartitionVisiblePolygons(Span<const Surface>(opB.Surfaces.Get(), opB.Surfaces.Count()));
            if (stats)
                stats->SplitCount += mesh.GetPolygons()->Count() - polyCountBefore;
        }
    }

    // Step 3: Classify each fragment by sampling both sides
    for (int32 i = 0; i < operands.Length(); i++)
    {
        auto& mesh = operandMeshes[i];
        const int32 polyCount = mesh.Polygons().Count();

        for (int32 k = 0; k < polyCount; k++)
        {
            auto& polygon = mesh.Polygons()[k];
            if (!polygon.Visible || polygon.FirstEdgeIndex == INVALID_INDEX)
                continue;

            const Vector3 centroid = mesh.GetPolygonCentroid(k);
            const Vector3 normal = mesh.GetPolygonNormal(k);

            if (normal.IsZero() || normal.IsNaN())
            {
                polygon.Visible = false;
                continue;
            }

            const Real eps = CalculateSampleEpsilon(centroid, normal, operands);

            const Vector3 minus = centroid - normal * eps;
            const Vector3 plus = centroid + normal * eps;

            const PointState stateMinus = EvaluatePoint(minus, operands);
            const PointState statePlus = EvaluatePoint(plus, operands);

            if (stateMinus.Solid == statePlus.Solid)
            {
                // No boundary exists across this face (both solid or both empty)
                polygon.Visible = false;
                if (stats)
                    stats->DiscardedInternalCount++;
            }
            else
            {
                // Boundary exists! Check coplanar / boundary ownership
                const int32 boundaryOwner = Math::Max(stateMinus.LastInfluencingOperation, statePlus.LastInfluencingOperation);
                if (boundaryOwner != -1 && boundaryOwner != i)
                {
                    // This boundary is owned by a later (or higher priority) operand
                    polygon.Visible = false;
                    if (stats)
                        stats->DuplicateFragmentCount++;
                    continue;
                }

                if (stateMinus.Solid && !statePlus.Solid)
                {
                    // Current polygon orientation points from solid to empty: keep
                    polygon.Visible = true;
                }
                else // (!stateMinus.Solid && statePlus.Solid)
                {
                    // Orientation is reversed: flip winding
                    polygon.Visible = true;
                    polygon.Inverted = !polygon.Inverted;
                }

                if (stats)
                    stats->FinalFragmentCount++;
            }
        }

        // Append surviving resolved geometry
        outputMesh.AppendResolvedGeometry(&mesh);
    }

    return true;
}

#endif

