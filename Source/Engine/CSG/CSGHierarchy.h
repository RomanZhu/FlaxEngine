// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Level/CSG/CSGScopeTypes.h"

class Actor;
class Scene;

namespace CSG
{
    class Brush;

    /// <summary>
    /// Hierarchy and scope traversal utilities for CSG actors.
    /// </summary>
    class FLAXENGINE_API CSGHierarchy
    {
    public:
        /// <summary>
        /// Finds the nearest output scope ancestor (such as CSGModel), or null if directly under scene.
        /// </summary>
        static Actor* FindOwningOutputScope(const Actor* actor);

        /// <summary>
        /// Finds the nearest boolean stack scope ancestor (such as CSGStack), or null if in implicit stack.
        /// </summary>
        static Actor* FindOwningStackScope(const Actor* actor);

        /// <summary>
        /// Resolves the build target key for a given actor.
        /// </summary>
        static CSGBuildTargetKey ResolveBuildTarget(const Actor* actor);

        /// <summary>
        /// Discovers explicit CSGStack scopes and top-level implicit brushes under a target root.
        /// Traversal stops at child output scopes.
        /// </summary>
        static void CollectTargetScopes(Actor* targetRoot, Array<Actor*>& outExplicitStacks, Array<Brush*>& outImplicitBrushes);

        /// <summary>
        /// Collects all brushes belonging to a stack scope in deterministic depth-first order.
        /// Traversal stops at nested scope boundaries.
        /// </summary>
        static void CollectStackBrushes(Actor* stackRoot, Array<Brush*>& outBrushes);
    };
}
