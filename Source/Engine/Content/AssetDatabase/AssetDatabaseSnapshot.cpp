// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseSnapshot.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Utilities/Crc.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    constexpr uint32 SnapshotMagic = 0x42444146; // FADB
    constexpr uint32 MaximumSnapshotEntries = 2000000;
    constexpr uint32 MaximumStringBytes = 16 * 1024 * 1024;

    struct SnapshotHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 PayloadSize;
        uint32 PayloadCrc;
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    void WriteString(MemoryWriteStream& stream, const StringView& value)
    {
        const StringAnsi utf8(value);
        stream.WriteUint32(utf8.Length());
        stream.WriteBytes(utf8.Get(), utf8.Length());
    }

    void WriteObjectIdArray(MemoryWriteStream& stream, const Array<AssetObjectId>& values)
    {
        stream.WriteUint32(values.Count());
        for (const AssetObjectId& value : values)
        {
            stream.WriteBytes(&value.Asset.Value, sizeof(Guid));
            stream.WriteInt64(value.LocalId);
        }
    }

    void WriteStringArray(MemoryWriteStream& stream, const Array<String>& values)
    {
        stream.WriteUint32(values.Count());
        for (const String& value : values)
            WriteString(stream, value);
    }

    void WriteRecord(MemoryWriteStream& stream, const AssetRecord& record)
    {
        stream.WriteBytes(&record.ID, sizeof(Guid));
        stream.WriteBytes(&record.SourceAssetID, sizeof(Guid));
        stream.WriteInt64(record.LocalId);
        WriteString(stream, record.TypeName);
        WriteString(stream, record.CanonicalPath.Get());
        WriteString(stream, record.SourcePath.Get());
        WriteString(stream, record.MetaPath.Get());
        WriteString(stream, record.SubAsset.Get());
        WriteString(stream, record.DisplayName);
        WriteString(stream, record.ProcessorID);
        WriteString(stream, record.PortabilityKey);
        stream.WriteUint64(record.MetaSemanticHash);
        stream.WriteUint32(record.ImporterSettingsVersion);
        stream.WriteUint8((uint8)record.SourceKind);
        stream.WriteUint8((uint8)record.Status);
        WriteStringArray(stream, record.Labels);
        WriteObjectIdArray(stream, record.BuildInputDependencies);
        WriteObjectIdArray(stream, record.RuntimeReferences);
    }

    class SafeReader
    {
    private:
        const byte* _data;
        uint32 _length;
        uint32 _position = 0;

    public:
        SafeReader(const byte* data, uint32 length)
            : _data(data)
            , _length(length)
        {
        }

        bool ReadBytes(void* output, uint32 length)
        {
            if (length > _length - _position)
                return true;
            Platform::MemoryCopy(output, _data + _position, length);
            _position += length;
            return false;
        }

        template<typename T>
        bool Read(T& output)
        {
            return ReadBytes(&output, sizeof(T));
        }

        bool ReadString(String& output)
        {
            uint32 length;
            if (Read(length) || length > MaximumStringBytes || length > _length - _position)
                return true;
            output = String(StringAnsiView((const char*)_data + _position, length));
            _position += length;
            return false;
        }

        bool ReadObjectIdArray(Array<AssetObjectId>& output)
        {
            uint32 count;
            if (Read(count) || count > MaximumSnapshotEntries ||
                count > (_length - _position) / (sizeof(Guid) + sizeof(int64)))
                return true;
            output.Resize(count, false);
            for (AssetObjectId& value : output)
            {
                Guid guid;
                if (Read(guid) || Read(value.LocalId))
                    return true;
                value.Asset = AssetGuid(guid);
            }
            return false;
        }

        bool ReadStringArray(Array<String>& output)
        {
            uint32 count;
            if (Read(count) || count > MaximumSnapshotEntries)
                return true;
            output.Resize(count, false);
            for (String& value : output)
                if (ReadString(value))
                    return true;
            return false;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };

    bool ReadRecord(SafeReader& reader, AssetRecord& record)
    {
        String canonicalPath, sourcePath, metaPath, subAsset;
        uint8 sourceKind, status;
        if (reader.Read(record.ID) || reader.Read(record.SourceAssetID) || reader.Read(record.LocalId) || reader.ReadString(record.TypeName) ||
            reader.ReadString(canonicalPath) || reader.ReadString(sourcePath) || reader.ReadString(metaPath) ||
            reader.ReadString(subAsset) || reader.ReadString(record.DisplayName) || reader.ReadString(record.ProcessorID) || reader.ReadString(record.PortabilityKey) ||
            reader.Read(record.MetaSemanticHash) || reader.Read(record.ImporterSettingsVersion) || reader.Read(sourceKind) || reader.Read(status) ||
            reader.ReadStringArray(record.Labels) || reader.ReadObjectIdArray(record.BuildInputDependencies) ||
            reader.ReadObjectIdArray(record.RuntimeReferences))
            return true;
        if (record.LocalId <= 0 || sourceKind > (uint8)AssetSourceKind::Folder || status > (uint8)AssetRecordStatus::PathCollision)
            return true;
        record.CanonicalPath = CanonicalAssetPath(canonicalPath);
        record.SourcePath = SourceFilePath(sourcePath);
        record.MetaPath = MetaFilePath(metaPath);
        record.SubAsset = SubAssetKey(subAsset);
        record.SourceKind = (AssetSourceKind)sourceKind;
        record.Status = (AssetRecordStatus)status;
        return false;
    }

    bool AtomicReplace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath(destination);
        const String stagingPath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0;
#else
        return FileSystem::MoveFile(destination, staging, true);
#endif
    }
}

