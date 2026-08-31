// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Physics/CollisionData.h"
#include "Engine/Serialization/Json.h"

/// <summary>Canonical text recipe for generated collision data.</summary>
class FLAXENGINE_API CollisionDataDocument
{
public:
    /// <summary>Projects the current collision recipe into the canonical authored document.</summary>
    static bool DecodeRuntime(const CollisionData::SerializedOptions& options, rapidjson_flax::Document& document, String& error)
    {
        return DecodeLegacy(options, document, error);
    }

    /// <summary>Decodes the legacy options prefix. Cooked bytes are intentionally not copied into Content text.</summary>
    static bool DecodeLegacy(const CollisionData::SerializedOptions& options, rapidjson_flax::Document& document, String& error);

    /// <summary>Parses and validates a canonical collision recipe.</summary>
    static bool Parse(const rapidjson_flax::Value& document, CollisionData::SerializedOptions& options, String& error);
};
