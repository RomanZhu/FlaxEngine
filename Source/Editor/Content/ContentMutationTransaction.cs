// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json;
using FlaxEngine;

namespace FlaxEditor.Content
{
    internal sealed class ContentMutationStep
    {
        public readonly string Name;
        public readonly int[] EntryIndices;
        public readonly Func<ContentMutationResult> Commit;
        public readonly Func<bool> Rollback;
        public readonly Func<bool> Verify;

        public ContentMutationStep(string name, int[] entryIndices, Func<ContentMutationResult> commit, Func<bool> rollback, Func<bool> verify = null)
        {
            Name = name;
            EntryIndices = entryIndices ?? Array.Empty<int>();
            Commit = commit ?? throw new ArgumentNullException(nameof(commit));
            Rollback = rollback ?? throw new ArgumentNullException(nameof(rollback));
            Verify = verify;
        }
    }

    internal sealed class ContentMutationTransaction
    {
        private const int JournalVersion = 1;
        private const string RecoveryFolderName = "ContentMutationRecovery";
        private readonly ContentMutationPlan _plan;
        private readonly string _journalRoot;
        private readonly string _journalPath;
        private readonly JournalDocument _journal;

#if FLAX_TESTS
        internal static Func<string, Exception> FaultInjector;
#endif

        private sealed class JournalDocument
        {
            public int Version = JournalVersion;
            public ContentMutationPlan Plan;
            public ContentMutationJournalState State;
            public DateTime UpdatedUtc;
            public string LastError;
        }

        private sealed class MutationStepException : Exception
        {
            public readonly ContentMutationResult Result;

            public MutationStepException(ContentMutationResult result)
            : base(result.Message)
            {
                Result = result;
            }
        }

        public ContentMutationTransaction(ContentMutationPlan plan, string journalRoot = null)
        {
            _plan = plan ?? throw new ArgumentNullException(nameof(plan));
            _journalRoot = journalRoot ?? StringUtils.CombinePaths(Globals.ProjectCacheFolder, RecoveryFolderName);
            _journalPath = Path.Combine(_journalRoot, _plan.Id.ToString("N") + ".json");
            _journal = new JournalDocument
            {
                Plan = _plan,
                State = ContentMutationJournalState.Prepared,
                UpdatedUtc = DateTime.UtcNow,
            };
        }

        public ContentMutationResult Execute(IReadOnlyList<ContentMutationStep> steps)
        {
            if (CanonicalGraphDocuments.UseNewAssetDatabase && _plan.Entries.Any(x =>
                    ContentMutationPathUtils.IsWithinRoot(x.SourcePath, Globals.ProjectContentFolder) ||
                    ContentMutationPathUtils.IsWithinRoot(x.DestinationPath, Globals.ProjectContentFolder)))
            {
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, _plan.Entries.FirstOrDefault()?.SourcePath,
                    _plan.Entries.FirstOrDefault()?.DestinationPath,
                    "Asset System v3 project-tree mutations must use the native AssetMutationService.", transactionId: _plan.Id);
            }
            var preflight = _plan.Preflight();
            if (!preflight.Succeeded)
                return preflight;
            if (steps == null || steps.Count == 0)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, preflight.SourcePath, preflight.DestinationPath, "The transaction contains no commit steps.", transactionId: _plan.Id);

            try
            {
                InjectFault("preflight-complete");
                PersistJournal();
            }
            catch (Exception ex)
            {
                TryDeleteJournal();
                ContentMutationDiagnostics.Log("transaction.journal-failed", $"id={_plan.Id:N}; state=Prepared; message='{ContentMutationDiagnostics.Sanitize(ex.Message)}'");
                return ContentMutationResult.Fail(ContentMutationFailure.JournalFailure, preflight.SourcePath, preflight.DestinationPath, ex.Message, transactionId: _plan.Id);
            }

