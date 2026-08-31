// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportWorkerProtocol.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/AssetDatabase/SubAsset.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>

namespace
{
    constexpr uint32 ProtocolMagic = 0x57494146; // FAIW
    constexpr uint32 MaximumProtocolItems = 1024 * 1024;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.Message = message;
        return true;
    }

    class ProtocolWriter
    {
    public:
        Array<byte> Data;

        template<typename T>
        void Pod(const T& value)
        {
            Data.Add(reinterpret_cast<const byte*>(&value), sizeof(T));
        }

        void Hash(const ContentHash& value)
        {
            Data.Add(value.Bytes, sizeof(value.Bytes));
        }

        void StringValue(const StringView& value)
        {
            const StringAnsi utf8(value);
            const uint32 length = static_cast<uint32>(utf8.Length());
            Pod(length);
            Data.Add(reinterpret_cast<const byte*>(utf8.Get()), length);
        }

        void AnsiValue(const StringAnsiView& value)
        {
            const uint32 length = static_cast<uint32>(value.Length());
            Pod(length);
            Data.Add(reinterpret_cast<const byte*>(value.Get()), length);
        }

        void Bytes(const Array<byte>& value)
        {
            const uint64 length = value.Count();
            Pod(length);
            Data.Add(value.Get(), value.Count());
        }
    };

    class ProtocolReader
    {
        const byte* _data;
        uint64 _length;
        uint64 _position = 0;

    public:
        ProtocolReader(const byte* data, uint64 length)
            : _data(data)
            , _length(length)
        {
        }

        template<typename T>
        bool Pod(T& value)
        {
            if (_position + sizeof(T) > _length)
                return true;
            Platform::MemoryCopy(&value, _data + _position, sizeof(T));
            _position += sizeof(T);
            return false;
        }

        bool Hash(ContentHash& value)
        {
            if (_position + sizeof(value.Bytes) > _length)
                return true;
            Platform::MemoryCopy(value.Bytes, _data + _position, sizeof(value.Bytes));
            _position += sizeof(value.Bytes);
            return false;
        }

        bool StringValue(String& value)
        {
            uint32 length;
            if (Pod(length) || length > MAX_int32 || _position + length > _length)
                return true;
            value = String(StringAnsiView(reinterpret_cast<const char*>(_data + _position), static_cast<int32>(length)));
            _position += length;
            return false;
        }

        bool AnsiValue(StringAnsi& value)
        {
            uint32 length;
            if (Pod(length) || length > MAX_int32 || _position + length > _length)
                return true;
            value = StringAnsi(reinterpret_cast<const char*>(_data + _position), static_cast<int32>(length));
            _position += length;
            return false;
        }

        bool Bytes(Array<byte>& value)
        {
            uint64 length;
            if (Pod(length) || length > MAX_int32 || _position + length > _length)
                return true;
            value.Resize(static_cast<int32>(length));
            if (length)
                Platform::MemoryCopy(value.Get(), _data + _position, length);
            _position += length;
            return false;
        }

        bool Count(uint32& count)
        {
            return Pod(count) || count > MaximumProtocolItems;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };

    bool WriteAtomic(const StringView& path, const Array<byte>& data)
    {
        const String staging = String(path) + TEXT(".tmp-") + Guid::New().ToString(Guid::FormatType::N);
        if (File::WriteAllBytes(staging, data.Get(), data.Count()) || FileSystem::MoveFile(path, staging, true))
        {
            FileSystem::DeleteFile(staging);
            return true;
        }
        return false;
    }

    void WriteDiagnostic(ProtocolWriter& writer, const AssetPipelineDiagnostic& value)
    {
        writer.Pod(value.SchemaVersion);
        writer.Pod(value.Code);
        writer.Pod(value.Severity);
        writer.Pod(value.Stage);
        writer.Pod(value.AssetGuid);
        writer.StringValue(value.SourcePath);
        writer.StringValue(value.ProcessorId);
        writer.StringValue(value.Target);
        writer.StringValue(value.OutputKind);
        writer.StringValue(value.Location.File);
        writer.Pod(value.Location.Line);
        writer.Pod(value.Location.Column);
        writer.StringValue(value.Location.GraphNode);
        writer.StringValue(value.Location.GraphPin);
        writer.StringValue(value.Message);
        writer.StringValue(value.Remediation);
        const uint32 count = value.Related.Count();
        writer.Pod(count);
        for (const String& related : value.Related)
            writer.StringValue(related);
    }

    bool ReadDiagnostic(ProtocolReader& reader, AssetPipelineDiagnostic& value)
    {
        uint32 count;
        if (reader.Pod(value.SchemaVersion) || reader.Pod(value.Code) || reader.Pod(value.Severity) || reader.Pod(value.Stage) ||
            reader.Pod(value.AssetGuid) || reader.StringValue(value.SourcePath) || reader.StringValue(value.ProcessorId) ||
            reader.StringValue(value.Target) || reader.StringValue(value.OutputKind) || reader.StringValue(value.Location.File) ||
            reader.Pod(value.Location.Line) || reader.Pod(value.Location.Column) || reader.StringValue(value.Location.GraphNode) ||
            reader.StringValue(value.Location.GraphPin) || reader.StringValue(value.Message) || reader.StringValue(value.Remediation) || reader.Count(count))
            return true;
        value.Related.Resize(count);
        for (String& related : value.Related)
        {
            if (reader.StringValue(related))
                return true;
        }
        return false;
    }

    void WriteDependency(ProtocolWriter& writer, const AssetImportDependency& value)
    {
        writer.Pod(value.Kind);
        writer.Pod(value.Object.Asset.Value);
        writer.Pod(value.Object.LocalId);
        writer.StringValue(value.Identity);
        writer.Hash(value.ExpectedHash);
        writer.Hash(value.ExactArtifact.Digest);
        writer.StringValue(value.Origin);
    }

    bool ReadDependency(ProtocolReader& reader, AssetImportDependency& value)
    {
        Guid asset;
        if (reader.Pod(value.Kind) || reader.Pod(asset) || reader.Pod(value.Object.LocalId) || reader.StringValue(value.Identity) ||
            reader.Hash(value.ExpectedHash) || reader.Hash(value.ExactArtifact.Digest) || reader.StringValue(value.Origin))
            return true;
        value.Object.Asset = AssetGuid(asset);
        return false;
    }

    bool IsSafeRelativePath(const StringView& path)
    {
        if (path.IsEmpty() || path[0] == TEXT('/') || path[0] == TEXT('\\') || path.Find(TEXT(':')) != -1)
            return false;
        int32 start = 0;
        for (int32 i = 0; i <= path.Length(); i++)
        {
            if (i != path.Length() && path[i] != TEXT('/') && path[i] != TEXT('\\'))
                continue;
            const StringView segment(path.Get() + start, i - start);
            if (segment.IsEmpty() || segment == TEXT(".") || segment == TEXT(".."))
                return false;
            start = i + 1;
        }
        return true;
    }

    bool AddWithoutOverflow(uint64& total, uint64 value)
    {
        if (value > MAX_uint64 - total)
            return true;
        total += value;
        return false;
    }
}

