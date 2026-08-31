// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Types/Guid.h"

/// <summary>
/// Object serialization modification base class. Allows to extend the serialization process by custom effects like object ids mapping.
/// </summary>
class FLAXENGINE_API ISerializeModifier
{
public:
    /// <summary>
    /// Number of engine build when data was serialized. Useful to upgrade data from the older storage format.
    /// </summary>
    uint32 EngineBuild;

    // Utility for scene deserialization to track currently mapped in Prefab Instance object IDs into IdsMapping.
    int32 CurrentInstance;

    /// <summary>
    /// The object IDs mapping. Key is a serialized object id, value is mapped value to use.
    /// </summary>
    Dictionary<Guid, Guid> IdsMapping;

    /// <summary>
    /// Source asset namespace of the scene or prefab document currently being deserialized.
    /// Used only to derive ephemeral runtime keys from authored local file IDs.
    /// </summary>
    Guid CurrentSourceAssetId;

    ISerializeModifier();
};
