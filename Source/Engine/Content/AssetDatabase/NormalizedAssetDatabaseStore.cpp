// Copyright (c) Wojciech Figat. All rights reserved.

#include "NormalizedAssetDatabaseStore.h"
#include "AssetDatabaseBinary.h"
#include "DurableAssetFileSystem.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Utilities/Crc.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

using namespace SourceAssetDatabaseBinary;

namespace
{
    constexpr uint32 ManifestMagic = 0x4d444146; // FADM
    constexpr uint32 ManifestVersion = 1;
    constexpr uint32 TableMagic = 0x54444146; // FADT
    constexpr uint32 TableVersion = 1;
    constexpr uint32 WalMagic = 0x4c574146; // FAWL
    constexpr uint32 WalVersion = 1;
    constexpr uint32 WalFrameMagic = 0x46574146; // FAWF
    constexpr uint32 MaximumPayloadBytes = 1024 * 1024 * 1024;

    enum class TableId : uint32
    {
        DatabaseState,
        SourceAssets,
        AssetObjects,
        Dependencies,
        ImportPublications,
        Diagnostics,
        ImportTargets,
        ArtifactObjects,
        Labels,
        FileJournal,
        RefreshSessions,
        ImportAttempts,
        CustomDependencies,
        Count,
    };

#pragma pack(push, 1)
    struct Manifest
    {
        uint32 Magic = ManifestMagic;
        uint32 Version = ManifestVersion;
        uint32 SchemaVersion = AssetDatabaseSchema::Version;
        Guid ProjectId = Guid::Empty;
        uint64 Generation = 0;
        uint64 Revision = 0;
        uint32 Crc = 0;
    };

    struct TableHeader
    {
        uint32 Magic = TableMagic;
        uint32 Version = TableVersion;
        uint32 Table = 0;
        uint64 Generation = 0;
        uint64 Revision = 0;
        uint32 PayloadSize = 0;
        uint32 PayloadCrc = 0;
    };

    struct WalHeader
    {
        uint32 Magic = WalMagic;
        uint32 Version = WalVersion;
        uint32 SchemaVersion = AssetDatabaseSchema::Version;
        Guid ProjectId = Guid::Empty;
        uint64 BaseRevision = 0;
        uint32 Crc = 0;
    };

    struct WalFrameHeader
    {
        uint32 Magic = WalFrameMagic;
        uint32 PayloadSize = 0;
        uint32 PayloadCrc = 0;
        uint64 BaseRevision = 0;
        uint64 Revision = 0;
    };
#pragma pack(pop)

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    template<typename T>
    uint32 HeaderCrc(const T& value)
    {
        return Crc::MemCrc32(&value, sizeof(T) - sizeof(uint32));
    }

    bool FlushWalFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String normalizedPath(path);
        HANDLE handle = CreateFileW(*normalizedPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#else
        return DurableAssetFileSystem::FlushFile(path);
#endif
    }

    bool WriteAtomic(const StringView& path, const void* data, uint32 length)
    {
        const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { DurableAssetFileSystem::DeleteFile(staging); };
#if PLATFORM_LINUX || PLATFORM_MAC
        if (File::WriteAllBytes(staging, data, length) || DurableAssetFileSystem::FlushFile(staging))
            return true;
#else
        if (File::WriteAllBytes(staging, data, length))
            return true;
#endif
        return DurableAssetFileSystem::Replace(path, staging);
    }

    const Char* TableName(TableId table)
    {
        switch (table)
        {
        case TableId::DatabaseState: return TEXT("database_state");
        case TableId::SourceAssets: return TEXT("source_assets");
        case TableId::AssetObjects: return TEXT("asset_objects");
        case TableId::Dependencies: return TEXT("dependencies");
        case TableId::ImportPublications: return TEXT("import_publications");
        case TableId::Diagnostics: return TEXT("diagnostics");
        case TableId::ImportTargets: return TEXT("import_targets");
        case TableId::ArtifactObjects: return TEXT("artifact_objects");
        case TableId::Labels: return TEXT("labels");
        case TableId::FileJournal: return TEXT("file_journal");
        case TableId::RefreshSessions: return TEXT("refresh_sessions");
        case TableId::ImportAttempts: return TEXT("import_attempts");
        case TableId::CustomDependencies: return TEXT("custom_dependencies");
        default: return TEXT("invalid");
        }
    }

    String TablePath(const StringView& directory, TableId table, uint64 generation)
    {
        return String(directory) / String::Format(TEXT("{0}-{1}.table"), TableName(table), generation);
    }

