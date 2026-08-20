// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "CSGOperand.h"
#include "CSGMesh.h"
#include "CSGHierarchy.h"

#if COMPILE_WITH_CSG_BUILDER

namespace CSG
{
    /// <summary>
    /// CSG target compilation utilities.
    /// </summary>
    class FLAXENGINE_API CSGCompilation
    {
    public:
        /// <summary>
        /// Snapshots an operand from a live brush, optionally converting into target-local space.
        /// </summary>
        static bool SnapshotOperand(Brush* brush, int32 operationIndex, Operand& outOperand, const Transform* targetTransform = nullptr);

        /// <summary>
        /// Snapshots an ordered sequence of operands from live brushes, optionally converting into target-local space.
        /// </summary>
        static bool SnapshotOperands(const Array<Brush*>& brushes, Array<Operand>& outOperands, const Transform* targetTransform = nullptr);

        /// <summary>
        /// Compiles a set of brushes belonging to a single stack scope into a resolved mesh.
        /// </summary>
        static bool CompileStack(const Array<Brush*>& brushes, Mesh& outMesh, const Transform* targetTransform = nullptr, StackBuildStats* stats = nullptr);

        /// <summary>
        /// Compiles all explicit and implicit stacks under a target root into a combined resolved mesh in target space.
        /// </summary>
        static bool CompileTargetMeshes(Actor* targetRoot, Mesh& outCombinedMesh);
    };
}

#endif
