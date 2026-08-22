// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Why a mixed-mode record may or may not be converted.</summary>
enum class MigrationEligibility : byte
{
    AlreadyMigrated,
    ReadyToMigrate,
    MissingOriginalSource,
    Conflict,
    Unsupported,
};

/// <summary>One inventory row. Inventory itself never writes Content.</summary>
struct FLAXENGINE_API MigrationInventoryEntry
{
    Guid ID;
    String TypeName;
    String SourcePath;
    String ProposedDestination;
    String SourceKind;
    String Eligibility;
    String Reason;
};

/// <summary>Read-only mixed-mode inventory and dry-run plan fingerprint.</summary>
class FLAXENGINE_API MigrationInventory
{
public:
    static constexpr int32 FormatVersion = 1;

    static const Char* GetEligibilityName(MigrationEligibility eligibility);
    static MigrationEligibility Classify(const AssetRecord& record, String& reason, String& proposedDestination);
    static void Build(const Array<AssetRecord>& records, Array<MigrationInventoryEntry>& entries);
    static bool WriteCanonicalJson(const Array<MigrationInventoryEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool Fingerprint(const Array<MigrationInventoryEntry>& entries, const Array<Guid>& selected, const StringView& backupRoot, StringAnsi& fingerprint, AssetPipelineDiagnostic& diagnostic);
    static bool HasBlockingConflict(const Array<MigrationInventoryEntry>& entries);
};
