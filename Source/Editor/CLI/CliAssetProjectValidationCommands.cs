// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using FlaxEngine;
using Newtonsoft.Json;

namespace FlaxEditor
{
    /// <summary>Read-only bootstrap and project-wide hard-cut validation commands.</summary>
    public static class CliAssetProjectValidationCommands
    {
        private static bool _migrationQuiesced;

        private static CliCommandResult Quiesce()
        {
            if (ScriptsBuilder.IsCompiling || GameCooker.IsRunning || Editor.Instance.ContentImporting.IsImporting)
                return CliCommandResult.Failure("FLX-ASSET-MIGRATION-BUSY-0001", "Migration requires scripts, importing, and cooking to be idle.");
            if (!_migrationQuiesced)
            {
                Editor.Instance.ContentDatabase.SuspendAssetDatabaseAutoRefresh();
                _migrationQuiesced = true;
            }
            return null;
        }

        private static void ReleaseQuiescence()
        {
            if (!_migrationQuiesced)
                return;
            Editor.Instance.ContentDatabase.ResumeAssetDatabaseAutoRefresh();
            _migrationQuiesced = false;
        }

        private static CliCommandResult MigrationResult(ProjectMigrationResult result)
        {
            if (result.Completed || result.Phase == ProjectMigrationPhase.RolledBack)
                ReleaseQuiescence();
            return result.Succeeded
                ? CliCommandResult.Success(result)
                : CliCommandResult.Failure("FLX-ASSET-MIGRATION-0002", result.Message, result);
        }

