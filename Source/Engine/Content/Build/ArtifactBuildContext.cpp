// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactBuildContext.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    void SetBuildFailure(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const PreparedAsset& prepared, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = prepared.AssetID;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
    }
}

bool ArtifactWriter::WriteFile(const StringView& relativePath, const void* data, int32 length, AssetPipelineDiagnostic& diagnostic)
{
    if (!_context)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.Message = TEXT("Artifact writer is closed.");
        return true;
    }
    return _context->WriteOutputFile(_outputKind, relativePath, data, length, diagnostic);
}

bool ArtifactWriter::WriteFileFromPath(const StringView& relativePath, const StringView& sourcePath,
    uint64 expectedSize, const ContentHash& expectedHash, AssetPipelineDiagnostic& diagnostic)
{
    if (!_context)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.Message = TEXT("Artifact writer is closed.");
        return true;
    }
    return _context->WriteOutputFileFromPath(_outputKind, relativePath, sourcePath, expectedSize, expectedHash, diagnostic);
}

ArtifactBuildContext::ArtifactBuildContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
    const Guid& jobId, const PreparedAsset& prepared, const Array<ArtifactBuildInput>& inputs,
    const AssetCancellationToken& cancellation, uint64 maximumInputBytes, uint64 maximumOutputBytes, int32 maximumOutputFiles,
    const ArtifactTarget& target)
    : _projectRoot(projectRoot)
    , _contentRoot(contentRoot)
    , _libraryRoot(libraryRoot)
    , _jobId(jobId)
    , _prepared(prepared)
    , _inputs(inputs)
    , _cancellation(cancellation)
    , _target(target)
    , _maximumInputBytes(maximumInputBytes)
    , _maximumOutputBytes(maximumOutputBytes)
    , _maximumOutputFiles(maximumOutputFiles)
{
}

ArtifactBuildContext::ArtifactBuildContext(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
    const Guid& jobId, const PreparedAsset& prepared, const Array<ArtifactBuildInput>& inputs,
    const AssetCancellationToken& cancellation, const ArtifactTarget& target)
    : ArtifactBuildContext(projectRoot, contentRoot, libraryRoot, jobId, prepared, inputs, cancellation,
        1024ull * 1024ull * 1024ull, 4ull * 1024ull * 1024ull * 1024ull, 4096, target)
{
}

ArtifactBuildContext::~ArtifactBuildContext()
{
    if (_initialized && !_closed)
        Cancel();
}

bool ArtifactBuildContext::CheckActive(AssetPipelineDiagnostic& diagnostic) const
{
    if (_cancellation.IsCancellationRequested())
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, _prepared, _stagingPath, TEXT("Artifact build was cancelled."));
        return true;
    }
    if (!_initialized || _closed)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, _stagingPath, TEXT("Artifact build context is not active."));
        return true;
    }
    return false;
}