    void SelectTable(const SourceAssetDatabaseState& state, TableId table, SourceAssetDatabaseState& fragment)
    {
        fragment.Database = state.Database;
        switch (table)
        {
        case TableId::SourceAssets: fragment.Sources = state.Sources; break;
        case TableId::AssetObjects: fragment.Objects = state.Objects; break;
        case TableId::Dependencies: fragment.Dependencies = state.Dependencies; break;
        case TableId::ImportPublications: fragment.Publications = state.Publications; break;
        case TableId::Diagnostics: fragment.Diagnostics = state.Diagnostics; break;
        case TableId::ImportTargets: fragment.ImportTargets = state.ImportTargets; break;
        case TableId::ArtifactObjects: fragment.ArtifactObjects = state.ArtifactObjects; break;
        case TableId::Labels: fragment.Labels = state.Labels; break;
        case TableId::FileJournal: fragment.FileJournal = state.FileJournal; break;
        case TableId::RefreshSessions: fragment.RefreshSessions = state.RefreshSessions; break;
        case TableId::ImportAttempts: fragment.ImportAttempts = state.ImportAttempts; break;
        case TableId::CustomDependencies: fragment.CustomDependencies = state.CustomDependencies; break;
        default: break;
        }
    }

    void MergeTable(SourceAssetDatabaseState& state, TableId table, SourceAssetDatabaseState& fragment)
    {
        switch (table)
        {
        case TableId::DatabaseState: state.Database = fragment.Database; break;
        case TableId::SourceAssets: state.Sources = MoveTemp(fragment.Sources); break;
        case TableId::AssetObjects: state.Objects = MoveTemp(fragment.Objects); break;
        case TableId::Dependencies: state.Dependencies = MoveTemp(fragment.Dependencies); break;
        case TableId::ImportPublications: state.Publications = MoveTemp(fragment.Publications); break;
        case TableId::Diagnostics: state.Diagnostics = MoveTemp(fragment.Diagnostics); break;
        case TableId::ImportTargets: state.ImportTargets = MoveTemp(fragment.ImportTargets); break;
        case TableId::ArtifactObjects: state.ArtifactObjects = MoveTemp(fragment.ArtifactObjects); break;
        case TableId::Labels: state.Labels = MoveTemp(fragment.Labels); break;
        case TableId::FileJournal: state.FileJournal = MoveTemp(fragment.FileJournal); break;
        case TableId::RefreshSessions: state.RefreshSessions = MoveTemp(fragment.RefreshSessions); break;
        case TableId::ImportAttempts: state.ImportAttempts = MoveTemp(fragment.ImportAttempts); break;
        case TableId::CustomDependencies: state.CustomDependencies = MoveTemp(fragment.CustomDependencies); break;
        default: break;
        }
    }

    void SerializeWalRecord(const NormalizedAssetDatabaseWalRecord& record, Array<byte>& output)
    {
        Writer writer;
        writer.Write((uint32)record.Mutations.Count());
        for (const AssetDatabaseMutation& mutation : record.Mutations)
        {
            writer.Write((byte)mutation.Kind);
            writer.Write(mutation.Key);
            writer.Write(mutation.LocalFileId);
            writer.Write(mutation.Value);
            writer.WriteString(mutation.TargetId);
            writer.Write(mutation.Artifact);
            writer.Write((uint32)mutation.Payload.Count());
            if (mutation.Payload.HasItems())
                writer.Stream.WriteBytes(mutation.Payload.Get(), mutation.Payload.Count());
        }
        Array<byte> changes;
        record.Changes.Serialize(changes);
        writer.Write((uint32)changes.Count());
        if (changes.HasItems())
            writer.Stream.WriteBytes(changes.Get(), changes.Count());
        writer.Finish(output);
    }

