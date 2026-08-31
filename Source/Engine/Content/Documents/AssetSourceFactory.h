// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Parameters used to create or replace a canonical authored source document.</summary>
struct AssetCreationParameters
{
    String TypeName;
    StringAnsi Payload;
};

/// <summary>Collects canonical source bytes produced by an asset source factory.</summary>
class SourceDocumentWriter
{
public:
    StringAnsi Text;
};

/// <summary>Creates canonical source documents. Runtime artifacts are produced only by importers.</summary>
class FLAXENGINE_API AssetSourceFactory
{
public:
    virtual ~AssetSourceFactory() = default;

    virtual StringView GetDefaultExtension() const = 0;
    virtual bool WriteInitialSource(SourceDocumentWriter& writer, const AssetCreationParameters& parameters,
        AssetPipelineDiagnostic& diagnostic) const = 0;

    /// <summary>Writes a source document, creates or preserves its metadata, and imports it synchronously.</summary>
    static bool CreateOrReplace(const StringView& path, const AssetCreationParameters& parameters,
        AssetPipelineDiagnostic& diagnostic);
};
