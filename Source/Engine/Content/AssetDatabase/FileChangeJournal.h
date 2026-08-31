// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetChangeSet.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Durable append-only journal of committed ordered asset change sets.</summary>
class FLAXENGINE_API FileChangeJournal
{
private:
    String _path;
    uint64 _baseRevision = 0;
    uint64 _lastRevision = 0;
    bool _open = false;

public:
    /// <summary>Opens the journal and discards only an incomplete/corrupt tail. Returns true on failure.</summary>
    bool Open(const StringView& path, uint64 baseRevision, AssetPipelineDiagnostic& diagnostic);
    void Close();
    bool IsOpen() const;
    uint64 GetBaseRevision() const;
    uint64 GetLastRevision() const;

    /// <summary>Atomically compacts all retained history into a snapshot cursor.</summary>
    bool Reset(uint64 baseRevision, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Appends exactly the next revision. Re-appending the current tail is idempotent.</summary>
    bool Append(const AssetChangeSet& changeSet, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Reads committed changes after a cursor. Sets requiresSnapshot when history was compacted.</summary>
    bool ReadAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const;
};
