// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabase.h"
#include "SourceHashCache.h"

using AssetDatabaseFileState = SourceHashFileState;

/// <summary>Versioned, checksummed and non-authoritative Library database snapshot.</summary>
class FLAXENGINE_API AssetDatabaseSnapshotStore
{
public:
    static constexpr uint32 CurrentVersion = 6;

    /// <returns>True on failure.</returns>
    static bool SaveAtomic(const StringView& path, const StringView& projectRoot, const StringView& contentRoot, const AssetDatabaseSnapshot& snapshot, const Array<AssetDatabaseFileState>& fileStates, AssetPipelineDiagnostic& diagnostic);

    /// <returns>True when the snapshot is absent, invalid, stale, or cannot be loaded. Callers should rescan.</returns>
    static bool Load(const StringView& path, const StringView& projectRoot, const StringView& contentRoot, AssetDatabase& database, Array<AssetDatabaseFileState>& fileStates, AssetPipelineDiagnostic& diagnostic);
};
