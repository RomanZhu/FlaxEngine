// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonFwd.h"

/// <summary>Canonical authored scene/prefab document conversion.</summary>
namespace ScenePrefabDocument
{
    /// <summary>Validates authored flat object records. Returns true on failure.</summary>
    bool ValidateObjects(const rapidjson_flax::Value& objects, bool scene, String& error);

    /// <summary>Builds a runtime object array from authored object records.</summary>
    bool ToRuntimeObjects(const rapidjson_flax::Value& source, rapidjson_flax::Value& runtime,
                          rapidjson_flax::Document::AllocatorType& allocator, bool scene, String& error);

    /// <summary>Builds canonical authored object records from runtime object records.</summary>
    bool ToSourceObjects(const rapidjson_flax::Value& runtime, rapidjson_flax::Value& source,
                         rapidjson_flax::Document::AllocatorType& allocator, bool scene, String& error);

    /// <summary>Converts a runtime JSON envelope into a canonical authored source document.</summary>
    bool RuntimeEnvelopeToSource(rapidjson_flax::StringBuffer& json, bool scene, bool pretty, String& error);
}