            var attemptedSteps = new List<int>(steps.Count);
            var completedPaths = new List<string>();
            ContentMutationResult failure = default;
            try
            {
                SetJournalState(ContentMutationJournalState.Committing);
                for (int i = 0; i < steps.Count; i++)
                {
                    var step = steps[i];
                    attemptedSteps.Add(i);
                    SetEntryStates(step.EntryIndices, ContentMutationEntryState.Committing);
                    PersistJournal();
                    InjectFault("before-" + step.Name);

                    var commitPreflight = _plan.VerifyBeforeCommit(step.EntryIndices);
                    if (!commitPreflight.Succeeded)
                        throw new MutationStepException(commitPreflight);

                    var result = step.Commit();
                    if (!result.Succeeded)
                        throw new MutationStepException(result);
                    if (step.Verify != null && !step.Verify())
                        throw new MutationStepException(ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, preflight.SourcePath, preflight.DestinationPath, $"Transaction step '{step.Name}' did not verify.", transactionId: _plan.Id));

                    SetEntryStates(step.EntryIndices, ContentMutationEntryState.Committed);
                    for (int j = 0; j < step.EntryIndices.Length; j++)
                    {
                        var index = step.EntryIndices[j];
                        if ((uint)index < (uint)_plan.Entries.Count)
                            completedPaths.Add(_plan.Entries[index].DestinationPath);
                    }
                    PersistJournal();
                    InjectFault("after-" + step.Name);
                }

