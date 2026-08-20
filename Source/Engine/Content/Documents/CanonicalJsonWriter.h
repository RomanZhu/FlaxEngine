// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Serialization/Json.h"

/// <summary>Stable canonical JSON validation failures.</summary>
enum class CanonicalJsonErrorCode : byte
{
    None,
    ParseError,
    DuplicateKey,
    NonFiniteNumber,
};

/// <summary>Canonical JSON validation error.</summary>
struct FLAXENGINE_API CanonicalJsonError
{
    CanonicalJsonErrorCode Code = CanonicalJsonErrorCode::None;
    int32 Offset = -1;
    String Path;
    String Message;
};

/// <summary>
/// Deterministic UTF-8 JSON formatter shared by sidecars and authored text documents.
/// </summary>
class FLAXENGINE_API CanonicalJsonWriter
{
public:
    /// <summary>Parses, validates, and writes canonical JSON.</summary>
    /// <returns>True on failure.</returns>
    static bool Canonicalize(const StringAnsiView& input, StringAnsi& output, CanonicalJsonError& error, const Array<StringAnsi>* rootFieldOrder = nullptr);

    /// <summary>Validates and writes an existing DOM as canonical JSON.</summary>
    /// <returns>True on failure.</returns>
    static bool Write(const rapidjson_flax::Value& value, StringAnsi& output, CanonicalJsonError& error, const Array<StringAnsi>* rootFieldOrder = nullptr, const Dictionary<StringAnsi, Array<StringAnsi>>* objectFieldOrders = nullptr);

    /// <summary>Rejects duplicate keys and non-finite values recursively.</summary>
    /// <returns>True on failure.</returns>
    static bool Validate(const rapidjson_flax::Value& value, CanonicalJsonError& error);
};
