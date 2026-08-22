// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "MigrationInventory.h"

/// <summary>Durable migration journal states. Commit is illegal before publish/verify.</summary>
enum class MigrationJournalState : byte
{
    None,
    Planned,
    BackedUp,
    Published,
    Committed,
    RolledBack,
    Failed,
};

/// <summary>One idempotent per-asset conversion recorded in the journal.</summary>
struct FLAXENGINE_API MigrationJournalOperation
{
    Guid AssetID;
    String Kind;
    String SourcePath;
    String DestinationPath;
    String BackupPath;
    String BeforeHash;
    String AfterHash;
    String State;
};

/// <summary>Resumable mixed-mode conversion journal with hash-safe rollback.</summary>
struct FLAXENGINE_API MigrationJournal
{
    int32 FormatVersion = 1;
    StringAnsi PlanFingerprint;
    String BackupRoot;
    String State;
    Array<Guid> Selected;
    Array<MigrationJournalOperation> Operations;
};

/// <summary>Plan, backup, publish, commit, and roll back selected inventory rows.</summary>
class FLAXENGINE_API MigrationSession
{
public:
    static const Char* GetStateName(MigrationJournalState state);
    static bool ParseState(const StringView& text, MigrationJournalState& state);
    static bool CreatePlan(const Array<MigrationInventoryEntry>& inventory, const Array<Guid>& selected, const StringView& backupRoot, MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool WriteCanonicalJson(const MigrationJournal& journal, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool ParseCanonicalJson(const StringAnsiView& json, MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool SaveAtomic(const StringView& path, const MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool Load(const StringView& path, MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool Backup(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool Publish(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool Commit(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool Rollback(MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
    static bool EnsureCurrentFingerprint(const Array<MigrationInventoryEntry>& inventory, const MigrationJournal& journal, AssetPipelineDiagnostic& diagnostic);
};
