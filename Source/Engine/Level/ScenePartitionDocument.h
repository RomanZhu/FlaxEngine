// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonFwd.h"

/// <summary>One authored scene partition source reference.</summary>
struct ScenePartitionReference
{
    AssetObjectId Object;
    int64 RootFileId = 0;
};

/// <summary>Canonical authored scene-partition document validation and composition.</summary>
namespace ScenePartitionDocument
{
    /// <summary>Validates and extracts the partition table from an authored scene document.</summary>
    bool ReadSceneReferences(const rapidjson_flax::Value& scene, Array<ScenePartitionReference>& references, String& error);

    /// <summary>Validates one authored scene chunk and returns its root and object table.</summary>
    bool ReadChunk(const rapidjson_flax::Value& chunk, int64& rootFileId, const rapidjson_flax::Value*& objects, String& error);

    /// <summary>Appends one canonical chunk object table to a runtime scene object table.</summary>
    bool AppendRuntimeObjects(const rapidjson_flax::Value& chunk, int64 expectedRootFileId,
        rapidjson_flax::Value& runtimeObjects, rapidjson_flax::Document::AllocatorType& allocator, String& error);
}
