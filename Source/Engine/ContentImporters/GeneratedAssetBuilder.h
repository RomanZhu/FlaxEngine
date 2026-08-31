// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Types.h"

#if COMPILE_WITH_ASSETS_IMPORTER

/// <summary>
/// Writes editor-generated binary cache assets through an isolated temporary file.
/// Canonical project sources and imported artifacts must use the asset pipeline instead.
/// </summary>
class FLAXENGINE_API GeneratedAssetBuilder
{
public:
    /// <summary>If true, generated editor metadata stores project-local source paths relative to the project root.</summary>
    static bool UseRelativeSourcePaths;

    /// <summary>Builds a generated binary cache asset and atomically publishes it at the destination.</summary>
    static bool Build(const CreateAssetFunction& callback, const StringView& outputPath, const StringView& expectedType,
        Guid& assetID, void* argument = nullptr);

    /// <summary>Builds a generated binary cache asset from an explicit source file.</summary>
    static bool BuildFromSource(const CreateAssetFunction& callback, const StringView& sourcePath, const StringView& outputPath,
        const StringView& expectedType, Guid& assetID, void* argument = nullptr);

    /// <summary>Builds only when the source is newer than the generated destination.</summary>
    static bool BuildFromSourceIfModified(const CreateAssetFunction& callback, const StringView& sourcePath, const StringView& outputPath,
        const StringView& expectedType, Guid& assetID, void* argument = nullptr);

    /// <summary>Normalizes an editor-only source path for generated metadata.</summary>
    static String GetSourceReference(const String& path);

private:
    static bool BuildInternal(const CreateAssetFunction& callback, const StringView& sourcePath, const StringView& outputPath,
        const StringView& expectedType, Guid& assetID, void* argument);
};

#endif
