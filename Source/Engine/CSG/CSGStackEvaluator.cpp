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

#endif
