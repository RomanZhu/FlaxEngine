// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneCSGData.h"
#include "Engine/CSG/CSGBuilder.h"
#include "Engine/Level/Scene/Scene.h"

using namespace CSG;

void Brush::OnBrushModified()
{
#if COMPILE_WITH_CSG_BUILDER
    const auto scene = GetBrushScene();
    if (scene && scene->IsDuringPlay())
    {
        Builder::OnBrushModified(this);
    }
#endif
}

SceneCSGData::SceneCSGData(Scene* scene)
    : _scene(scene)
    , BuildTime(0)
{
}

void SceneCSGData::BuildCSG(float timeoutMs) const
{
#if COMPILE_WITH_CSG_BUILDER
    Builder::Build(_scene, timeoutMs);
#endif
}