                SetJournalState(ContentMutationJournalState.Committed);
                TryDeleteJournal();
                ContentMutationDiagnostics.Log("transaction.committed", $"id={_plan.Id:N}; operation={_plan.Operation}; entries={_plan.Entries.Count}; steps={steps.Count}");
                return ContentMutationResult.Success(preflight.SourcePath, preflight.DestinationPath, _plan.Id, completedPaths.ToArray());
            }
            catch (MutationStepException ex)
            {
                failure = ex.Result;
            }
            catch (UnauthorizedAccessException ex)
            {
                failure = ContentMutationResult.Fail(ContentMutationFailure.PermissionDenied, preflight.SourcePath, preflight.DestinationPath, ex.Message, transactionId: _plan.Id);
            }
            catch (IOException ex)
            {
                failure = ContentMutationResult.Fail(ContentMutationFailure.LockedStorage, preflight.SourcePath, preflight.DestinationPath, ex.Message, transactionId: _plan.Id);
            }
            catch (Exception ex)
            {
                failure = ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, preflight.SourcePath, preflight.DestinationPath, ex.Message, transactionId: _plan.Id);
            }

            return Rollback(steps, attemptedSteps, completedPaths, failure);
        }

        private ContentMutationResult Rollback(IReadOnlyList<ContentMutationStep> steps, List<int> attemptedSteps, List<string> completedPaths, ContentMutationResult failure)
        {
            var rolledBackPaths = new List<string>();
            bool rollbackFailed = false;
            _journal.LastError = failure.Message;
            TrySetJournalState(ContentMutationJournalState.RollingBack);
            for (int i = attemptedSteps.Count - 1; i >= 0; i--)
            {
                var step = steps[attemptedSteps[i]];
                SetEntryStates(step.EntryIndices, ContentMutationEntryState.RollingBack);
                TryPersistJournal();
                try
                {
                    InjectFault("rollback-" + step.Name);
                    if (!step.Rollback())
                    {
                        rollbackFailed = true;
                        SetEntryStates(step.EntryIndices, ContentMutationEntryState.Failed);
                        continue;
                    }
                    SetEntryStates(step.EntryIndices, ContentMutationEntryState.RolledBack);
                    for (int j = 0; j < step.EntryIndices.Length; j++)
                    {
                        var index = step.EntryIndices[j];
                        if ((uint)index < (uint)_plan.Entries.Count)
                            rolledBackPaths.Add(_plan.Entries[index].SourcePath);
                    }
                }
                catch (Exception ex)
                {
                    rollbackFailed = true;
                    _journal.LastError = (_journal.LastError + Environment.NewLine + ex.Message).Trim();
                    SetEntryStates(step.EntryIndices, ContentMutationEntryState.Failed);
                }
                TryPersistJournal();
            }

            if (rollbackFailed)
            {
                TrySetJournalState(ContentMutationJournalState.RecoveryRequired);
                var recoveryPaths = string.Join("; ", _plan.Entries.Select(x => $"'{x.SourcePath}' <-> '{x.DestinationPath}'"));
                LogError($"Content transaction {_plan.Id:N} requires recovery. Paths: {recoveryPaths}. Journal: '{_journalPath}'.");
                ContentMutationDiagnostics.Log("transaction.recovery-required", $"id={_plan.Id:N}; journal='{_journalPath}'; failure={failure.Failure}");
                return ContentMutationResult.Fail(ContentMutationFailure.RollbackFailure, failure.SourcePath, failure.DestinationPath, failure.Message, true, _plan.Id, completedPaths.ToArray(), rolledBackPaths.ToArray());
            }

            TrySetJournalState(ContentMutationJournalState.RolledBack);
            TryDeleteJournal();
            ContentMutationDiagnostics.Log("transaction.rolled-back", $"id={_plan.Id:N}; operation={_plan.Operation}; failure={failure.Failure}; attempted={attemptedSteps.Count}");
            return ContentMutationResult.Fail(failure.Failure, failure.SourcePath, failure.DestinationPath, failure.Message, false, _plan.Id, completedPaths.ToArray(), rolledBackPaths.ToArray());
        }

        private void SetEntryStates(int[] indices, ContentMutationEntryState state)
        {
            for (int i = 0; i < indices.Length; i++)
            {
                var index = indices[i];
                if ((uint)index < (uint)_plan.Entries.Count)
                    _plan.Entries[index].State = state;
            }
        }

        private void SetJournalState(ContentMutationJournalState state)
        {
            _journal.State = state;
            _journal.UpdatedUtc = DateTime.UtcNow;
            PersistJournal();
        }

        private void TrySetJournalState(ContentMutationJournalState state)
        {
            _journal.State = state;
            _journal.UpdatedUtc = DateTime.UtcNow;
            TryPersistJournal();
        }

        private void PersistJournal()
        {
            InjectFault("before-journal-persist");
            Directory.CreateDirectory(_journalRoot);
            _journal.UpdatedUtc = DateTime.UtcNow;
            var temporaryPath = _journalPath + ".tmp";
            File.WriteAllText(temporaryPath, JsonConvert.SerializeObject(_journal, Formatting.Indented));
            File.Move(temporaryPath, _journalPath, true);
            InjectFault("after-journal-persist");
        }

        private bool TryPersistJournal()
        {
            try
            {
                PersistJournal();
                return true;
            }
            catch (Exception ex)
            {
                LogWarning($"Failed to persist content mutation journal '{_journalPath}': {ex.Message}");
                return false;
            }
        }

        private void TryDeleteJournal()
        {
            try
            {
                InjectFault("before-journal-remove");
                if (File.Exists(_journalPath))
                    File.Delete(_journalPath);
                var temporaryPath = _journalPath + ".tmp";
                if (File.Exists(temporaryPath))
                    File.Delete(temporaryPath);
                InjectFault("after-journal-remove");
            }
            catch (Exception ex)
            {
                // A committed/rolled-back journal is harmless and will be cleaned on startup.
                LogWarning($"Failed to remove completed content mutation journal '{_journalPath}': {ex.Message}");
            }
        }

        private static void InjectFault(string point)
        {
#if FLAX_TESTS
            var exception = FaultInjector?.Invoke(point);
            if (exception != null)
                throw exception;
#endif
        }

        internal static int RecoverPendingTransactions(string journalRoot = null)
        {
            return RecoverPendingTransactions(null, journalRoot);
        }

        internal static int RecoverPendingTransactions(List<string> recoveredImportSources, string journalRoot = null)
        {
            journalRoot ??= StringUtils.CombinePaths(Globals.ProjectCacheFolder, RecoveryFolderName);
            if (!Directory.Exists(journalRoot))
                return 0;

            int recoveryRequired = 0;
            foreach (var journalPath in Directory.EnumerateFiles(journalRoot, "*.json", SearchOption.TopDirectoryOnly))
            {
                JournalDocument journal;
                try
                {
                    journal = JsonConvert.DeserializeObject<JournalDocument>(File.ReadAllText(journalPath));
                }
                catch (Exception ex)
                {
                    recoveryRequired++;
                    LogError($"Cannot read content recovery journal '{journalPath}': {ex.Message}");
                    continue;
                }

                if (journal?.Plan == null)
                {
                    recoveryRequired++;
                    LogError($"Content recovery journal '{journalPath}' is invalid and was preserved.");
                    continue;
                }
                if (journal.State == ContentMutationJournalState.Prepared || journal.State == ContentMutationJournalState.Committed || journal.State == ContentMutationJournalState.RolledBack)
                {
                    TryDeleteRecoveredJournal(journalPath);
                    continue;
                }

                if (TryRecoverJournal(journal, recoveredImportSources))
                {
                    TryDeleteRecoveredJournal(journalPath);
                    ContentMutationDiagnostics.Log("transaction.startup-recovered", $"id={journal.Plan.Id:N}; operation={journal.Plan.Operation}; journal='{journalPath}'");
                    continue;
                }

                recoveryRequired++;
                journal.State = ContentMutationJournalState.RecoveryRequired;
                journal.UpdatedUtc = DateTime.UtcNow;
                if (string.IsNullOrWhiteSpace(journal.LastError))
                    journal.LastError = $"Automatic startup recovery could not safely resolve the {journal.Plan.Operation} transaction.";
                try
                {
                    File.WriteAllText(journalPath, JsonConvert.SerializeObject(journal, Formatting.Indented));
                }
                catch
                {
                    // Preserve the original journal if it cannot be updated.
                }
                var paths = string.Join("; ", journal.Plan.Entries.Select(x => $"'{x.SourcePath}' <-> '{x.DestinationPath}'"));
                LogError($"Content transaction {journal.Plan.Id:N} requires recovery. Paths: {paths}. Journal: '{journalPath}'.");
            }
            return recoveryRequired;
        }

        internal static string PreserveRecoveryRecord(ContentMutationPlan plan, string error, string journalRoot = null)
        {
            try
            {
                var transaction = new ContentMutationTransaction(plan, journalRoot);
                transaction._journal.State = ContentMutationJournalState.RecoveryRequired;
                transaction._journal.LastError = error;
                transaction.PersistJournal();
                var paths = string.Join("; ", plan.Entries.Select(x => $"'{x.SourcePath}' <-> '{x.DestinationPath}'"));
                LogError($"Content transaction {plan.Id:N} requires recovery. Paths: {paths}. Journal: '{transaction._journalPath}'.");
                return transaction._journalPath;
            }
            catch (Exception ex)
            {
                LogError($"Failed to preserve content recovery record {plan?.Id:N}: {ex.Message}");
                return null;
            }
        }

        private static void LogError(string message)
        {
            try
            {
                Editor.LogError(message);
            }
            catch (DllNotFoundException)
            {
                Console.Error.WriteLine(message);
            }
            catch (EntryPointNotFoundException)
            {
                Console.Error.WriteLine(message);
            }
        }

        private static void LogWarning(string message)
        {
            try
            {
                Editor.LogWarning(message);
            }
            catch (DllNotFoundException)
            {
                Console.Error.WriteLine(message);
            }
            catch (EntryPointNotFoundException)
            {
                Console.Error.WriteLine(message);
            }
        }

        private static bool TryRecoverJournal(JournalDocument journal, List<string> recoveredImportSources)
        {
            switch (journal.Plan.Operation)
            {
            case ContentMutationOperationKind.Move:
            case ContentMutationOperationKind.Rename:
            case ContentMutationOperationKind.Delete:
            case ContentMutationOperationKind.Restore:
                return TryRecoverMoveLikeJournal(journal);
            case ContentMutationOperationKind.Copy:
            case ContentMutationOperationKind.Create:
                return TryRemoveCreatedDestinations(journal);
            case ContentMutationOperationKind.ImportOutput:
            {
                if (!TryRecoverImportJournal(journal, out var sourcePaths))
                    return false;
                if (recoveredImportSources != null)
                {
                    for (int i = 0; i < sourcePaths.Length; i++)
                    {
                        if (!recoveredImportSources.Contains(sourcePaths[i], ContentMutationPathUtils.Comparer))
                            recoveredImportSources.Add(sourcePaths[i]);
                    }
                }
                return true;
            }
            case ContentMutationOperationKind.Cleanup:
                return TryRecoverCleanupJournal(journal);
            default:
                return false;
            }
        }

        private static bool TryRecoverMoveLikeJournal(JournalDocument journal)
        {
            try
            {
                for (int i = journal.Plan.Entries.Count - 1; i >= 0; i--)
                {
                    var entry = journal.Plan.Entries[i];
                    if (entry.Role == ContentMutationPathRole.Descendant || entry.State == ContentMutationEntryState.Prepared)
                        continue;
                    var sourceExists = ContentMutationPathUtils.Exists(entry.SourcePath);
                    var destinationExists = ContentMutationPathUtils.Exists(entry.DestinationPath);
                    if (sourceExists && !destinationExists)
                        continue;
                    if (sourceExists && destinationExists && (journal.Plan.Operation == ContentMutationOperationKind.Delete || journal.Plan.Operation == ContentMutationOperationKind.Restore))
                    {
                        if (!TryDeletePath(entry.DestinationPath))
                            return false;
                        continue;
                    }
                    if (sourceExists || !destinationExists)
                        return false;

                    var parent = Path.GetDirectoryName(entry.SourcePath);
                    if (!Directory.Exists(parent))
                        Directory.CreateDirectory(parent);
                    if (Directory.Exists(entry.DestinationPath))
                        Directory.Move(entry.DestinationPath, entry.SourcePath);
                    else
                        File.Move(entry.DestinationPath, entry.SourcePath);
                }
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static bool TryRemoveCreatedDestinations(JournalDocument journal)
        {
            try
            {
                for (int i = journal.Plan.Entries.Count - 1; i >= 0; i--)
                {
                    var entry = journal.Plan.Entries[i];
                    if (entry.Role == ContentMutationPathRole.Descendant || entry.State == ContentMutationEntryState.Prepared)
                        continue;
                    if (ContentMutationPathUtils.Exists(entry.DestinationPath) && !TryDeletePath(entry.DestinationPath))
                        return false;
                }
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static bool TryRecoverImportJournal(JournalDocument journal, out string[] recoveredSourcePaths)
        {
            recoveredSourcePaths = Array.Empty<string>();
            try
            {
                var output = journal.Plan.Entries.FirstOrDefault(x => x.Role == ContentMutationPathRole.Main);
                var backup = journal.Plan.Entries.FirstOrDefault(x => x.Role == ContentMutationPathRole.ReplacementBackup);
                if (output == null)
                    return TryRecoverMetadataOnlyImportJournal(journal, out recoveredSourcePaths);

                // A normal import transaction rolls back its generated metadata
                // before restoring or removing the interrupted main output.
                for (int i = journal.Plan.Entries.Count - 1; i >= 0; i--)
                {
                    var entry = journal.Plan.Entries[i];
                    if (entry.Role != ContentMutationPathRole.MetadataSidecar || entry.State == ContentMutationEntryState.Prepared)
                        continue;
                    if (ContentMutationPathUtils.Exists(entry.DestinationPath) && !TryDeletePath(entry.DestinationPath))
                        return false;
                }

                if (output.State != ContentMutationEntryState.Prepared)
                {
                    if (backup != null && File.Exists(backup.DestinationPath))
                    {
                        var parent = Path.GetDirectoryName(output.DestinationPath);
                        if (!Directory.Exists(parent))
                            Directory.CreateDirectory(parent);
                        if (File.Exists(output.DestinationPath))
                            File.Replace(backup.DestinationPath, output.DestinationPath, null);
                        else
                            File.Move(backup.DestinationPath, output.DestinationPath);
                    }
                    else if (ContentMutationPathUtils.Exists(output.DestinationPath) && !TryDeletePath(output.DestinationPath))
                    {
                        return false;
                    }
                }

                if (backup != null && ContentMutationPathUtils.Exists(backup.DestinationPath) && !TryDeletePath(backup.DestinationPath))
                    return false;
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static bool TryRecoverMetadataOnlyImportJournal(JournalDocument journal, out string[] recoveredSourcePaths)
        {
            recoveredSourcePaths = Array.Empty<string>();
            var metadataEntries = journal.Plan.Entries.Where(x => x.Role == ContentMutationPathRole.MetadataSidecar).ToArray();
            if (metadataEntries.Length == 0)
                return false;

            var recovered = new HashSet<string>(ContentMutationPathUtils.Comparer);
            for (int i = 0; i < metadataEntries.Length; i++)
            {
                var entry = metadataEntries[i];
                var metadataPath = ContentMutationPathUtils.Normalize(entry.DestinationPath);
                if (metadataPath == null || !metadataPath.EndsWith(".meta", ContentMutationPathUtils.Comparison))
                    return false;
                var sourcePath = metadataPath.Substring(0, metadataPath.Length - 5);
                var backup = journal.Plan.Entries.FirstOrDefault(x =>
                    x.Role == ContentMutationPathRole.ReplacementBackup && ContentMutationPathUtils.AreEquivalent(x.SourcePath, metadataPath));

                if (entry.State == ContentMutationEntryState.Prepared)
                {
                    if (backup != null && ContentMutationPathUtils.Exists(backup.DestinationPath) && !TryRestoreRecoveryPath(backup.DestinationPath, metadataPath))
                        return false;
                    if (!ContentMutationPathUtils.AreEquivalent(entry.SourcePath, sourcePath) && File.Exists(entry.SourcePath) && !TryDeletePath(entry.SourcePath))
                        return false;
                    continue;
                }

                if (!File.Exists(sourcePath))
                    return false;
                if (!File.Exists(metadataPath))
                {
                    var stagedMetadata = !ContentMutationPathUtils.AreEquivalent(entry.SourcePath, sourcePath) && File.Exists(entry.SourcePath);
                    if (stagedMetadata)
                    {
                        Directory.CreateDirectory(Path.GetDirectoryName(metadataPath));
                        File.Move(entry.SourcePath, metadataPath);
                    }
                    else if (backup != null && ContentMutationPathUtils.Exists(backup.DestinationPath))
                    {
                        if (!TryRestoreRecoveryPath(backup.DestinationPath, metadataPath))
                            return false;
                    }
                    else
                    {
                        return false;
                    }
                }

                // Database state and generated artifacts are reconstructed after
                // the startup scan. Staging files and superseded metadata backups
                // are no longer authoritative once a sidecar is present.
                if (!ContentMutationPathUtils.AreEquivalent(entry.SourcePath, sourcePath) && File.Exists(entry.SourcePath) && !TryDeletePath(entry.SourcePath))
                    return false;
                if (backup != null && ContentMutationPathUtils.Exists(backup.DestinationPath) && !TryDeletePath(backup.DestinationPath))
                    return false;
                recovered.Add(sourcePath);
            }

            recoveredSourcePaths = recovered.ToArray();
            return true;
        }

        private static bool TryRecoverCleanupJournal(JournalDocument journal)
        {
            try
            {
                for (int i = journal.Plan.Entries.Count - 1; i >= 0; i--)
                {
                    var entry = journal.Plan.Entries[i];
                    if (ContentMutationPathUtils.Exists(entry.SourcePath) && !TryDeletePath(entry.SourcePath))
                        return false;
                }
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static bool TryRestoreRecoveryPath(string backupPath, string destinationPath)
        {
            try
            {
                var parent = Path.GetDirectoryName(destinationPath);
                if (!Directory.Exists(parent))
                    Directory.CreateDirectory(parent);
                if (File.Exists(destinationPath))
                    File.Replace(backupPath, destinationPath, null);
                else
                    File.Move(backupPath, destinationPath);
                return File.Exists(destinationPath) && !File.Exists(backupPath);
            }
            catch
            {
                return false;
            }
        }

        private static bool TryDeletePath(string path)
        {
            try
            {
                if (Directory.Exists(path))
                    Directory.Delete(path, true);
                else if (File.Exists(path))
                    File.Delete(path);
                return !ContentMutationPathUtils.Exists(path);
            }
            catch
            {
                return false;
            }
        }

        private static void TryDeleteRecoveredJournal(string journalPath)
        {
            try
            {
                File.Delete(journalPath);
            }
            catch
            {
                // A completed journal can be retried on the next startup.
            }
        }
    }
}