bool ArtifactBuildContext::Initialize(AssetPipelineDiagnostic& diagnostic)
{
    if (_initialized || !_jobId.IsValid() || !_prepared.AssetID.IsValid())
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, _libraryRoot, TEXT("Artifact build context has invalid or repeated initialization."));
        return true;
    }
    if (_cancellation.IsCancellationRequested())
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, _prepared, _libraryRoot, TEXT("Artifact build was cancelled before staging creation."));
        return true;
    }
    if (ArtifactStore::EnsureLayout(_libraryRoot, diagnostic))
        return true;

    for (int32 i = 0; i < _inputs.Count(); i++)
    {
        ArtifactBuildInput& input = _inputs[i];
        const AssetDependency* declaration = nullptr;
        for (const AssetDependency& dependency : _prepared.Dependencies)
        {
            if (dependency.StableIdentity == input.StableIdentity && dependency.Kind != AssetDependencyKind::RuntimeReference)
            {
                if (declaration)
                {
                    SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, input.Path, TEXT("Build input identity is ambiguous across prepared declarations."));
                    return true;
                }
                declaration = &dependency;
            }
        }
        if (!declaration)
        {
            SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, input.Path, TEXT("Build input was not declared during Prepare."));
            return true;
        }
        for (int32 previous = 0; previous < i; previous++)
        {
            if (_inputs[previous].StableIdentity == input.StableIdentity)
            {
                SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, input.Path, TEXT("Build input capability is duplicated."));
                return true;
            }
        }
        if (declaration->Kind == AssetDependencyKind::SourceFile)
        {
            AssetPathPolicy::ProjectPath normalized;
            if (AssetPathPolicy::TryNormalizeProjectPath(_projectRoot, _contentRoot, _libraryRoot, input.Path, normalized, diagnostic) || normalized.ProjectRelativePath != declaration->StableIdentity)
            {
                SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, input.Path, TEXT("Source input path does not match its prepared canonical identity."));
                return true;
            }
            input.Path = normalized.AbsolutePath;
            input.ExpectedContent = declaration->Content;
        }
        else if (declaration->Kind == AssetDependencyKind::BuildInput)
        {
            if (!AssetPathPolicy::IsArtifactPathValid(ArtifactStoragePath(input.Path), _libraryRoot) || input.ExpectedContent.IsZero())
            {
                SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, input.Path, TEXT("Artifact build input is outside Library or lacks an expected content hash."));
                return true;
            }
        }
        else
        {
            SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, input.Path, TEXT("Toolchain declarations are identities, not arbitrary Build-stage file handles."));
            return true;
        }
    }

    const String jobsPath = ArtifactStore::GetTemporaryPath(_libraryRoot) / TEXT("Jobs");
    if ((!FileSystem::DirectoryExists(jobsPath) && FileSystem::CreateDirectory(jobsPath)) || FileSystem::FileExists(jobsPath))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, _prepared, jobsPath, TEXT("Cannot create artifact job staging root."));
        return true;
    }
    ArtifactStoragePath stagingPath;
    if (ArtifactStore::TryGetJobStagingPath(_libraryRoot, _jobId, stagingPath, diagnostic))
        return true;
    _stagingPath = stagingPath.Get();
    if (FileSystem::DirectoryExists(_stagingPath) || FileSystem::FileExists(_stagingPath) || FileSystem::CreateDirectory(_stagingPath))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ArtifactLockBusy, _prepared, _stagingPath, TEXT("Artifact job staging identity is already in use or cannot be created."));
        _stagingPath.Clear();
        return true;
    }
    _initialized = true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ArtifactBuildContext::ReadInput(const StringView& stableIdentity, Array<byte>& data, ContentHash& hash, AssetPipelineDiagnostic& diagnostic)
{
    data.Clear();
    hash = ContentHash();
    if (CheckActive(diagnostic))
        return true;
    const ArtifactBuildInput* selected = nullptr;
    for (const ArtifactBuildInput& input : _inputs)
    {
        if (input.StableIdentity == stableIdentity)
        {
            selected = &input;
            break;
        }
    }
    if (!selected)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, stableIdentity, TEXT("Build attempted to read an undeclared input identity."));
        return true;
    }
    const uint64 size = FileSystem::GetFileSize(selected->Path);
    if (!FileSystem::FileExists(selected->Path))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, _prepared, selected->Path, TEXT("Declared build input is missing."));
        return true;
    }
    if (size > _maximumInputBytes || _inputBytesRead > _maximumInputBytes - size)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, _prepared, selected->Path, TEXT("Artifact build exceeded the input-byte limit."));
        return true;
    }
    if (File::ReadAllBytes(selected->Path, data))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, _prepared, selected->Path, TEXT("Cannot read declared build input."));
        return true;
    }
    hash = ContentHash::Compute(data.Get(), data.Count());
    if (hash != selected->ExpectedContent)
    {
        data.Clear();
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, _prepared, selected->Path, TEXT("Declared build input content does not match its expected hash."));
        return true;
    }
    _inputBytesRead += size;
    return false;
}

bool ArtifactBuildContext::TryGetInputPath(const StringView& stableIdentity, String& path, AssetPipelineDiagnostic& diagnostic) const
{
    path.Clear();
    if (CheckActive(diagnostic))
        return true;
    for (const ArtifactBuildInput& input : _inputs)
    {
        if (input.StableIdentity == stableIdentity)
        {
            path = input.Path;
            return false;
        }
    }
    SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, stableIdentity, TEXT("Build attempted to access an undeclared input path."));
    return true;
}

bool ArtifactBuildContext::CreateScratchFilePath(const StringView& extension, String& path, AssetPipelineDiagnostic& diagnostic) const
{
    path.Clear();
    if (CheckActive(diagnostic))
        return true;
    if (extension.IsEmpty() || extension.Contains(TEXT("/")) || extension.Contains(TEXT("\\")) || extension.Contains(TEXT(":")) || extension.Contains(TEXT("..")))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, _prepared, extension, TEXT("Artifact scratch extension is invalid."));
        return true;
    }
    const String scratchRoot = _stagingPath / TEXT("Scratch");
    if (!FileSystem::DirectoryExists(scratchRoot) && FileSystem::CreateDirectory(scratchRoot))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, _prepared, scratchRoot, TEXT("Cannot create artifact adapter scratch directory."));
        return true;
    }
    path = scratchRoot / (Guid::New().ToString(Guid::FormatType::N) + extension);
    return false;
}

