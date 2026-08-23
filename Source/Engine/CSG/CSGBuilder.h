// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Delegate.h"
#include "Brush.h"

class Scene;
class Actor;
class Model;

/// <summary>
/// Builds transient CSG models for editor previews.
/// </summary>
API_CLASS(Static, Attributes="HideInEditor") class FLAXENGINE_API CSGPreviewBuilder
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(CSGPreviewBuilder);

public:
    /// <summary>
    /// Compiles all CSG brushes below an actor into a transient model.
    /// </summary>
    /// <param name="root">The hierarchy root to compile.</param>
    /// <param name="reusableModel">An inactive transient model that may be reused.</param>
    /// <returns>The compiled transient model, or null if the hierarchy has no visible CSG geometry.</returns>
    API_FUNCTION() static Model* Build(Actor* root, Model* reusableModel = nullptr);
};

namespace CSG
{
    class RawData;

#if COMPILE_WITH_CSG_BUILDER

    /// <summary>
    /// CSG geometry builder
    /// </summary>
    class FLAXENGINE_API Builder
    {
    public:

        /// <summary>
        /// Action fired when any CSG brush on scene gets edited (different dimensions or transformation etc.)
        /// </summary>
        static Delegate<Brush*> OnBrushModified;

    public:

        static bool IsActive();

        /// <summary>
        /// Build CSG geometry for the given scene.
        /// </summary>
        /// <param name="scene">The scene.</param>
        /// <param name="timeoutMs">The timeout to wait before building CSG (in milliseconds).</param>
        static void Build(Scene* scene, float timeoutMs = 50);

    };

#endif
};
