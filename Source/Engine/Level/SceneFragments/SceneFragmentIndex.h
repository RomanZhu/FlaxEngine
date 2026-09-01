// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "SceneFragmentId.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"

/// <summary>One entry in a scene-owned private fragment index.</summary>
struct FLAXENGINE_API SceneFragmentIndexEntry
{
    int64 RootActorLocalId = 0;
    String RelativePhysicalPath;
    ContentHash Content;
    uint64 Size = 0;
    uint32 SerializerVersion = 0;
};

/// <summary>Authoritative manifest for one scene fragment directory.</summary>
struct FLAXENGINE_API SceneFragmentIndex
{
    static constexpr uint32 CurrentFormatVersion = 1;

    Guid OwnerSceneGuid = Guid::Empty;
    uint32 FormatVersion = CurrentFormatVersion;
    uint64 IndexRevision = 0;
    Array<SceneFragmentIndexEntry> Fragments;
};
