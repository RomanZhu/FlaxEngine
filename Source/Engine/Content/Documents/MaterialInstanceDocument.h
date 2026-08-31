// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"

/// <summary>Semantic text codec for material instance authored documents.</summary>
class FLAXENGINE_API MaterialInstanceDocument
{
public:
    /// <summary>Projects the current runtime representation into the canonical authored document.</summary>
    static bool DecodeRuntime(const Span<byte>& chunk, rapidjson_flax::Document& document, String& error)
    {
        return DecodeLegacy(chunk, document, error);
    }

    /// <summary>Decodes a legacy runtime chunk into the canonical authored document.</summary>
    static bool DecodeLegacy(const Span<byte>& chunk, rapidjson_flax::Document& document, String& error);

    /// <summary>Compiles a canonical authored document into the runtime chunk.</summary>
    static bool Compile(const rapidjson_flax::Value& document, Array<byte>& chunk, Array<Guid>* references, String& error);
};
