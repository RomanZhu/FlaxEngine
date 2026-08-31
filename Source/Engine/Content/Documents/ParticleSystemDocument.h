// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"

/// <summary>Semantic text codec for particle-system timelines.</summary>
class FLAXENGINE_API ParticleSystemDocument
{
public:
    /// <summary>Projects the current runtime timeline into the canonical authored document.</summary>
    static bool DecodeRuntime(const Span<byte>& timeline, rapidjson_flax::Document& document, String& error)
    {
        return DecodeLegacy(timeline, document, error);
    }

    /// <summary>Decodes legacy particle-system timeline bytes into explicit tracks and overrides.</summary>
    static bool DecodeLegacy(const Span<byte>& timeline, rapidjson_flax::Document& document, String& error);

    /// <summary>Compiles explicit particle-system tracks and overrides into runtime timeline bytes.</summary>
    static bool Compile(const rapidjson_flax::Value& document, Array<byte>& timeline, Array<Guid>* references, String& error);
};
