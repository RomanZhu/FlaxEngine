// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "SceneFragmentIndex.h"
#include "SceneFragmentDiagnostics.h"

/// <summary>One deterministic fragment produced by scene serialization.</summary>
struct FLAXENGINE_API SceneFragmentWrite
{
    int64 RootActorLocalId = 0;
    Array<int64> ContainedLocalIds;
    Array<byte> Payload;
    uint32 SerializerVersion = 1;
};

/// <summary>Private scene-GUID-keyed fragment storage.</summary>
class FLAXENGINE_API SceneFragmentStore
{
public:
    static constexpr uint32 FragmentFormatVersion = 1;

    static String GetRootPath();
    static String GetScenePath(const Guid& sceneGuid);
    static String GetIndexPath(const Guid& sceneGuid);
    static String GetRelativeFragmentPath(int64 rootActorLocalId);

    /// <summary>Loads and validates the index and every referenced fragment. Returns true on failure.</summary>
    static bool Load(const Guid& sceneGuid, SceneFragmentIndex& index, Array<Array<byte>>& fragments, String& error);

    /// <summary>Publishes changed fragments followed by their index and removes stale indexed files. Returns true on failure.</summary>
    static bool Save(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments, String& error);

    /// <summary>Removes the private fragment directory owned by a scene. Returns true on failure.</summary>
    static bool Delete(const Guid& sceneGuid, String& error);

    static bool ReadIndex(const Guid& sceneGuid, SceneFragmentIndex& index, String& error);
};