        /// <summary>Begins M0 using external recovery state and quiesces editor mutation services.</summary>
        [CliCommand("assets.migration.v3.begin", Description = "Begin the resumable asset-system v3 migration with an external verified backup.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult BeginMigration(string backupParent = null, string journalPath = null)
        {
            var busy = Quiesce();
            if (busy != null)
                return busy;
            var project = Editor.Instance.GameProject;
            var projectParent = Directory.GetParent(project.ProjectFolderPath)?.FullName ?? project.ProjectFolderPath;
            var recoveryRoot = backupParent ?? Path.Combine(projectParent, Path.GetFileName(project.ProjectFolderPath) + "-AssetMigrationBackups");
            journalPath ??= Path.Combine(recoveryRoot, Path.GetFileNameWithoutExtension(project.ProjectPath) + ".asset-migration.journal.json");
            var result = ProjectMigrationOrchestrator.Begin(project.ProjectPath, Globals.ProjectContentFolder, recoveryRoot, journalPath);
            if (!result.Succeeded)
                ReleaseQuiescence();
            return MigrationResult(result);
        }

        /// <summary>Resumes one deterministic phase. Only the legacy-freeze boundary requires external evidence.</summary>
        [CliCommand("assets.migration.v3.resume", Description = "Resume exactly one asset-system v3 migration phase.", Access = CliCommandAccess.MutatesProject)]
        public static object ResumeMigration(string journalPath, string evidenceReportJson = null)
        {
            var busy = Quiesce();
            if (busy != null)
                return busy;
            var state = ProjectMigrationOrchestrator.Inspect(journalPath);
            if (!state.Succeeded)
                return MigrationResult(state);
            if (state.Phase == ProjectMigrationPhase.M7CleanDatabaseImported)
                return new HostCookMigrationOperation(journalPath, state.BackupRoot);
            var needsReport = state.Phase == ProjectMigrationPhase.M0PreflightAndBackup;
            if (needsReport && string.IsNullOrWhiteSpace(evidenceReportJson))
                return CliCommandResult.Failure("FLX-ASSET-MIGRATION-EVIDENCE-0003", "This migration phase requires a deterministic JSON evidence report.", state);
            var evidence = new ProjectMigrationEvidence
            {
                LegacyGraphFrozen = state.Phase == ProjectMigrationPhase.M0PreflightAndBackup,
                ReportJson = evidenceReportJson ?? "{\"schemaVersion\":1}",
            };
            return MigrationResult(ProjectMigrationOrchestrator.Resume(journalPath, evidence));
        }

        /// <summary>Restores the verified backup while rollback remains legal.</summary>
        [CliCommand("assets.migration.v3.rollback", Description = "Rollback an uncommitted v3 migration from its verified external backup.", Access = CliCommandAccess.Destructive)]
        public static CliCommandResult RollbackMigration(string journalPath)
        {
            var busy = Quiesce();
            if (busy != null)
                return busy;
            return MigrationResult(ProjectMigrationOrchestrator.Rollback(journalPath));
        }

        private sealed class HostCookMigrationOperation : CliCommandOperation
        {
            private readonly string _journalPath;
            private bool _completed;
            private CliCommandResult _result;

            public HostCookMigrationOperation(string journalPath, string backupRoot)
            {
                _journalPath = journalPath;
                GameCooker.GetCurrentPlatform(out _, out var platform, out _);
                var output = Path.Combine(backupRoot, "HostCook");
                GameCooker.Event += OnCookEvent;
                if (GameCooker.Build(platform, BuildConfiguration.Development, output, BuildOptions.None, Array.Empty<string>()))
                {
                    GameCooker.Event -= OnCookEvent;
                    _result = CliCommandResult.Failure("FLX-ASSET-MIGRATION-COOK-0004", "M8 host cook could not start.");
                    _completed = true;
                }
            }

            public override bool IsCompleted => _completed;
            public override CliCommandResult Result => _result;
            public override void Update(TimeSpan timeBudget) { }

            public override void Cancel()
            {
                GameCooker.Event -= OnCookEvent;
                if (GameCooker.IsRunning)
                    GameCooker.Cancel(false);
                _result = CliCommandResult.Failure("FLX-ASSET-MIGRATION-COOK-CANCELLED", "M8 host cook was cancelled.");
                _completed = true;
            }

            private void OnCookEvent(GameCooker.EventType type)
            {
                if (type == GameCooker.EventType.BuildStarted)
                    return;
                GameCooker.Event -= OnCookEvent;
                if (type == GameCooker.EventType.BuildFailed)
                {
                    _result = CliCommandResult.Failure("FLX-ASSET-MIGRATION-COOK-0005", "M8 host cook failed.");
                    _completed = true;
                    return;
                }
                var diagnostics = AssetDatabaseFacade.GetDiagnostics();
                var persistentReferencesValid = diagnostics.All(x => x.Severity != AssetPipelineDiagnosticSeverity.Error);
                var report = JsonConvert.SerializeObject(new
                {
                    schemaVersion = 1,
                    hostCookSucceeded = true,
                    persistentReferencesValid,
                    diagnostics,
                });
                var evidence = new ProjectMigrationEvidence
                {
                    HostCookSucceeded = true,
                    PersistentReferencesVerified = persistentReferencesValid,
                    ReportJson = report,
                };
                _result = MigrationResult(ProjectMigrationOrchestrator.Resume(_journalPath, evidence));
                _completed = true;
            }
        }

        /// <summary>Runs the deterministic read-only migration preflight.</summary>
        [CliCommand("assets.migration.preflight", Description = "Inventory migration blockers and produce a deterministic source-tree report without writes.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult MigrationPreflight()
        {
            return ValidateProject();
        }

        /// <summary>Validates the committed marker, mandatory settings role, source tree, metadata, and migration cutover.</summary>
        [CliCommand("assets.project.validate", Description = "Validate the asset-system v3 project marker and canonical source tree without writes.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult ValidateProject()
        {
            var result = AssetProjectValidator.Validate(Editor.Instance.GameProject.ProjectPath, Globals.ProjectContentFolder);
            var report = new
            {
                schemaVersion = 1,
                result.Valid,
                result.RequiresMigration,
                result.ReadOnly,
                result.SourceFiles,
                result.SourceFolders,
                result.MetadataFiles,
                result.SourceTreeFingerprint,
                settingsPath = result.Bootstrap.SettingsPath,
                settingsGuid = result.Bootstrap.SettingsGuid,
                settingsFingerprint = result.Bootstrap.SettingsFingerprint,
                canonicalReport = result.ReportJson,
                diagnostics = result.Diagnostics.Select(x => new
                {
                    code = x.Code.ToString(),
                    severity = x.Severity.ToString(),
                    stage = x.Stage.ToString(),
                    path = x.SourcePath,
                    message = x.Message,
                    remediation = x.Remediation,
                }).ToArray(),
            };
            return result.Valid
                ? CliCommandResult.Success(report)
                : CliCommandResult.Failure("FLX-ASSET-PROJECT-VALIDATION-0004", "Asset-system project validation failed.", report);
        }
    }
}
