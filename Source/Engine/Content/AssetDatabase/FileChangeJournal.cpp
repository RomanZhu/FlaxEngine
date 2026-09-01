// Copyright (c) Wojciech Figat. All rights reserved.

#include "FileChangeJournal.h"
#include "AssetDatabaseBinary.h"
#include "DurableAssetFileSystem.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/Crc.h"

namespace
{
    constexpr uint32 JournalMagic = 0x4a434146; // FACJ
    constexpr uint32 JournalVersion = 1;
    constexpr uint32 FrameMagic = 0x46434146; // FACF
    constexpr uint32 MaximumFrameBytes = 256 * 1024 * 1024;
    constexpr uint64 MaximumRetainedRevisions = 4096;

    struct JournalHeader
    {
        uint32 Magic;
        uint32 Version;
        uint64 BaseRevision;
        uint32 HeaderCrc;
    };

    struct FrameHeader
    {
        uint32 Magic;
        uint32 PayloadSize;
        uint32 PayloadCrc;
        uint64 Revision;
    };

    uint32 HeaderCrc(const JournalHeader& header)
    {
        return Crc::MemCrc32(&header, 16);
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool WriteAtomic(const StringView& path, const byte* data, uint32 length)
    {
        const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { DurableAssetFileSystem::DeleteFile(staging); };
        return DurableAssetFileSystem::WriteFile(staging, data, length) ||
               DurableAssetFileSystem::Replace(path, staging);
    }

    bool ParseJournal(const Array<byte>& data, uint64& baseRevision, uint64& lastRevision, uint32& validLength,
        Array<AssetChangeSet>* changes, const uint64 afterRevision)
    {
        if (data.Count() < (int32)sizeof(JournalHeader))
            return true;
        JournalHeader header;
        Platform::MemoryCopy(&header, data.Get(), sizeof(header));
        if (header.Magic != JournalMagic || header.Version != JournalVersion || header.HeaderCrc != HeaderCrc(header))
            return true;
        baseRevision = header.BaseRevision;
        lastRevision = baseRevision;
        validLength = sizeof(JournalHeader);
        while (validLength < (uint32)data.Count())
        {
            if ((uint32)data.Count() - validLength < sizeof(FrameHeader))
                break;
            FrameHeader frame;
            Platform::MemoryCopy(&frame, data.Get() + validLength, sizeof(frame));
            if (frame.Magic != FrameMagic || frame.PayloadSize > MaximumFrameBytes ||
                frame.PayloadSize > (uint32)data.Count() - validLength - sizeof(FrameHeader) || frame.Revision != lastRevision + 1)
                break;
            const byte* payload = data.Get() + validLength + sizeof(FrameHeader);
            if (frame.PayloadCrc != Crc::MemCrc32(payload, frame.PayloadSize))
                break;
            AssetChangeSet changeSet;
            if (AssetChangeSet::Deserialize(payload, frame.PayloadSize, changeSet) || changeSet.Revision != frame.Revision)
                break;
            lastRevision = frame.Revision;
            validLength += sizeof(FrameHeader) + frame.PayloadSize;
            if (changes && changeSet.Revision > afterRevision)
                changes->Add(MoveTemp(changeSet));
        }
        return false;
    }
}

bool FileChangeJournal::Compact(uint64 baseRevision, AssetPipelineDiagnostic& diagnostic)
{
    Array<byte> data;
    uint64 parsedBase, parsedLast;
    uint32 validLength;
    Array<AssetChangeSet> changes;
    if (File::ReadAllBytes(_path, data) || ParseJournal(data, parsedBase, parsedLast, validLength, &changes, baseRevision) ||
        validLength != (uint32)data.Count() || baseRevision < parsedBase || baseRevision > parsedLast)
        return Fail(diagnostic, _path, TEXT("Cannot read source asset change history for compaction."));
    JournalHeader header = { JournalMagic, JournalVersion, baseRevision, 0 };
    header.HeaderCrc = HeaderCrc(header);
    Array<byte> compacted;
    compacted.Resize(sizeof(header), false);
    Platform::MemoryCopy(compacted.Get(), &header, sizeof(header));
    for (const AssetChangeSet& change : changes)
    {
        Array<byte> payload;
        change.Serialize(payload);
        FrameHeader frame = { FrameMagic, (uint32)payload.Count(), Crc::MemCrc32(payload.Get(), payload.Count()), change.Revision };
        const int32 offset = compacted.Count();
        compacted.Resize(offset + sizeof(frame) + payload.Count(), true);
        Platform::MemoryCopy(compacted.Get() + offset, &frame, sizeof(frame));
        if (payload.HasItems())
            Platform::MemoryCopy(compacted.Get() + offset + sizeof(frame), payload.Get(), payload.Count());
    }
    if (WriteAtomic(_path, compacted.Get(), compacted.Count()))
        return Fail(diagnostic, _path, TEXT("Cannot compact source asset change history."));
    _baseRevision = baseRevision;
    _lastRevision = parsedLast;
    return false;
}

bool FileChangeJournal::Open(const StringView& path, uint64 baseRevision, AssetPipelineDiagnostic& diagnostic)
{
    Close();
    _path = path;
    if (!FileSystem::FileExists(path))
    {
        JournalHeader header = { JournalMagic, JournalVersion, baseRevision, 0 };
        header.HeaderCrc = HeaderCrc(header);
        if (WriteAtomic(path, (const byte*)&header, sizeof(header)))
            return Fail(diagnostic, path, TEXT("Cannot create the source asset change journal."));
        _baseRevision = _lastRevision = baseRevision;
        _open = true;
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    Array<byte> data;
    uint32 validLength;
    if (File::ReadAllBytes(path, data) || ParseJournal(data, _baseRevision, _lastRevision, validLength, nullptr, 0))
    {
        _path = path;
        _open = true;
        return Reset(baseRevision, diagnostic);
    }
    if (validLength != (uint32)data.Count() && WriteAtomic(path, data.Get(), validLength))
        return Fail(diagnostic, path, TEXT("Cannot recover the incomplete source asset change journal tail."));
    _open = true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void FileChangeJournal::Close()
{
    _path.Clear();
    _baseRevision = 0;
    _lastRevision = 0;
    _open = false;
}

bool FileChangeJournal::IsOpen() const
{
    return _open;
}

uint64 FileChangeJournal::GetBaseRevision() const
{
    return _baseRevision;
}

uint64 FileChangeJournal::GetLastRevision() const
{
    return _lastRevision;
}

bool FileChangeJournal::Reset(uint64 baseRevision, AssetPipelineDiagnostic& diagnostic)
{
    if (!_open)
        return Fail(diagnostic, _path, TEXT("Source asset change journal is not open."));
    JournalHeader header = { JournalMagic, JournalVersion, baseRevision, 0 };
    header.HeaderCrc = HeaderCrc(header);
    if (WriteAtomic(_path, (const byte*)&header, sizeof(header)))
        return Fail(diagnostic, _path, TEXT("Cannot compact the source asset change journal."));
    _baseRevision = _lastRevision = baseRevision;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool FileChangeJournal::Append(const AssetChangeSet& changeSet, AssetPipelineDiagnostic& diagnostic)
{
    if (!_open || changeSet.Revision == 0)
        return Fail(diagnostic, _path, TEXT("Source asset change journal is not open or revision is invalid."));
    if (changeSet.Revision == _lastRevision)
        return false;
    if (changeSet.Revision != _lastRevision + 1)
        return Fail(diagnostic, _path, TEXT("Source asset change journal revisions must be contiguous."));

    Array<byte> payload;
    changeSet.Serialize(payload);
    FrameHeader frame = { FrameMagic, (uint32)payload.Count(), Crc::MemCrc32(payload.Get(), payload.Count()), changeSet.Revision };
    File* file = File::Open(_path, FileMode::OpenAlways, FileAccess::Write, FileShare::Read);
    if (!file)
        return Fail(diagnostic, _path, TEXT("Cannot open the source asset change journal for append."));
    file->SetPosition(file->GetSize());
    const bool failed = file->Write(&frame, sizeof(frame)) || (payload.HasItems() && file->Write(payload.Get(), payload.Count()));
    Delete(file);
    if (failed || DurableAssetFileSystem::FlushFile(_path))
        return Fail(diagnostic, _path, TEXT("Cannot append the source asset change journal."));
    _lastRevision = changeSet.Revision;
    if (_lastRevision - _baseRevision > MaximumRetainedRevisions &&
        Compact(_lastRevision - MaximumRetainedRevisions, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool FileChangeJournal::ReadAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const
{
    result.Clear();
    requiresSnapshot = revision < _baseRevision;
    if (!_open)
        return Fail(diagnostic, _path, TEXT("Source asset change journal is not open."));
    Array<byte> data;
    uint64 baseRevision, lastRevision;
    uint32 validLength;
    if (File::ReadAllBytes(_path, data) || ParseJournal(data, baseRevision, lastRevision, validLength, &result, revision) || validLength != (uint32)data.Count())
        return Fail(diagnostic, _path, TEXT("Source asset change journal is invalid."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
