// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "GroupActor.h"
#include "Engine/Level/CSG/CSGScopeTypes.h"

/// <summary>
/// Abstract base actor for CSG semantic and compiler scope boundaries.
/// </summary>
API_CLASS(Abstract)
class FLAXENGINE_API CSGScopeActor : public GroupActor
{
    DECLARE_SCENE_OBJECT_ABSTRACT(CSGScopeActor);

public:
    /// <summary>
    /// Gets the CSG scope kind for this actor.
    /// </summary>
    /// <returns>The scope kind.</returns>
    API_PROPERTY() virtual CSGScopeKind GetCSGScopeKind() const = 0;

    /// <summary>
    /// Gets whether this scope defines a Boolean interaction boundary.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool IsBooleanScope() const
    {
        return GetCSGScopeKind() == CSGScopeKind::BooleanStack;
    }

    /// <summary>
    /// Gets whether this scope defines an independent generated output boundary.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool IsOutputScope() const
    {
        return GetCSGScopeKind() == CSGScopeKind::ModelOutput;
    }
};