bool AssetDatabaseSnapshotStore::SaveAtomic(const StringView& path, const StringView& projectRoot, const StringView& contentRoot, const AssetDatabaseSnapshot& snapshot, const Array<AssetDatabaseFileState>& fileStates, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    MemoryWriteStream payload;
    WriteString(payload, projectRoot);
    WriteString(payload, contentRoot);
    payload.WriteUint32(snapshot.Records.Count());
    for (const AssetRecord& record : snapshot.Records)
        WriteRecord(payload, record);
    payload.WriteUint32(fileStates.Count());
    for (const AssetDatabaseFileState& state : fileStates)
    {
        WriteString(payload, state.Path);
        payload.WriteUint64(state.Size);
        payload.WriteInt64(state.LastWriteTicks);
        payload.WriteUint64(state.VolumeIdentity);
        payload.WriteUint64(state.FileIdentity);
        payload.WriteInt64(state.ChangeTicks);
        payload.WriteUint8(state.IdentityReliable ? 1 : 0);
        payload.WriteBytes(state.CachedContentHash.Bytes, sizeof(state.CachedContentHash.Bytes));
        payload.WriteUint32(state.CacheChecksum);
    }

    SnapshotHeader header;
    header.Magic = SnapshotMagic;
    header.Version = CurrentVersion;
    header.PayloadSize = payload.GetPosition();
    header.PayloadCrc = Crc::MemCrc32(payload.GetHandle(), header.PayloadSize);
    MemoryWriteStream data(sizeof(SnapshotHeader) + header.PayloadSize);
    data.WriteBytes(&header, sizeof(header));
    data.WriteBytes(payload.GetHandle(), header.PayloadSize);

    const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
    SCOPE_EXIT { FileSystem::DeleteFile(staging); };
    if (File::WriteAllBytes(staging, data.GetHandle(), data.GetPosition()) || AtomicReplace(path, staging))
        return Fail(diagnostic, path, TEXT("Cannot atomically write the disposable asset database snapshot."));
    return false;
}

bool AssetDatabaseSnapshotStore::Load(const StringView& path, const StringView& projectRoot, const StringView& contentRoot, AssetDatabase& database, Array<AssetDatabaseFileState>& fileStates, AssetPipelineDiagnostic& diagnostic)
{
    fileStates.Clear();
    BytesContainer data;
    if (File::ReadAllBytes(path, data) || data.Length() < sizeof(SnapshotHeader))
        return Fail(diagnostic, path, TEXT("Asset database snapshot is missing or truncated."));
    SnapshotHeader header;
    Platform::MemoryCopy(&header, data.Get(), sizeof(header));
    if (header.Magic != SnapshotMagic || header.Version != CurrentVersion || header.PayloadSize != data.Length() - sizeof(header) ||
        header.PayloadCrc != Crc::MemCrc32(data.Get() + sizeof(header), header.PayloadSize))
        return Fail(diagnostic, path, TEXT("Asset database snapshot version or checksum is invalid."));

    SafeReader reader(data.Get() + sizeof(header), header.PayloadSize);
    String storedProjectRoot, storedContentRoot;
    uint32 recordCount;
    if (reader.ReadString(storedProjectRoot) || reader.ReadString(storedContentRoot) || storedProjectRoot != projectRoot || storedContentRoot != contentRoot ||
        reader.Read(recordCount) || recordCount > MaximumSnapshotEntries)
        return Fail(diagnostic, path, TEXT("Asset database snapshot belongs to another project or has an invalid record count."));
    Array<AssetRecord> records;
    records.Resize(recordCount, false);
    for (AssetRecord& record : records)
    {
        if (ReadRecord(reader, record))
            return Fail(diagnostic, path, TEXT("Asset database snapshot contains a malformed record."));
    }
    uint32 fileCount;
    if (reader.Read(fileCount) || fileCount > MaximumSnapshotEntries)
        return Fail(diagnostic, path, TEXT("Asset database snapshot has an invalid source-state count."));
    fileStates.Resize(fileCount, false);
    for (AssetDatabaseFileState& state : fileStates)
    {
        uint8 identityReliable;
        if (reader.ReadString(state.Path) || reader.Read(state.Size) || reader.Read(state.LastWriteTicks) ||
            reader.Read(state.VolumeIdentity) || reader.Read(state.FileIdentity) || reader.Read(state.ChangeTicks) || reader.Read(identityReliable) ||
            reader.ReadBytes(state.CachedContentHash.Bytes, sizeof(state.CachedContentHash.Bytes)) || reader.Read(state.CacheChecksum) || identityReliable > 1)
            return Fail(diagnostic, path, TEXT("Asset database snapshot contains malformed source state."));
        state.IdentityReliable = identityReliable != 0;
    }
    if (!reader.AtEnd())
        return Fail(diagnostic, path, TEXT("Asset database snapshot contains unexpected trailing data."));
    if (database.PublishFullSnapshot(records, diagnostic))
        return true;
    return false;
}