bool ArtifactBuildContext::OpenOutput(const StringAnsiView& outputKind, ArtifactWriter& writer, AssetPipelineDiagnostic& diagnostic)
{
    writer.Close();
    if (CheckActive(diagnostic))
        return true;
    bool declared = false;
    for (const DeclaredArtifactOutput& output : _prepared.Outputs)
    {
        if (output.Kind == outputKind)
        {
            declared = true;
            break;
        }
    }
    if (!declared)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, _stagingPath, TEXT("Build attempted to open an undeclared output kind."));
        diagnostic.OutputKind = String(outputKind);
        return true;
    }
    writer._context = this;
    writer._outputKind = outputKind.ToStringAnsi();
    return false;
}

bool ArtifactBuildContext::WriteOutputFile(const StringAnsiView& outputKind, const StringView& relativePath, const void* data, int32 length, AssetPipelineDiagnostic& diagnostic)
{
    if (CheckActive(diagnostic))
        return true;
    const PackageEntryPath packagePath(relativePath);
    if (length < 0 || relativePath.Contains(TEXT(":")) || !AssetPathPolicy::IsPackageEntryPathValid(packagePath))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, _prepared, relativePath, TEXT("Artifact output path must be a safe relative staging path."));
        diagnostic.OutputKind = String(outputKind);
        return true;
    }
    if (_files.Count() >= _maximumOutputFiles || static_cast<uint64>(length) > _maximumOutputBytes || _outputBytesWritten > _maximumOutputBytes - static_cast<uint64>(length))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, _prepared, relativePath, TEXT("Artifact build exceeded its output file or byte limit."));
        diagnostic.OutputKind = String(outputKind);
        return true;
    }
    int32 outputIndex = -1;
    for (int32 i = 0; i < _prepared.Outputs.Count(); i++)
    {
        if (_prepared.Outputs[i].Kind == outputKind)
        {
            outputIndex = i;
            break;
        }
    }
    if (outputIndex < 0)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, relativePath, TEXT("Artifact output writer no longer matches a declared output."));
        return true;
    }
    String normalized(relativePath);
    normalized.Replace(TEXT('\\'), TEXT('/'));
    const String outputRoot = _stagingPath / TEXT("Outputs") / String::Format(TEXT("{0}"), outputIndex);
    const String absolutePath = outputRoot / normalized;
    if (!AssetPathPolicy::IsSameOrChild(absolutePath, _stagingPath))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, _prepared, absolutePath, TEXT("Artifact output escaped job staging."));
        return true;
    }
    for (const StagedArtifactFile& file : _files)
    {
        if (file.AbsolutePath.Compare(absolutePath, StringSearchCase::IgnoreCase) == 0)
        {
            SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, absolutePath, TEXT("Artifact output file was written more than once."));
            return true;
        }
    }
    const String directory = StringUtils::GetDirectoryName(absolutePath);
    if ((!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory)) || File::WriteAllBytes(absolutePath, data, length))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, absolutePath, TEXT("Cannot write artifact staging output."));
        return true;
    }
    StagedArtifactFile file;
    file.OutputKind = outputKind.ToStringAnsi();
    file.RelativePath = normalized;
    file.AbsolutePath = absolutePath;
    file.Size = length;
    file.Hash = ContentHash::Compute(data, length);
    _files.Add(MoveTemp(file));
    _outputBytesWritten += length;
    return false;
}

