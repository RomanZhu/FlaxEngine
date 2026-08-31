// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Completed one-way project migration phase.</summary>
API_ENUM() enum class ProjectMigrationPhase : byte
{
    None,
    M0PreflightAndBackup,
    M1LegacyGraphFrozen,
    M2CanonicalRootsAndSettings,
    M3AssetsClassified,
    M4MetadataAndIdentityConverted,
    M5ReferencesRewritten,
    M6AuthoredSourcesWritten,
    M7CleanDatabaseImported,
    M8SemanticallyVerified,
    M9Committed,
    RolledBack,
    Failed,
};

/// <summary>Hashed evidence supplied by refresh/import/reference/cook verification stages.</summary>
API_STRUCT() struct FLAXENGINE_API ProjectMigrationEvidence
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(ProjectMigrationEvidence);

    API_FIELD() bool LegacyGraphFrozen = false;
    API_FIELD() bool ReferenceRewriteComplete = false;
    API_FIELD() bool AuthoredSourcesVerified = false;
    API_FIELD() bool CleanRefreshComplete = false;
    API_FIELD() bool ImportsComplete = false;
    API_FIELD() bool PersistentReferencesVerified = false;
    API_FIELD() bool HostCookSucceeded = false;
    API_FIELD() String ReportJson;
};

/// <summary>Durable project migration operation result.</summary>
API_STRUCT() struct FLAXENGINE_API ProjectMigrationResult
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(ProjectMigrationResult);

    API_FIELD() bool Succeeded = false;
    API_FIELD() bool Completed = false;
    API_FIELD() bool AwaitingExternalWork = false;
    API_FIELD() Guid TransactionId = Guid::Empty;
    API_FIELD() ProjectMigrationPhase Phase = ProjectMigrationPhase::None;
    API_FIELD() String JournalPath;
    API_FIELD() String BackupRoot;
    API_FIELD() String SourceTreeFingerprint;
    API_FIELD() String VerificationFingerprint;
    API_FIELD() String Message;
    API_FIELD() Array<AssetPipelineDiagnostic> Diagnostics;
};

/// <summary>Resumable M0-M9 project migration coordinator with backup-first and marker-last semantics.</summary>
API_CLASS(Static) class FLAXENGINE_API ProjectMigrationOrchestrator
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(ProjectMigrationOrchestrator);

public:
    /// <summary>Runs M0 preflight, takes a complete external backup, and creates the durable journal.</summary>
    API_FUNCTION() static ProjectMigrationResult Begin(const StringView& projectDescriptorPath, const StringView& contentRoot,
        const StringView& backupParent, const StringView& journalPath);

    /// <summary>Resumes exactly one idempotent phase. External verification phases require hashed evidence.</summary>
    API_FUNCTION() static ProjectMigrationResult Resume(const StringView& journalPath, const ProjectMigrationEvidence& evidence);

    /// <summary>Loads journal state without mutation.</summary>
    API_FUNCTION() static ProjectMigrationResult Inspect(const StringView& journalPath);

    /// <summary>Restores the verified M0 backup before M9. Refuses rollback after edits or marker commit.</summary>
    API_FUNCTION() static ProjectMigrationResult Rollback(const StringView& journalPath);
};
