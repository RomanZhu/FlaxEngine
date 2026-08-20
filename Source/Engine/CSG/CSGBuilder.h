// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Delegate.h"
#include "Brush.h"

class Scene;
class CSGModel;

namespace CSG
{
    class RawData;

    /// <summary>
    /// Build intent for CSG model output generation.
    /// </summary>
    enum class ModelBuildIntent : byte
    {
        /// <summary>Throttled live interactive preview without disk persistence or collision cooking.</summary>
        Preview,
        /// <summary>Persisted model, raw data, and collision generation.</summary>
        Persist,
    };

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

        /// <summary>
        /// Build CSG geometry for the given CSGModel actor.
        /// </summary>
        /// <param name="model">The model.</param>
        /// <param name="timeoutMs">The timeout to wait before building CSG (in milliseconds).</param>
        /// <param name="intent">The build intent (preview vs persist).</param>
        static void Build(CSGModel* model, float timeoutMs = 50, ModelBuildIntent intent = ModelBuildIntent::Preview);

        /// <summary>
        /// Synchronously compiles and persists CSG model output.
        /// </summary>
        /// <param name="model">The model.</param>
        /// <param name="ownerAssetId">The owning asset ID (e.g. Prefab ID) for asset-owned output, or empty for scene-owned output.</param>
        /// <returns>True if successfully persisted, false otherwise.</returns>
        static bool Persist(CSGModel* model, const Guid& ownerAssetId = Guid::Empty);
    };

#endif
};
