// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"

/// <summary>Persistent identity of a private scene fragment.</summary>
struct FLAXENGINE_API SceneFragmentId
{
    Guid OwnerSceneGuid = Guid::Empty;
    int64 RootActorLocalId = 0;

    bool IsValid() const
    {
        return OwnerSceneGuid.IsValid() && RootActorLocalId > 1;
    }

    bool operator==(const SceneFragmentId& other) const
    {
        return OwnerSceneGuid == other.OwnerSceneGuid && RootActorLocalId == other.RootActorLocalId;
    }
};
