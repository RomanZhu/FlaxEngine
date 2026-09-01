// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Platform/CriticalSection.h"

/// <summary>One exact dirty authored-source edit revision.</summary>
struct FLAXENGINE_API DirtySourceRecord
{
    String SourcePath;
    ContentHash BaseSourceHash;
    uint64 EditRevision = 0;
    Array<int64> DirtyLocalIDs;
    String Reason;
};

/// <summary>Thread-safe per-source dirty state cleared only for the committed edit revision.</summary>
class FLAXENGINE_API DirtySourceRegistry
{
private:
    mutable CriticalSection _locker;
    Array<DirtySourceRecord> _records;
    uint64 _nextRevision = 0;

public:
    DirtySourceRegistry() = default;

    static DirtySourceRegistry& Get();

    /// <summary>Marks a source dirty and returns its new edit revision.</summary>
    uint64 MarkDirty(const StringView& path, const ContentHash& baseSourceHash,
        int64 localID = 0, const StringView& reason = StringView::Empty);

    /// <summary>Gets the current dirty record for a source.</summary>
    bool TryGet(const StringView& path, DirtySourceRecord& result) const;

    /// <summary>Clears the source only when the exact committed edit revision is still current.</summary>
    bool ClearCommitted(const StringView& path, uint64 editRevision);

    /// <summary>Removes all dirty state for a source.</summary>
    void Remove(const StringView& path);

    int32 Count() const;
};
