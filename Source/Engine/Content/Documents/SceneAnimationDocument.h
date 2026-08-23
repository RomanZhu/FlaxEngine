// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"

/// <summary>Semantic text codec for scene animation timelines.</summary>
class FLAXENGINE_API SceneAnimationDocument
{
public:
    /// <summary>Decodes legacy timeline bytes into explicit tracks and keyframes.</summary>
    static bool DecodeLegacy(const Span<byte>& timeline, rapidjson_flax::Document& document, String& error);

    /// <summary>Compiles explicit tracks and keyframes into runtime timeline bytes.</summary>
    static bool Compile(const rapidjson_flax::Value& document, Array<byte>& timeline, Array<Guid>* references, String& error);
};
