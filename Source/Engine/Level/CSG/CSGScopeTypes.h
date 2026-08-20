// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// The kind of CSG semantic/compiler scope.
/// </summary>
API_ENUM() enum class CSGScopeKind : uint8
{
    /// <summary>
    /// Boolean interaction scope. Brushes inside this stack interact with one another.
    /// </summary>
    BooleanStack = 0,

    /// <summary>
    /// Generated output scope. Owns independent model and collision output.
    /// </summary>
    ModelOutput = 1,
};

/// <summary>
/// The kind of CSG compilation/rebuild target.
/// </summary>
API_ENUM() enum class CSGBuildTargetKind : uint8
{
    /// <summary>
    /// Legacy scene output target.
    /// </summary>
    LegacyScene = 0,

    /// <summary>
    /// Independent CSGModel actor output target.
    /// </summary>
    Model = 1,
};

/// <summary>
/// Unique identifier for a CSG build target.
/// </summary>
API_STRUCT() struct FLAXENGINE_API CSGBuildTargetKey
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(CSGBuildTargetKey);

    /// <summary>
    /// The target kind.
    /// </summary>
    API_FIELD() CSGBuildTargetKind Kind = CSGBuildTargetKind::LegacyScene;

    /// <summary>
    /// The owning scene identifier.
    /// </summary>
    API_FIELD() Guid SceneId = Guid::Empty;

    /// <summary>
    /// The target owner identifier (Scene ID for LegacyScene, CSGModel actor ID for Model).
    /// </summary>
    API_FIELD() Guid OwnerId = Guid::Empty;

public:
    CSGBuildTargetKey() = default;

    CSGBuildTargetKey(CSGBuildTargetKind kind, const Guid& sceneId, const Guid& ownerId)
        : Kind(kind)
        , SceneId(sceneId)
        , OwnerId(ownerId)
    {
    }

    bool operator==(const CSGBuildTargetKey& other) const
    {
        return Kind == other.Kind && SceneId == other.SceneId && OwnerId == other.OwnerId;
    }

    bool operator!=(const CSGBuildTargetKey& other) const
    {
        return !(*this == other);
    }

    bool IsValid() const
    {
        return SceneId.IsValid() && OwnerId.IsValid();
    }
};

inline uint32 GetHash(const CSGBuildTargetKey& key)
{
    uint32 hash = (uint32)key.Kind;
    CombineHash(hash, GetHash(key.SceneId));
    CombineHash(hash, GetHash(key.OwnerId));
    return hash;
}