bool AssetImportWorkerProtocol::SaveRequest(const StringView& path, const AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic)
{
    if (ValidateRequest(request, diagnostic))
        return true;
    ProtocolWriter writer;
    writer.Pod(ProtocolMagic);
    writer.Pod(request.ProtocolVersion);
    writer.Pod(request.JobID);
    writer.Pod(request.Capability);
    writer.Pod(request.Asset.Value);
    writer.Pod(request.SourceRevision);
    writer.StringValue(request.SourcePath);
    writer.Hash(request.SourceHash);
    writer.Bytes(request.SourceSnapshot);
    writer.Hash(request.MetaHash);
    writer.Bytes(request.MetaSnapshot);
    writer.StringValue(request.Importer.ID);
    writer.Pod(request.Importer.ProviderKind);
    writer.Pod(request.Importer.ImporterVersion);
    writer.Pod(request.Importer.SettingsSchemaVersion);
    writer.Hash(request.Importer.ImplementationHash);
    writer.Pod(request.Importer.ProducesMainObject);
    writer.Pod(request.Importer.ProducesSubObjects);
    writer.Pod(request.Importer.PathSensitive);
    writer.StringValue(request.Target);
    uint32 count = request.AuthorizedInputs.Count();
    writer.Pod(count);
    for (const AssetImportWorkerInput& input : request.AuthorizedInputs)
    {
        writer.StringValue(input.Identity);
        writer.StringValue(input.CanonicalPath);
        writer.Hash(input.Hash);
        writer.Bytes(input.Snapshot);
    }
    count = request.AllowedTools.Count();
    writer.Pod(count);
    for (const AssetImportWorkerTool& tool : request.AllowedTools)
    {
        writer.StringValue(tool.Name);
        writer.Hash(tool.VersionHash);
    }
    writer.StringValue(request.OutputStagingPath);
    writer.Pod(request.Limits.MaximumInputBytes);
    writer.Pod(request.Limits.MaximumOutputBytes);
    writer.Pod(request.Limits.MaximumMemoryBytes);
    writer.Pod(request.Limits.MaximumOutputFiles);
    writer.Pod(request.Limits.TimeoutMilliseconds);
    if (WriteAtomic(path, writer.Data))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, TEXT("Cannot write the isolated import request atomically."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImportWorkerProtocol::LoadRequest(const StringView& path, AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic)
{
    Array<byte> data;
    if (File::ReadAllBytes(path, data))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, TEXT("Cannot read the isolated import request."));
    ProtocolReader reader(data.Get(), data.Count());
    uint32 magic, count;
    if (reader.Pod(magic) || magic != ProtocolMagic || reader.Pod(request.ProtocolVersion) || reader.Pod(request.JobID) ||
        reader.Pod(request.Capability) || reader.Pod(request.Asset.Value) || reader.Pod(request.SourceRevision) ||
        reader.StringValue(request.SourcePath) || reader.Hash(request.SourceHash) || reader.Bytes(request.SourceSnapshot) ||
        reader.Hash(request.MetaHash) || reader.Bytes(request.MetaSnapshot) || reader.StringValue(request.Importer.ID) ||
        reader.Pod(request.Importer.ProviderKind) ||
        reader.Pod(request.Importer.ImporterVersion) || reader.Pod(request.Importer.SettingsSchemaVersion) ||
        reader.Hash(request.Importer.ImplementationHash) || reader.Pod(request.Importer.ProducesMainObject) ||
        reader.Pod(request.Importer.ProducesSubObjects) || reader.Pod(request.Importer.PathSensitive) || reader.StringValue(request.Target) ||
        reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import request is truncated or malformed."));
    request.AuthorizedInputs.Resize(count);
    for (AssetImportWorkerInput& input : request.AuthorizedInputs)
    {
        if (reader.StringValue(input.Identity) || reader.StringValue(input.CanonicalPath) || reader.Hash(input.Hash) || reader.Bytes(input.Snapshot))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import input snapshot is truncated."));
    }
    if (reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import tool allowlist is malformed."));
    request.AllowedTools.Resize(count);
    for (AssetImportWorkerTool& tool : request.AllowedTools)
    {
        if (reader.StringValue(tool.Name) || reader.Hash(tool.VersionHash))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import tool allowlist is truncated."));
    }
    if (reader.StringValue(request.OutputStagingPath) || reader.Pod(request.Limits.MaximumInputBytes) ||
        reader.Pod(request.Limits.MaximumOutputBytes) || reader.Pod(request.Limits.MaximumMemoryBytes) ||
        reader.Pod(request.Limits.MaximumOutputFiles) || reader.Pod(request.Limits.TimeoutMilliseconds) || !reader.AtEnd())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import request has invalid trailing data."));
    return ValidateRequest(request, diagnostic);
}

bool AssetImportWorkerProtocol::SaveResult(const StringView& path, const AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic)
{
    ProtocolWriter writer;
    writer.Pod(ProtocolMagic);
    writer.Pod(result.ProtocolVersion);
    writer.Pod(result.JobID);
    writer.Pod(result.Capability);
    writer.Pod(result.Status);
    uint32 count = result.Diagnostics.Count();
    writer.Pod(count);
    for (const AssetPipelineDiagnostic& value : result.Diagnostics)
        WriteDiagnostic(writer, value);
    count = result.Objects.Count();
    writer.Pod(count);
    for (const AssetImportedObjectDeclaration& value : result.Objects)
    {
        writer.StringValue(value.StableIdentifier);
        writer.StringValue(value.TypeName);
        writer.StringValue(value.DisplayName);
        writer.Pod(value.IsMain);
    }
    count = result.Dependencies.Count();
    writer.Pod(count);
    for (const AssetImportDependency& value : result.Dependencies)
        WriteDependency(writer, value);
    count = result.Outputs.Count();
    writer.Pod(count);
    for (const AssetImportWorkerOutput& value : result.Outputs)
    {
        writer.StringValue(value.Name);
        writer.AnsiValue(value.Kind);
        writer.StringValue(value.RelativePath);
        writer.Hash(value.Hash);
        writer.Pod(value.Size);
    }
    writer.AnsiValue(result.OutputManifestDraft);
    count = result.ObservedToolchain.Count();
    writer.Pod(count);
    for (const AssetImportWorkerTool& value : result.ObservedToolchain)
    {
        writer.StringValue(value.Name);
        writer.Hash(value.VersionHash);
    }
    writer.Pod(result.PeakMemory);
    if (WriteAtomic(path, writer.Data))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, TEXT("Cannot write the isolated import result atomically."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImportWorkerProtocol::LoadResult(const StringView& path, AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic)
{
    Array<byte> data;
    if (File::ReadAllBytes(path, data))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, TEXT("Isolated import worker did not produce a result."));
    ProtocolReader reader(data.Get(), data.Count());
    uint32 magic, count;
    if (reader.Pod(magic) || magic != ProtocolMagic || reader.Pod(result.ProtocolVersion) || reader.Pod(result.JobID) ||
        reader.Pod(result.Capability) || reader.Pod(result.Status) || reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import result is truncated or malformed."));
    result.Diagnostics.Resize(count);
    for (AssetPipelineDiagnostic& value : result.Diagnostics)
    {
        if (ReadDiagnostic(reader, value))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import diagnostics are malformed."));
    }
    if (reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import object declarations are malformed."));
    result.Objects.Resize(count);
    for (AssetImportedObjectDeclaration& value : result.Objects)
    {
        if (reader.StringValue(value.StableIdentifier) || reader.StringValue(value.TypeName) || reader.StringValue(value.DisplayName) || reader.Pod(value.IsMain))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import object declaration is truncated."));
    }
    if (reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import dependencies are malformed."));
    result.Dependencies.Resize(count);
    for (AssetImportDependency& value : result.Dependencies)
    {
        if (ReadDependency(reader, value))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import dependency is truncated."));
    }
    if (reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import outputs are malformed."));
    result.Outputs.Resize(count);
    for (AssetImportWorkerOutput& value : result.Outputs)
    {
        if (reader.StringValue(value.Name) || reader.AnsiValue(value.Kind) || reader.StringValue(value.RelativePath) ||
            reader.Hash(value.Hash) || reader.Pod(value.Size))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import output is truncated."));
    }
    if (reader.AnsiValue(result.OutputManifestDraft) || reader.Count(count))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import manifest draft is malformed."));
    result.ObservedToolchain.Resize(count);
    for (AssetImportWorkerTool& value : result.ObservedToolchain)
    {
        if (reader.StringValue(value.Name) || reader.Hash(value.VersionHash))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import tool observation is truncated."));
    }
    if (reader.Pod(result.PeakMemory) || !reader.AtEnd())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import result has invalid trailing data."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImportWorkerProtocol::ValidateRequest(const AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic)
{
    if (request.ProtocolVersion != AssetImportJobRequest::CurrentProtocolVersion || !request.JobID.IsValid() ||
        !request.Capability.IsValid() || !request.Asset.IsValid() || request.SourceRevision == 0 || request.SourcePath.IsEmpty() ||
        request.SourceHash.IsZero() || request.MetaHash.IsZero() || request.Importer.ID.IsEmpty() ||
        (request.Importer.ProviderKind != AssetProcessorProviderKind::Native && request.Importer.ProviderKind != AssetProcessorProviderKind::Managed) ||
        request.Importer.ImporterVersion == 0 || request.Importer.SettingsSchemaVersion == 0 ||
        request.Importer.ImplementationHash.IsZero() || request.Target.IsEmpty() || request.OutputStagingPath.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import request identity is incomplete."));
    if (request.Limits.MaximumInputBytes == 0 || request.Limits.MaximumOutputBytes == 0 || request.Limits.MaximumMemoryBytes == 0 ||
        request.Limits.MaximumOutputFiles < 1 || request.Limits.TimeoutMilliseconds == 0)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, TEXT("Isolated import request resource limits are invalid."));
    if (ContentHash::Compute(request.SourceSnapshot.Get(), request.SourceSnapshot.Count()) != request.SourceHash ||
        ContentHash::Compute(request.MetaSnapshot.Get(), request.MetaSnapshot.Count()) != request.MetaHash)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated source or metadata snapshot hash does not match."));
    uint64 inputBytes = request.SourceSnapshot.Count() + request.MetaSnapshot.Count();
    Array<String> identities;
    for (const AssetImportWorkerInput& input : request.AuthorizedInputs)
    {
        if (input.Identity.IsEmpty() || input.CanonicalPath.IsEmpty() || input.Hash.IsZero() || identities.Contains(input.Identity) ||
            ContentHash::Compute(input.Snapshot.Get(), input.Snapshot.Count()) != input.Hash || AddWithoutOverflow(inputBytes, input.Snapshot.Count()))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, TEXT("Isolated import authorized input is invalid or duplicated."));
        identities.Add(input.Identity);
    }
    if (inputBytes > request.Limits.MaximumInputBytes)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, TEXT("Isolated import snapshots exceed the input-size quota."));
    for (int32 i = 0; i < request.AllowedTools.Count(); i++)
    {
        const AssetImportWorkerTool& tool = request.AllowedTools[i];
        if (tool.Name.IsEmpty() || tool.VersionHash.IsZero())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, TEXT("Isolated import tool allowlist entry is invalid."));
        for (int32 j = 0; j < i; j++)
        {
            if (request.AllowedTools[j].Name == tool.Name)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, TEXT("Isolated import tool allowlist is duplicated."));
        }
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImportWorkerProtocol::ValidateResult(const AssetImportJobRequest& request, AssetImportJobResult& result, AssetPipelineDiagnostic& diagnostic)
{
    if (result.ProtocolVersion != request.ProtocolVersion || result.JobID != request.JobID || result.Capability != request.Capability)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, TEXT("Isolated import result does not match the request capability."));
    if (result.PeakMemory > request.Limits.MaximumMemoryBytes || result.Outputs.Count() > request.Limits.MaximumOutputFiles ||
        result.Diagnostics.Count() > 4096 || result.Dependencies.Count() > MaximumProtocolItems || result.Objects.Count() > MaximumProtocolItems)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, TEXT("Isolated import result exceeds a resource quota."));
    for (const AssetImportWorkerTool& observed : result.ObservedToolchain)
    {
        bool allowed = false;
        for (const AssetImportWorkerTool& tool : request.AllowedTools)
        {
            if (tool.Name == observed.Name && tool.VersionHash == observed.VersionHash)
            {
                allowed = true;
                break;
            }
        }
        if (!allowed)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, TEXT("Isolated import worker observed an unapproved toolchain."));
    }
    int32 mainObjects = 0;
    Array<String> stableIdentifiers;
    for (const AssetImportedObjectDeclaration& object : result.Objects)
    {
        if (!SubAssetPolicy::IsKeyValid(object.StableIdentifier) || object.TypeName.IsEmpty() || stableIdentifiers.Contains(object.StableIdentifier))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker declared an invalid or duplicated object."));
        stableIdentifiers.Add(object.StableIdentifier);
        if (object.IsMain)
            mainObjects++;
    }
    if ((request.Importer.ProducesMainObject && mainObjects != 1) || (!request.Importer.ProducesMainObject && mainObjects != 0))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker declared an invalid main object count."));
    if (!request.Importer.ProducesSubObjects && result.Objects.Count() > mainObjects)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker declared unsupported sub-objects."));
    if (result.Status != AssetImportWorkerStatus::Succeeded)
    {
        diagnostic = result.Diagnostics.HasItems() ? result.Diagnostics[0] : AssetPipelineDiagnostic();
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, TEXT("Isolated import worker failed without a diagnostic."));
        return true;
    }
    if (!FileSystem::DirectoryExists(request.OutputStagingPath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, TEXT("Isolated import output staging directory is missing."));
    uint64 outputBytes = 0;
    Array<String> relativeOutputs;
    for (AssetImportWorkerOutput& output : result.Outputs)
    {
        if (output.Name.IsEmpty() || output.Kind.IsEmpty() || !IsSafeRelativePath(output.RelativePath) || output.Hash.IsZero() ||
            output.Size == 0 || relativeOutputs.Contains(output.RelativePath) || AddWithoutOverflow(outputBytes, output.Size))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker output declaration is invalid or duplicated."));
        const String path = request.OutputStagingPath / output.RelativePath;
        if (!AssetPathPolicy::IsSameOrChild(path, request.OutputStagingPath) || !FileSystem::FileExists(path) ||
            FileSystem::GetFileSize(path) != output.Size)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker output path or size is invalid."));
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes) || ContentHash::Compute(bytes.Get(), bytes.Count()) != output.Hash)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker output hash is invalid."));
        relativeOutputs.Add(output.RelativePath);
    }
    if (outputBytes > request.Limits.MaximumOutputBytes || result.OutputManifestDraft.Length() > request.Limits.MaximumOutputBytes)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, TEXT("Isolated import worker outputs exceed the output-size quota."));
    Array<String> stagedFiles;
    if (FileSystem::DirectoryGetFiles(stagedFiles, request.OutputStagingPath, TEXT("*"), DirectorySearchOption::AllDirectories) ||
        stagedFiles.Count() != result.Outputs.Count())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, TEXT("Isolated import worker left undeclared staging files."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

int32 AssetImportWorkerHost::Run(const StringView& requestPath, const StringView& resultPath, const StringView& capability,
                                 AssetImportWorkerAction action)
{
    AssetPipelineDiagnostic diagnostic;
    AssetImportJobRequest request;
    if (AssetImportWorkerProtocol::LoadRequest(requestPath, request, diagnostic))
        return 2;
    Guid suppliedCapability;
    if (Guid::Parse(capability, suppliedCapability) || suppliedCapability != request.Capability || !action.IsBinded())
        return 3;
    AssetImportJobResult result;
    result.ProtocolVersion = request.ProtocolVersion;
    result.JobID = request.JobID;
    result.Capability = request.Capability;
    if (action(request, result, diagnostic))
    {
        result.Status = AssetImportWorkerStatus::Failed;
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.AssetGuid = request.Asset.Value;
            diagnostic.SourcePath = request.SourcePath;
            diagnostic.ProcessorId = request.Importer.ID;
            diagnostic.Message = TEXT("Native import worker failed without a diagnostic.");
        }
        result.Diagnostics.Add(diagnostic);
    }
    else
    {
        result.Status = AssetImportWorkerStatus::Succeeded;
    }
    return AssetImportWorkerProtocol::SaveResult(resultPath, result, diagnostic) ? 4 : 0;
}
