// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Config.h"

#if COMPILE_WITH_ASSETS_IMPORTER

#include "Engine/Core/Enums.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Content/Storage/FlaxFile.h"

class JsonWriter;
class CreateAssetContext;

/// <summary>
/// Create/Import new asset callback result
/// </summary>
DECLARE_ENUM_8(CreateAssetResult, Ok, Abort, Error, CannotSaveFile, InvalidPath, CannotAllocateChunk, InvalidTypeID, Skip);

/// <summary>
/// Create/Import new asset callback function
/// </summary>
typedef Function<CreateAssetResult(CreateAssetContext&)> CreateAssetFunction;

/// <summary>
/// Importing/creating asset context structure
/// </summary>
class FLAXENGINE_API CreateAssetContext : public NonCopyable
{
private:
    bool _artifactStagingMode = false;
    Guid _intendedAssetID = Guid::Empty;
    String _intendedTypeName;
    String _artifactOutputPath;

public:
    /// <summary>Returns true when the importer can write only to an isolated build-job staging file.</summary>
    FORCE_INLINE bool IsArtifactStagingMode() const
    {
        return _artifactStagingMode;
    }

    /// <summary>
    /// Path of the input file (may be empty if creating new asset)
    /// </summary>
    String InputPath;

    /// <summary>
    /// Output file path
    /// </summary>
    String OutputPath;

    /// <summary>
    /// Target asset path (may be different than OutputPath)
    /// </summary>
    String TargetAssetPath;

    /// <summary>
    /// Asset file data container
    /// </summary>
    AssetInitData Data;

    /// <summary>
    /// True if skip the default asset import metadata added by the importer. May generate unwanted version control diffs.
    /// </summary>
    bool SkipMetadata;

    /// <summary>
    /// Custom argument for the importing function
    /// </summary>
    void* CustomArg;

    // TODO: add Progress(float progress) to notify operation progress
    // TODO: add cancellation feature - so progress can be aborted on demand

public:
    /// <summary>Creates an importer context that can publish only to a staging file.</summary>
    CreateAssetContext(const StringView& inputPath, const StringView& outputPath, const Guid& id, void* arg, bool artifactStagingMode, const StringView& intendedTypeName);

    /// <summary>
    /// Finalizes an instance of the <see cref="CreateAssetContext"/> class.
    /// </summary>
    ~CreateAssetContext()
    {
    }

public:
    /// <summary>
    /// Runs the specified callback.
    /// </summary>
    /// <param name="callback">The import/create asset callback.</param>
    /// <returns>Operation result.</returns>
    CreateAssetResult Run(const CreateAssetFunction& callback);

public:
    /// <summary>
    /// Allocates the chunk in the output data so upgrader can write to it.
    /// </summary>
    /// <param name="index">The index of the chunk.</param>
    /// <returns>True if cannot allocate it.</returns>
    bool AllocateChunk(int32 index);

};

#define IMPORT_SETUP(type, serializedVersion) context.Data.Header.TypeName = type::TypeName; context.Data.SerializedVersion = serializedVersion;

#endif
