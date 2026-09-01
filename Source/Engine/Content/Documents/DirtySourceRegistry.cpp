// Copyright (c) Wojciech Figat. All rights reserved.

#include "DirtySourceRegistry.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    String NormalizeDirtyPath(const StringView& path)
    {
        String result(path);
        FileSystem::NormalizePath(result);
        return result;
    }

    int32 FindRecord(const Array<DirtySourceRecord>& records, const StringView& path)
    {
        for (int32 i = 0; i < records.Count(); i++)
        {
            if (FileSystem::AreFilePathsEquivalent(records[i].SourcePath, path))
                return i;
        }
        return -1;
    }
}

DirtySourceRegistry& DirtySourceRegistry::Get()
{
    static DirtySourceRegistry registry;
    return registry;
}

uint64 DirtySourceRegistry::MarkDirty(const StringView& path, const ContentHash& baseSourceHash,
    int64 localID, const StringView& reason)
{
    const String normalized = NormalizeDirtyPath(path);
    ScopeLock lock(_locker);
    int32 index = FindRecord(_records, normalized);
    if (index == -1)
    {
        index = _records.Count();
        DirtySourceRecord record;
        record.SourcePath = normalized;
        _records.Add(MoveTemp(record));
    }
    DirtySourceRecord& record = _records[index];
    record.BaseSourceHash = baseSourceHash;
    record.EditRevision = ++_nextRevision;
    if (localID != 0 && !record.DirtyLocalIDs.Contains(localID))
        record.DirtyLocalIDs.Add(localID);
    if (reason.HasChars())
        record.Reason = reason;
    return record.EditRevision;
}

bool DirtySourceRegistry::TryGet(const StringView& path, DirtySourceRecord& result) const
{
    const String normalized = NormalizeDirtyPath(path);
    ScopeLock lock(_locker);
    const int32 index = FindRecord(_records, normalized);
    if (index == -1)
        return false;
    result = _records[index];
    return true;
}

bool DirtySourceRegistry::ClearCommitted(const StringView& path, uint64 editRevision)
{
    if (editRevision == 0)
        return false;
    const String normalized = NormalizeDirtyPath(path);
    ScopeLock lock(_locker);
    const int32 index = FindRecord(_records, normalized);
    if (index == -1 || _records[index].EditRevision != editRevision)
        return false;
    _records.RemoveAt(index);
    return true;
}

void DirtySourceRegistry::Remove(const StringView& path)
{
    const String normalized = NormalizeDirtyPath(path);
    ScopeLock lock(_locker);
    const int32 index = FindRecord(_records, normalized);
    if (index != -1)
        _records.RemoveAt(index);
}

int32 DirtySourceRegistry::Count() const
{
    ScopeLock lock(_locker);
    return _records.Count();
}
