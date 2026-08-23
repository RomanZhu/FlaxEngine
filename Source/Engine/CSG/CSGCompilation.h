// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "CSGOperand.h"
#include "CSGMesh.h"
#include "CSGHierarchy.h"

class Scene;
class Actor;

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
        /// Snapshots an operand from a live brush.
        /// </summary>
        static bool SnapshotOperand(Brush* brush, int32 operationIndex, Operand& outOperand);

        /// <summary>
        /// Snapshots an ordered sequence of operands from live brushes.
        /// </summary>
        static bool SnapshotOperands(const Array<Brush*>& brushes, Array<Operand>& outOperands);

        /// <summary>
        /// Compiles a set of brushes belonging to a single stack scope into a resolved mesh.
        /// </summary>
        static bool CompileStack(const Array<Brush*>& brushes, Mesh& outMesh, StackBuildStats* stats = nullptr);

        /// <summary>
        /// Compiles all explicit and implicit stacks under an actor into a combined resolved mesh.
        /// </summary>
        static bool CompileTargetMeshes(Actor* targetRoot, Mesh& outCombinedMesh);

        /// <summary>
        /// Compiles all explicit and implicit stacks under a scene into a combined resolved mesh.
        /// </summary>
        static bool CompileTargetMeshes(Scene* scene, Mesh& outCombinedMesh);
    };
}

#endif
