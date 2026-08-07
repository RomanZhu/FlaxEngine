// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "EmptyActor.h"

/// <summary>
/// An empty actor that establishes a semantic editing and viewport selection boundary for its children.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Group\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API GroupActor : public EmptyActor
{
    DECLARE_SCENE_OBJECT(GroupActor);

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override;
#endif
};
