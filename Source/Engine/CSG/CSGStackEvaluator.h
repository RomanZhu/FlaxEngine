// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "CSGOperand.h"

#if COMPILE_WITH_CSG_BUILDER

namespace CSG
{
    class Mesh;

    /// <summary>
    /// Evaluator for ordered CSG stack boolean expressions.
    /// </summary>
    class CSGStackEvaluator
    {
    public:
        /// <summary>
        /// Tests if a point is contained within a convex polyhedral operand.
        /// </summary>
        /// <param name="operand">The operand.</param>
        /// <param name="point">The query point.</param>
        /// <param name="tolerance">Distance tolerance for plane containment.</param>
        /// <returns>True if the point is inside or on the boundary of the operand, false if strictly outside.</returns>
        static bool Contains(const Operand& operand, const Vector3& point, Real tolerance = Plane::DistanceEpsilon);

        /// <summary>
        /// Evaluates ordered point occupancy across a sequence of CSG operands.
        /// </summary>
        /// <param name="point">The query point.</param>
        /// <param name="operands">The ordered list of operands.</param>
        /// <param name="tolerance">Distance tolerance for plane containment.</param>
        /// <returns>The point occupancy state and latest containing operand index.</returns>
        static PointState EvaluatePoint(const Vector3& point, Span<const Operand> operands, Real tolerance = Plane::DistanceEpsilon);
    };
}

#endif
