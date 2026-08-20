// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "CSGScopeActor.h"

/// <summary>
/// A CSG scope actor that establishes a hard Boolean interaction boundary.
/// Brushes inside this stack evaluate against each other in deterministic order,
/// but never perform Boolean operations against brushes in other stacks.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/CSG/CSG Stack\"), ActorToolbox(\"CSG\")")
class FLAXENGINE_API CSGStack : public CSGScopeActor
{
    DECLARE_SCENE_OBJECT(CSGStack);

public:
    // [CSGScopeActor]
    CSGScopeKind GetCSGScopeKind() const override
    {
        return CSGScopeKind::BooleanStack;
    }
};
