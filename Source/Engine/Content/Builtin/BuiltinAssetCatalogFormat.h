// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/Span.h"

/// <summary>One persistent entry in a built-in asset catalog.</summary>
struct FLAXENGINE_API BuiltinAssetCatalogSerializedEntry
{
    Guid ID;
    String TypeName;
    String RelativePath;
    String Uri;
};

/// <summary>Deterministic built-in catalog serialization keyed by persistent object identity.</summary>
class FLAXENGINE_API BuiltinAssetCatalogFormat
{
public:
    static constexpr uint32 Version = 3;

    /// <summary>Serializes catalog entries by their persistent object GUID.</summary>
    /// <returns>True on failure.</returns>
    static bool ToBytes(const Array<BuiltinAssetCatalogSerializedEntry>& entries, Array<byte>& output,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Parses and validates catalog bytes. Catalogs from earlier identity formats are rejected.</summary>
    /// <returns>True on failure.</returns>
    static bool FromBytes(const Span<byte>& input, Array<BuiltinAssetCatalogSerializedEntry>& entries,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Returns true for superseded catalog formats that did not persist object GUID identity directly.</summary>
    static bool IsLegacyVersion(const Span<byte>& input);
};
