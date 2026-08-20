// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/CSG/CSGCompiledData.h"
#include "Engine/Core/Types/DateTime.h"

class Scene;

namespace CSG
{
    /// <summary>
    /// CSG geometry data container (used per scene).
    /// </summary>
    class FLAXENGINE_API SceneCSGData : public CSGCompiledData
    {
    private:
        Scene* _scene;

    public:
        /// <summary>
        /// CSG mesh building action time (registered by CSG::Builder, in UTC format). Invalid if not built by active engine instance.
        /// </summary>
        DateTime BuildTime;

    public:
        /// <summary>
        /// Initializes a new instance of the <see cref="SceneCSGData"/> class.
        /// </summary>
        /// <param name="scene">The parent scene.</param>
        SceneCSGData(Scene* scene);

        /// <summary>
        /// Build CSG geometry for the given scene.
        /// </summary>
        /// <param name="timeoutMs">The timeout to wait before building CSG (in milliseconds).</param>
        void BuildCSG(float timeoutMs = 50) const;
    };
}
