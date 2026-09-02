// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;

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
        private readonly ContentMutationPlan _plan;

#if FLAX_TESTS
        internal static Func<string, Exception> FaultInjector;
#endif

        private sealed class MutationStepException : Exception
        {
            public readonly ContentMutationResult Result;

            public MutationStepException(ContentMutationResult result)
            : base(result.Message)
            {
                Result = result;
            }
        }

        public ContentMutationTransaction(ContentMutationPlan plan)
        {
            _plan = plan ?? throw new ArgumentNullException(nameof(plan));
        }

        public ContentMutationResult Execute(IReadOnlyList<ContentMutationStep> steps)
        {
            var preflight = _plan.Preflight();
            if (!preflight.Succeeded)
                return preflight;
            if (steps == null || steps.Count == 0)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, preflight.SourcePath, preflight.DestinationPath, "The transaction contains no commit steps.", transactionId: _plan.Id);

            var attemptedSteps = new List<int>(steps.Count);
            var completedPaths = new List<string>();
            ContentMutationResult failure = default;
            try
            {
                InjectFault("preflight-complete");
                for (int i = 0; i < steps.Count; i++)
                {
                    var step = steps[i];
                    attemptedSteps.Add(i);
                    SetEntryStates(step.EntryIndices, ContentMutationEntryState.Committing);
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
                    InjectFault("after-" + step.Name);
                }

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
            var rollbackErrors = new List<string>();
            bool rollbackFailed = false;
            for (int i = attemptedSteps.Count - 1; i >= 0; i--)
            {
                var step = steps[attemptedSteps[i]];
                SetEntryStates(step.EntryIndices, ContentMutationEntryState.RollingBack);
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
                            rolledBackPaths.Add(_plan.Entries[index].DestinationPath);
                    }
                }
                catch (Exception ex)
                {
                    rollbackFailed = true;
                    rollbackErrors.Add(ex.Message);
                    SetEntryStates(step.EntryIndices, ContentMutationEntryState.Failed);
                }
            }

            if (rollbackFailed)
            {
                var message = failure.Message;
                if (rollbackErrors.Count != 0)
                    message = (message + Environment.NewLine + string.Join(Environment.NewLine, rollbackErrors)).Trim();
                ContentMutationDiagnostics.Log("transaction.rollback-failed", $"id={_plan.Id:N}; operation={_plan.Operation}; failure={failure.Failure}; attempted={attemptedSteps.Count}");
                return ContentMutationResult.Fail(ContentMutationFailure.RollbackFailure, failure.SourcePath, failure.DestinationPath, message, _plan.Id, completedPaths.ToArray(), rolledBackPaths.ToArray());
            }

            ContentMutationDiagnostics.Log("transaction.rolled-back", $"id={_plan.Id:N}; operation={_plan.Operation}; failure={failure.Failure}; attempted={attemptedSteps.Count}");
            return ContentMutationResult.Fail(failure.Failure, failure.SourcePath, failure.DestinationPath, failure.Message, _plan.Id, completedPaths.ToArray(), rolledBackPaths.ToArray());
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

        private static void InjectFault(string point)
        {
#if FLAX_TESTS
            var exception = FaultInjector?.Invoke(point);
            if (exception != null)
                throw exception;
#endif
        }
    }
}
