// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactWriter.h"
#include "PreparedAsset.h"
#include "Engine/Core/NonCopyable.h"

/// <summary>Engine-owned mapping from a declared input identity to a validated file handle path.</summary>
struct FLAXENGINE_API ArtifactBuildInput
{
    String StableIdentity;
    String Path;
    ContentHash ExpectedContent;
};

/// <summary>Controlled Build-stage access to declared inputs and job staging only.</summary>
class FLAXENGINE_API ArtifactBuildContext : public NonCopyable
{
public:
    ArtifactBuildContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
        const Guid& jobId, const PreparedAsset& prepared, const Array<ArtifactBuildInput>& inputs,
        const AssetCancellationToken& cancellation, uint64 maximumInputBytes = 1024ull * 1024ull * 1024ull,
        uint64 maximumOutputBytes = 4ull * 1024ull * 1024ull * 1024ull, int32 maximumOutputFiles = 4096,
        const ArtifactTarget& target = ArtifactTarget());
    ArtifactBuildContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
        const Guid& jobId, const PreparedAsset& prepared, const Array<ArtifactBuildInput>& inputs,
        const AssetCancellationToken& cancellation, const ArtifactTarget& target);
    ~ArtifactBuildContext();

    /// <summary>Creates a unique staging directory and validates all input capabilities.</summary>
    bool Initialize(AssetPipelineDiagnostic& diagnostic);

    /// <summary>Reads one declared input through its stable identity. Returns true on failure.</summary>
    bool ReadInput(const StringView& stableIdentity, Array<byte>& data, ContentHash& hash, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Returns the engine-validated path for a declared input capability.</summary>
    bool TryGetInputPath(const StringView& stableIdentity, String& path, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Allocates a unique scratch path below this job for trusted compatibility adapters.</summary>
    bool CreateScratchFilePath(const StringView& extension, String& path, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Creates a writer capability for one declared output kind.</summary>
    bool OpenOutput(const StringAnsiView& outputKind, ArtifactWriter& writer, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Seals a successful staging directory after verifying every declared output has files.</summary>
    bool Close(AssetPipelineDiagnostic& diagnostic);

    /// <summary>Cancels work and removes only this job's staging directory.</summary>
    void Cancel();

    bool IsInitialized() const
    {
        return _initialized;
    }
    bool IsClosed() const
    {
        return _closed;
    }
    const String& GetStagingPath() const
    {
        return _stagingPath;
    }
    const String& GetExternalProcessWorkingDirectory() const
    {
        return _stagingPath;
    }
    const Array<StagedArtifactFile>& GetFiles() const
    {
        return _files;
    }
    const Guid& GetJobID() const
    {
        return _jobId;
    }
    const AssetCancellationToken& GetCancellation() const
    {
        return _cancellation;
    }
    const PreparedAsset& GetPreparedAsset() const
    {
        return _prepared;
    }
    const ArtifactTarget& GetTarget() const
    {
        return _target;
    }

private:
    friend class ArtifactWriter;

    String _projectRoot;
    String _contentRoot;
    String _libraryRoot;
    Guid _jobId;
    PreparedAsset _prepared;
    Array<ArtifactBuildInput> _inputs;
    AssetCancellationToken _cancellation;
    ArtifactTarget _target;
    uint64 _maximumInputBytes;
    uint64 _maximumOutputBytes;
    int32 _maximumOutputFiles;
    uint64 _inputBytesRead = 0;
    uint64 _outputBytesWritten = 0;
    String _stagingPath;
    Array<StagedArtifactFile> _files;
    bool _initialized = false;
    bool _closed = false;

    bool WriteOutputFile(const StringAnsiView& outputKind, const StringView& relativePath, const void* data, int32 length, AssetPipelineDiagnostic& diagnostic);
    bool CheckActive(AssetPipelineDiagnostic& diagnostic) const;
};
