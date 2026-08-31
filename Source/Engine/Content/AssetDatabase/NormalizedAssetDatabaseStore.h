// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseTransaction.h"

/// <summary>One committed normalized-table WAL transaction.</summary>
struct NormalizedAssetDatabaseWalRecord
{
    uint64 BaseRevision = 0;
    uint64 Revision = 0;
    Array<AssetDatabaseMutation> Mutations;
    AssetChangeSet Changes;
};

/// <summary>
/// Embedded durable normalized-table store. Checkpoints are generation-addressed table files and
/// commits are append-only, checksummed WAL frames published at one revision.
/// </summary>
class NormalizedAssetDatabaseStore
{
public:
    static String GetManifestPath(const StringView& directory);
    static String GetWalPath(const StringView& directory);

    static bool LoadCheckpoint(const StringView& directory, const Guid& projectId, SourceAssetDatabaseState& state,
        uint64& generation, AssetPipelineDiagnostic& diagnostic);
    static bool SaveCheckpoint(const StringView& directory, const SourceAssetDatabaseState& state,
        uint64& generation, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Opens and tail-recovers WAL, returning records newer than checkpointRevision.</summary>
    static bool OpenWal(const StringView& path, const Guid& projectId, uint64 checkpointRevision,
        uint64& baseRevision, uint64& lastRevision, Array<NormalizedAssetDatabaseWalRecord>& records,
        AssetPipelineDiagnostic& diagnostic);
    static bool AppendWal(const StringView& path, const Guid& projectId, uint64& lastRevision,
        const NormalizedAssetDatabaseWalRecord& record, AssetPipelineDiagnostic& diagnostic);
    static bool ResetWal(const StringView& path, const Guid& projectId, uint64 baseRevision,
        AssetPipelineDiagnostic& diagnostic);
};
