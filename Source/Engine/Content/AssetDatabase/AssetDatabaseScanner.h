// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabase.h"
#include "AssetDatabaseSnapshot.h"

/// <summary>Controls a canonical Content-root scan.</summary>
struct FLAXENGINE_API AssetDatabaseScanOptions
{
    int32 AssetSystemVersion = 2;
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
    /// <summary>Collects records for one source root without publishing them.</summary>
    /// <returns>True if enumeration failed. Content diagnostics are returned in result.</returns>
    static bool Collect(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const AssetDatabaseScanOptions& options, const AssetDatabaseSnapshot& previous, Array<AssetRecord>& records, AssetDatabaseScanResult& result);

    /// <summary>Collects records for an explicit file list without enumerating the Content tree.</summary>
    /// <returns>True if hashing failed fatally. Content diagnostics are returned in result.</returns>
    static bool CollectFromFiles(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const Array<String>& files, const AssetDatabaseScanOptions& options, const AssetDatabaseSnapshot& previous, Array<AssetRecord>& records, AssetDatabaseScanResult& result);

    /// <returns>True if enumeration or publication itself failed. Content diagnostics are returned in result.</returns>
    static bool Scan(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const AssetDatabaseScanOptions& options, AssetDatabase& database, AssetDatabaseScanResult& result);
};
