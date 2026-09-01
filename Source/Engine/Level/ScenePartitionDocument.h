// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonFwd.h"

/// <summary>Private scene-fragment payload validation and runtime composition.</summary>
namespace ScenePartitionDocument
{
    /// <summary>Validates one private scene fragment and returns its root and object payload.</summary>
    bool ReadFragment(const rapidjson_flax::Value& fragment, int64& rootActorLocalId, const rapidjson_flax::Value*& objects, String& error);

    /// <summary>Appends one private fragment payload to a runtime scene object table.</summary>
    bool AppendRuntimeObjects(const rapidjson_flax::Value& fragment, int64 expectedRootActorLocalId,
        rapidjson_flax::Value& runtimeObjects, rapidjson_flax::Document::AllocatorType& allocator, String& error);
}