    bool DeserializeWalRecord(const byte* data, uint32 length, NormalizedAssetDatabaseWalRecord& record)
    {
        Reader reader(data, length);
        uint32 count;
        if (reader.ReadCount(count))
            return true;
        record.Mutations.Resize(count, false);
        for (AssetDatabaseMutation& mutation : record.Mutations)
        {
            byte kind;
            uint32 payloadSize;
            if (reader.Read(kind) || kind > (byte)AssetDatabaseMutationKind::ReplaceSnapshot ||
                reader.Read(mutation.Key) || reader.Read(mutation.LocalFileId) || reader.Read(mutation.Value) ||
                reader.ReadString(mutation.TargetId) || reader.Read(mutation.Artifact) || reader.Read(payloadSize) ||
                payloadSize > MaximumPayloadBytes)
                return true;
            mutation.Kind = (AssetDatabaseMutationKind)kind;
            mutation.Payload.Resize(payloadSize, false);
            if (payloadSize && reader.ReadBytes(mutation.Payload.Get(), payloadSize))
                return true;
        }
        uint32 changeSize;
        if (reader.Read(changeSize) || changeSize > MaximumPayloadBytes || changeSize > length)
            return true;
        Array<byte> changes;
        changes.Resize(changeSize, false);
        if ((changeSize && reader.ReadBytes(changes.Get(), changeSize)) || !reader.AtEnd() ||
            AssetChangeSet::Deserialize(changes.Get(), changes.Count(), record.Changes))
            return true;
        return false;
    }

    bool WriteWalHeader(const StringView& path, const Guid& projectId, uint64 baseRevision)
    {
        WalHeader header;
        header.ProjectId = projectId;
        header.BaseRevision = baseRevision;
        header.Crc = HeaderCrc(header);
        return WriteAtomic(path, &header, sizeof(header));
    }
}

String NormalizedAssetDatabaseStore::GetManifestPath(const StringView& directory)
{
    return String(directory) / TEXT("normalized-store.manifest");
}

String NormalizedAssetDatabaseStore::GetWalPath(const StringView& directory)
{
    return String(directory) / TEXT("normalized-store.wal");
}

bool NormalizedAssetDatabaseStore::LoadCheckpoint(const StringView& directory, const Guid& projectId,
    SourceAssetDatabaseState& state, uint64& generation, AssetPipelineDiagnostic& diagnostic,
    NormalizedAssetDatabaseLoadFailure& failure)
{
    failure = NormalizedAssetDatabaseLoadFailure::RecoverableDerivedState;
    const String manifestPath = GetManifestPath(directory);
    Array<byte> bytes;
    if (File::ReadAllBytes(manifestPath, bytes) || bytes.Count() != sizeof(Manifest))
        return Fail(diagnostic, manifestPath, TEXT("Normalized source database manifest is missing or truncated."));
    Manifest manifest;
    Platform::MemoryCopy(&manifest, bytes.Get(), sizeof(manifest));
    if (manifest.Magic != ManifestMagic || manifest.Crc != HeaderCrc(manifest))
        return Fail(diagnostic, manifestPath, TEXT("Normalized source database manifest is invalid."));
    if (manifest.Version > ManifestVersion || manifest.SchemaVersion > AssetDatabaseSchema::Version)
    {
        failure = NormalizedAssetDatabaseLoadFailure::FutureVersion;
        return Fail(diagnostic, manifestPath, TEXT("Normalized source database uses a newer unsupported format."));
    }
    if (manifest.ProjectId != projectId)
    {
        failure = NormalizedAssetDatabaseLoadFailure::ForeignProject;
        return Fail(diagnostic, manifestPath, TEXT("Normalized source database belongs to another project."));
    }
    if (manifest.Version != ManifestVersion || manifest.SchemaVersion < AssetDatabaseSchema::Version ||
        manifest.Generation == 0)
        return Fail(diagnostic, manifestPath, TEXT("Normalized source database manifest is obsolete or invalid."));

    SourceAssetDatabaseState loaded;
    for (uint32 i = 0; i < (uint32)TableId::Count; i++)
    {
        const TableId table = (TableId)i;
        const String path = TablePath(directory, table, manifest.Generation);
        if (File::ReadAllBytes(path, bytes) || bytes.Count() < sizeof(TableHeader))
            return Fail(diagnostic, path, TEXT("Normalized source database table is missing or truncated."));
        TableHeader header;
        Platform::MemoryCopy(&header, bytes.Get(), sizeof(header));
        if (header.Magic != TableMagic || header.Version != TableVersion || header.Table != i ||
            header.Generation != manifest.Generation || header.Revision != manifest.Revision ||
            header.PayloadSize > MaximumPayloadBytes || header.PayloadSize != (uint32)bytes.Count() - sizeof(header) ||
            header.PayloadCrc != Crc::MemCrc32(bytes.Get() + sizeof(header), header.PayloadSize))
            return Fail(diagnostic, path, TEXT("Normalized source database table header or checksum is invalid."));
        SourceAssetDatabaseState fragment;
        if (SourceAssetDatabaseState::Deserialize(bytes.Get() + sizeof(header), header.PayloadSize, fragment, diagnostic, false) ||
            fragment.Database.ProjectId != projectId || fragment.Database.CurrentRevision != manifest.Revision)
            return Fail(diagnostic, path, TEXT("Normalized source database table payload is invalid."));
        MergeTable(loaded, table, fragment);
    }
    loaded.Database.SchemaVersion = AssetDatabaseSchema::Version;
    if (loaded.Validate(diagnostic))
        return true;
    state = MoveTemp(loaded);
    generation = manifest.Generation;
    failure = NormalizedAssetDatabaseLoadFailure::None;
    return false;
}

