// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabase.h"
#include "AssetDatabaseSnapshot.h"

/// <summary>Controls a canonical Content-root scan.</summary>
struct FLAXENGINE_API AssetDatabaseScanOptions
{
    bool StrictMetadata = false;
    int32 MaximumFiles = 1000000;
    const bool* Cancel = nullptr;
    SourceHashCache* HashCache = nullptr;
};

/// <summary>Result of one full source/sidecar reconciliation.</summary>
struct FLAXENGINE_API AssetDatabaseScanResult
{
    uint64 Revision = 0;
    int32 FilesExamined = 0;
    int32 SidecarsParsed = 0;
    bool Cancelled = false;
    Array<AssetPipelineDiagnostic> Diagnostics;
    Array<AssetDatabaseFileState> FileStates;

    bool HasBlockingDiagnostics() const;
};

/// <summary>Discovers exact source plus adjacent .meta pairs without opening generated artifacts.</summary>
class FLAXENGINE_API AssetDatabaseScanner
{
public:
    /// <returns>True if enumeration or publication itself failed. Content diagnostics are returned in result.</returns>
    static bool Scan(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const AssetDatabaseScanOptions& options, AssetDatabase& database, AssetDatabaseScanResult& result);
};