bool ArtifactBuildContext::WriteOutputFileFromPath(const StringAnsiView& outputKind, const StringView& relativePath,
    const StringView& sourcePath, uint64 expectedSize, const ContentHash& expectedHash,
    AssetPipelineDiagnostic& diagnostic)
{
    if (CheckActive(diagnostic))
        return true;
    String normalized(relativePath);
    normalized.Replace(TEXT('\\'), TEXT('/'));
    const PackageEntryPath packagePath(normalized);
    if (expectedSize == 0 || expectedHash.IsZero() || normalized.Contains(TEXT(":")) ||
        !AssetPathPolicy::IsPackageEntryPathValid(packagePath))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, _prepared, relativePath,
            TEXT("Artifact file-backed output path or expected content is invalid."));
        diagnostic.OutputKind = String(outputKind);
        return true;
    }
    if (_files.Count() >= _maximumOutputFiles || expectedSize > _maximumOutputBytes ||
        _outputBytesWritten > _maximumOutputBytes - expectedSize)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, _prepared, relativePath,
            TEXT("Artifact build exceeded its output file or byte limit."));
        diagnostic.OutputKind = String(outputKind);
        return true;
    }
    int32 outputIndex = -1;
    for (int32 i = 0; i < _prepared.Outputs.Count(); i++)
    {
        if (_prepared.Outputs[i].Kind == outputKind)
        {
            outputIndex = i;
            break;
        }
    }
    if (outputIndex < 0)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, relativePath,
            TEXT("Artifact output writer no longer matches a declared output."));
        return true;
    }

    String normalizedSource(sourcePath);
    StringUtils::PathRemoveRelativeParts(normalizedSource);
    FileSystem::NormalizePath(normalizedSource);
    String temporaryRoot = ArtifactStore::GetTemporaryPath(_libraryRoot);
    StringUtils::PathRemoveRelativeParts(temporaryRoot);
    FileSystem::NormalizePath(temporaryRoot);
    if (!AssetPathPolicy::IsSameOrChild(normalizedSource, temporaryRoot) ||
        FileSystem::AreFilePathsEqual(normalizedSource, temporaryRoot) ||
        !FileSystem::FileExists(normalizedSource) || FileSystem::GetFileSize(normalizedSource) != expectedSize)
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, _prepared, normalizedSource,
            TEXT("Artifact file-backed output source is outside engine temporary storage or changed size."));
        return true;
    }
    const String outputRoot = _stagingPath / TEXT("Outputs") / String::Format(TEXT("{0}"), outputIndex);
    const String absolutePath = outputRoot / normalized;
    if (!AssetPathPolicy::IsSameOrChild(absolutePath, _stagingPath))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, _prepared, absolutePath,
            TEXT("Artifact output escaped job staging."));
        return true;
    }
    for (const StagedArtifactFile& file : _files)
    {
        if (file.AbsolutePath.Compare(absolutePath, StringSearchCase::IgnoreCase) == 0)
        {
            SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, absolutePath,
                TEXT("Artifact output file was written more than once."));
            return true;
        }
    }
    const String directory = StringUtils::GetDirectoryName(absolutePath);
    if (!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory))
    {
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, absolutePath,
            TEXT("Cannot create artifact staging output directory."));
        return true;
    }
    File* source = File::Open(normalizedSource, FileMode::OpenExisting, FileAccess::Read, FileShare::Read);
    File* destination = File::Open(absolutePath, FileMode::CreateNew, FileAccess::Write, FileShare::Read);
    if (!source || !destination)
    {
        if (source)
            Delete(source);
        if (destination)
            Delete(destination);
        FileSystem::DeleteFile(absolutePath);
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, absolutePath,
            TEXT("Cannot open a file-backed artifact staging output."));
        return true;
    }
    Array<byte> buffer;
    buffer.Resize(256 * 1024, false);
    ContentHasher hasher;
    uint64 copied = 0;
    bool failed = false;
    for (;;)
    {
        uint32 read = 0;
        if (source->Read(buffer.Get(), buffer.Count(), &read) || copied > expectedSize || read > expectedSize - copied)
        {
            failed = true;
            break;
        }
        if (read == 0)
            break;
        uint32 written = 0;
        if (destination->Write(buffer.Get(), read, &written) || written != read)
        {
            failed = true;
            break;
        }
        hasher.Update(buffer.Get(), read);
        copied += read;
    }
    Delete(source);
    Delete(destination);
    const ContentHash actualHash = hasher.Finalize();
    if (failed || copied != expectedSize || actualHash != expectedHash ||
        FileSystem::GetFileSize(normalizedSource) != expectedSize || FileSystem::GetFileSize(absolutePath) != expectedSize)
    {
        FileSystem::DeleteFile(absolutePath);
        SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, _prepared, normalizedSource,
            TEXT("File-backed artifact output changed while it was streamed into staging."));
        return true;
    }
    StagedArtifactFile file;
    file.OutputKind = outputKind.ToStringAnsi();
    file.RelativePath = normalized;
    file.AbsolutePath = absolutePath;
    file.Size = expectedSize;
    file.Hash = actualHash;
    _files.Add(MoveTemp(file));
    _outputBytesWritten += expectedSize;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ArtifactBuildContext::Close(AssetPipelineDiagnostic& diagnostic)
{
    if (CheckActive(diagnostic))
        return true;
    for (const DeclaredArtifactOutput& output : _prepared.Outputs)
    {
        bool found = false;
        for (const StagedArtifactFile& file : _files)
        {
            if (file.OutputKind == output.Kind)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            SetBuildFailure(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, _prepared, _stagingPath, TEXT("A declared artifact output did not produce any files."));
            diagnostic.OutputKind = String(output.Kind);
            return true;
        }
    }
    _closed = true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void ArtifactBuildContext::Cancel()
{
    if (!_stagingPath.IsEmpty() && AssetPathPolicy::IsSameOrChild(_stagingPath, ArtifactStore::GetTemporaryPath(_libraryRoot)) && FileSystem::DirectoryExists(_stagingPath))
        FileSystem::DeleteDirectory(_stagingPath, true);
    _initialized = false;
    _closed = true;
    _files.Clear();
}