bool NormalizedAssetDatabaseStore::SaveCheckpoint(const StringView& directory, const SourceAssetDatabaseState& state,
    uint64& generation, AssetPipelineDiagnostic& diagnostic)
{
    if (state.Validate(diagnostic))
        return true;
    if (generation == MAX_uint64)
        return Fail(diagnostic, directory, TEXT("Normalized source database checkpoint generation is exhausted."));
    const uint64 nextGeneration = generation + 1;
    // Reserve the generation before any publication attempt. A rename can succeed but its directory flush can
    // report failure, so retrying the same generation could otherwise rewrite tables already named by a manifest.
    generation = nextGeneration;
    for (uint32 i = 0; i < (uint32)TableId::Count; i++)
    {
        const TableId table = (TableId)i;
        SourceAssetDatabaseState fragment;
        SelectTable(state, table, fragment);
        Array<byte> payload;
        fragment.Serialize(payload);
        TableHeader header;
        header.Table = i;
        header.Generation = nextGeneration;
        header.Revision = state.Database.CurrentRevision;
        header.PayloadSize = payload.Count();
        header.PayloadCrc = Crc::MemCrc32(payload.Get(), payload.Count());
        Array<byte> file;
        file.Resize(sizeof(header) + payload.Count(), false);
        Platform::MemoryCopy(file.Get(), &header, sizeof(header));
        if (payload.HasItems())
            Platform::MemoryCopy(file.Get() + sizeof(header), payload.Get(), payload.Count());
        const String path = TablePath(directory, table, nextGeneration);
        if (WriteAtomic(path, file.Get(), file.Count()))
            return Fail(diagnostic, path, TEXT("Cannot write normalized source database table checkpoint."));
    }
    if (DurableAssetFileSystem::FlushDirectory(directory))
        return Fail(diagnostic, directory, TEXT("Cannot durably publish normalized source database table checkpoints."));

    Manifest manifest;
    manifest.ProjectId = state.Database.ProjectId;
    manifest.Generation = nextGeneration;
    manifest.Revision = state.Database.CurrentRevision;
    manifest.Crc = HeaderCrc(manifest);
    const String manifestPath = GetManifestPath(directory);
    if (WriteAtomic(manifestPath, &manifest, sizeof(manifest)))
        return Fail(diagnostic, manifestPath, TEXT("Cannot publish normalized source database checkpoint manifest."));

    // Old and abandoned generations are inert after manifest publication. Cleanup is retryable maintenance and
    // must not turn an already authoritative checkpoint into a failed commit that retriggers on every transaction.
    Array<String> checkpointTables;
    if (!FileSystem::DirectoryGetFiles(checkpointTables, directory, TEXT("*.table")))
    {
        bool removedOldTable = false;
        const String activeSuffix = String::Format(TEXT("-{0}.table"), nextGeneration);
        for (const String& oldTablePath : checkpointTables)
        {
            const String fileName = StringUtils::GetFileName(oldTablePath);
            bool recognized = false;
            for (uint32 i = 0; i < (uint32)TableId::Count; i++)
            {
                if (fileName.StartsWith(String(TableName((TableId)i)) + TEXT("-")))
                {
                    recognized = true;
                    break;
                }
            }
            if (!recognized || fileName.EndsWith(activeSuffix))
                continue;
            removedOldTable |= !FileSystem::DeleteFile(oldTablePath);
        }
        if (removedOldTable)
            DurableAssetFileSystem::FlushDirectory(directory);
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool NormalizedAssetDatabaseStore::OpenWal(const StringView& path, const Guid& projectId, uint64 checkpointRevision,
    uint64& baseRevision, uint64& lastRevision, Array<NormalizedAssetDatabaseWalRecord>& records,
    AssetPipelineDiagnostic& diagnostic)
{
    records.Clear();
    if (!FileSystem::FileExists(path))
    {
        if (WriteWalHeader(path, projectId, checkpointRevision))
            return Fail(diagnostic, path, TEXT("Cannot create normalized source database WAL."));
        baseRevision = lastRevision = checkpointRevision;
        return false;
    }
    Array<byte> data;
    if (File::ReadAllBytes(path, data) || data.Count() < sizeof(WalHeader))
        return Fail(diagnostic, path, TEXT("Normalized source database WAL is missing or truncated."));
    WalHeader header;
    Platform::MemoryCopy(&header, data.Get(), sizeof(header));
    if (header.Magic != WalMagic || header.Version != WalVersion || header.SchemaVersion < 3 ||
        header.SchemaVersion > AssetDatabaseSchema::Version ||
        header.ProjectId != projectId || header.Crc != HeaderCrc(header) || checkpointRevision < header.BaseRevision)
        return Fail(diagnostic, path, TEXT("Normalized source database WAL header is invalid."));
    baseRevision = header.BaseRevision;
    lastRevision = baseRevision;
    uint32 position = sizeof(header);
    while (position < (uint32)data.Count())
    {
        if ((uint32)data.Count() - position < sizeof(WalFrameHeader))
            break;
        WalFrameHeader frame;
        Platform::MemoryCopy(&frame, data.Get() + position, sizeof(frame));
        if (frame.Magic != WalFrameMagic || frame.PayloadSize > MaximumPayloadBytes ||
            frame.PayloadSize > (uint32)data.Count() - position - sizeof(frame) || frame.BaseRevision != lastRevision ||
            frame.Revision != lastRevision + 1)
            break;
        const byte* payload = data.Get() + position + sizeof(frame);
        if (frame.PayloadCrc != Crc::MemCrc32(payload, frame.PayloadSize))
            break;
        NormalizedAssetDatabaseWalRecord record;
        record.BaseRevision = frame.BaseRevision;
        record.Revision = frame.Revision;
        if (DeserializeWalRecord(payload, frame.PayloadSize, record) || record.Changes.Revision != record.Revision)
            break;
        lastRevision = record.Revision;
        position += sizeof(frame) + frame.PayloadSize;
        if (record.Revision > checkpointRevision)
            records.Add(MoveTemp(record));
    }
    if (position != (uint32)data.Count() && WriteAtomic(path, data.Get(), position))
        return Fail(diagnostic, path, TEXT("Cannot recover normalized source database WAL tail."));
    if (checkpointRevision > lastRevision)
    {
        if (ResetWal(path, projectId, checkpointRevision, diagnostic))
            return true;
        baseRevision = lastRevision = checkpointRevision;
        records.Clear();
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool NormalizedAssetDatabaseStore::AppendWal(const StringView& path, const Guid& projectId, uint64& lastRevision,
    const NormalizedAssetDatabaseWalRecord& record, AssetPipelineDiagnostic& diagnostic)
{
    if (record.BaseRevision != lastRevision || record.Revision != lastRevision + 1 ||
        record.Changes.Revision != record.Revision)
        return Fail(diagnostic, path, TEXT("Normalized source database WAL transaction revision is not contiguous."));
    Array<byte> payload;
    SerializeWalRecord(record, payload);
    if (payload.Count() > MaximumPayloadBytes)
        return Fail(diagnostic, path, TEXT("Normalized source database WAL transaction exceeds the maximum payload size."));
    WalFrameHeader frame;
    frame.PayloadSize = payload.Count();
    frame.PayloadCrc = Crc::MemCrc32(payload.Get(), payload.Count());
    frame.BaseRevision = record.BaseRevision;
    frame.Revision = record.Revision;
    File* file = File::Open(path, FileMode::OpenExisting, FileAccess::Write, FileShare::Read);
    if (!file)
        return Fail(diagnostic, path, TEXT("Cannot open normalized source database WAL for append."));
    file->SetPosition(file->GetSize());
    const bool failed = file->Write(&frame, sizeof(frame)) || (payload.HasItems() && file->Write(payload.Get(), payload.Count()));
    Delete(file);
    if (failed || FlushWalFile(path))
        return Fail(diagnostic, path, TEXT("Cannot append normalized source database WAL transaction."));
    lastRevision = record.Revision;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool NormalizedAssetDatabaseStore::ResetWal(const StringView& path, const Guid& projectId, uint64 baseRevision,
    AssetPipelineDiagnostic& diagnostic)
{
    if (WriteWalHeader(path, projectId, baseRevision))
        return Fail(diagnostic, path, TEXT("Cannot checkpoint normalized source database WAL."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
