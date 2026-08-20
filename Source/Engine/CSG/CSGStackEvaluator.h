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

        /// <summary>
        /// Evaluates an ordered sequence of CSG operands into a single resolved output mesh.
        /// </summary>
        /// <param name="operands">The ordered operands.</param>
        /// <param name="outputMesh">The destination mesh to receive resolved boundaries.</param>
        /// <param name="stats">Optional statistics output.</param>
        /// <returns>True if evaluation succeeded, false if any operand failed to build or evaluate.</returns>
        static bool EvaluateStack(Span<const Operand> operands, Mesh& outputMesh, StackBuildStats* stats = nullptr);

        /// <summary>
        /// Calculates adaptive sample epsilon for two-sided occupancy testing.
        /// </summary>
        /// <param name="p">Polygon sample point.</param>
        /// <param name="normal">Polygon geometric normal.</param>
        /// <param name="operands">The stack operands.</param>
        /// <returns>The calculated epsilon distance.</returns>
        static Real CalculateSampleEpsilon(const Vector3& p, const Vector3& normal, Span<const Operand> operands);
    };
}

#endif
