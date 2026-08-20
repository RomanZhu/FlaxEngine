// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

class ArtifactBuildContext;

/// <summary>One file produced inside a job-owned staging directory.</summary>
struct FLAXENGINE_API StagedArtifactFile
{
    StringAnsi OutputKind;
    String RelativePath;
    String AbsolutePath;
    uint64 Size = 0;
    ContentHash Hash;
};

/// <summary>Capability object that can write only one declared output kind.</summary>
class FLAXENGINE_API ArtifactWriter
{
    friend class ArtifactBuildContext;

private:
    ArtifactBuildContext* _context = nullptr;
    StringAnsi _outputKind;

public:
    ArtifactWriter() = default;

    bool IsOpen() const
    {
        return _context != nullptr;
    }

    /// <summary>Writes a relative file below this output's staging directory. Returns true on failure.</summary>
    bool WriteFile(const StringView& relativePath, const void* data, int32 length, AssetPipelineDiagnostic& diagnostic);

    void Close()
    {
        _context = nullptr;
        _outputKind.Clear();
    }
};
