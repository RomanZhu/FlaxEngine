// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>
    /// Headless diagnostics and maintenance commands for the canonical asset pipeline.
    /// </summary>
    public static class CliAssetHeadlessCommands
    {
        /// <summary>Rebuilds the derived source database without deleting immutable artifacts.</summary>
        [CliCommand("assets.rebuild-database", Description = "Rebuild the derived source database from canonical sources without deleting artifacts.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult RebuildDatabase()
        {
            var databaseRoot = RequireLibraryChild("AssetDatabase");
            if (AssetPipelineService.Shutdown())
                return Failure("FLX-ASSET-DATABASE-SHUTDOWN-0006", "The source database could not be closed before rebuild.");
            try
            {
                if (Directory.Exists(databaseRoot))
                    Directory.Delete(databaseRoot, true);
            }
            catch (Exception ex)
            {
                AssetPipelineService.Initialize();
                return CliCommandResult.Failure("FLX-ASSET-DATABASE-DELETE-0006", "The derived source database could not be removed.", new { path = databaseRoot, exception = ex.Message });
            }
            if (AssetPipelineService.Initialize() || AssetPipelineService.Scan(true))
                return Failure("FLX-ASSET-DATABASE-REBUILD-0006", "The source database could not be rebuilt from canonical sources.");
            return SuccessSummary("rebuildDatabase", new { databaseRoot });
        }

        /// <summary>Forces a synchronous reimport of every supported canonical source.</summary>
        [CliCommand("assets.reimport-all", Description = "Force a synchronous reimport of every supported canonical source.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult ReimportAll()
        {
            var options = ImportAssetOptions.ImportRecursive | ImportAssetOptions.ForceSynchronousImport | ImportAssetOptions.ForceUpdate;
            if (AssetPipelineService.Refresh(options))
                return Failure("FLX-ASSET-REIMPORT-ALL-0006", "Forced canonical asset reimport failed.");
            return SuccessSummary("reimportAll", new { forced = true });
        }

        /// <summary>Dumps one source object and its related database state.</summary>
        [CliCommand("assets.dump", Description = "Dump one source object, its publications, dependencies, referencers, and diagnostics.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult Dump([CliOption("asset", Required = true)] string asset)
        {
            var objectId = ParseObjectId(asset);
            var record = ResolveRecord(objectId);
            var diagnostics = AssetDatabaseQueryService.GetDiagnostics()
                .Where(x => x.AssetGuid == record.SourceAssetID || x.AssetGuid == record.ID)
                .Select(DescribeDiagnostic)
                .ToArray();
            return CliCommandResult.Success(new
            {
                schemaVersion = 1,
                revision = AssetDatabaseQueryService.Revision,
                objectId = objectId.ToString(),
                record = DescribeRecord(record),
                publications = AssetDatabaseQueryService.GetPublications(objectId).Select(DescribePublication).ToArray(),
                dependencies = AssetDatabaseQueryService.GetDependencies(objectId).Select(DescribeDependency).ToArray(),
                referencers = AssetDatabaseQueryService.GetReferencers(objectId).Select(DescribeDependency).ToArray(),
                diagnostics,
                artifactCurrent = record.IsMain && IsArtifactSource(record) && AssetPipelineService.IsArtifactCurrent(record.ID),
            });
        }

        /// <summary>Lists normalized dependency edges for one canonical asset object.</summary>
        [CliCommand("assets.dependencies", Description = "List normalized dependency edges for one canonical asset object.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult Dependencies([CliOption("asset", Required = true)] string asset)
        {
            var objectId = ParseObjectId(asset);
            ResolveRecord(objectId);
            var edges = AssetDatabaseQueryService.GetDependencies(objectId).Select(DescribeDependency).ToArray();
            return CliCommandResult.Success(new { schemaVersion = 1, revision = AssetDatabaseQueryService.Revision, objectId = objectId.ToString(), count = edges.Length, edges });
        }

        /// <summary>Lists normalized reverse dependency edges for one canonical asset object.</summary>
        [CliCommand("assets.referencers", Description = "List normalized reverse dependency edges for one canonical asset object.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult Referencers([CliOption("asset", Required = true)] string asset)
        {
            var objectId = ParseObjectId(asset);
            ResolveRecord(objectId);
            var edges = AssetDatabaseQueryService.GetReferencers(objectId).Select(DescribeDependency).ToArray();
            return CliCommandResult.Success(new { schemaVersion = 1, revision = AssetDatabaseQueryService.Revision, objectId = objectId.ToString(), count = edges.Length, edges });
        }

        /// <summary>Strictly validates canonical project asset state.</summary>
        [CliCommand("assets.validate-project", Description = "Strictly validate canonical metadata, identities, dependency targets, and current artifacts.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandResult ValidateProject()
        {
            var metadata = CliAssetInspectionCommands.ValidateMetadata();
            if (!metadata.Succeeded)
                return metadata;

            var records = AssetDatabaseQueryService.GetRecords();
            var issues = new List<object>();
            var identities = new HashSet<string>(StringComparer.Ordinal);
            foreach (var record in records)
            {
                var objectId = ObjectId(record).ToString();
                if (!identities.Add(objectId))
                    issues.Add(new { code = "ASSET_DUPLICATE_OBJECT_ID", objectId, path = record.CanonicalPath, message = "More than one record owns the same persistent object identity." });
                if (IsBlocking(record.Status))
                    issues.Add(new { code = "ASSET_RECORD_BLOCKED", objectId, path = record.CanonicalPath, status = record.Status.ToString(), message = "The asset record is not usable." });

                foreach (var dependency in AssetDatabaseQueryService.GetDependencies(ObjectId(record)))
                {
                    if (dependency.TargetObject.IsValid && !records.Any(x => x.SourceAssetID == dependency.TargetObject.Asset.Value && x.LocalId == dependency.TargetObject.LocalId))
                    {
                        issues.Add(new
                        {
                            code = "ASSET_DEPENDENCY_TARGET_MISSING",
                            objectId,
                            target = dependency.TargetObject.ToString(),
                            origin = dependency.OriginPath,
                            message = "A recorded object dependency has no live database target.",
                        });
                    }
                    if (!string.IsNullOrEmpty(dependency.SourcePath) && !File.Exists(dependency.SourcePath) && !Directory.Exists(dependency.SourcePath))
                    {
                        issues.Add(new
                        {
                            code = "ASSET_SOURCE_DEPENDENCY_MISSING",
                            objectId,
                            sourcePath = dependency.SourcePath,
                            origin = dependency.OriginPath,
                            message = "A recorded source dependency does not exist.",
                        });
                    }
                }

                if (record.IsMain && record.Status == AssetRecordStatus.Ready && IsArtifactSource(record) && !AssetPipelineService.IsArtifactCurrent(record.ID))
                    issues.Add(new { code = "ASSET_ARTIFACT_NOT_CURRENT", objectId, path = record.CanonicalPath, processor = record.ProcessorID, message = "No exact current artifact is published for this source." });
            }

            var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Select(DescribeDiagnostic).ToArray();
            var report = new
            {
                schemaVersion = 1,
                revision = AssetDatabaseQueryService.Revision,
                records = records.Length,
                issues = issues.ToArray(),
                diagnostics,
                metadata = metadata.Data,
            };
            return issues.Count == 0
                ? CliCommandResult.Success(report)
                : CliCommandResult.Failure("FLX-ASSET-PROJECT-INVALID-0004", $"Asset project validation found {issues.Count} issue(s).", report);
        }

        /// <summary>Verifies two forced imports produce identical persistent state and artifact keys.</summary>
        [CliCommand("assets.verify-determinism", Description = "Force two synchronous imports and compare stable identities and exact publication keys.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult VerifyDeterminism()
        {
            var options = ImportAssetOptions.ImportRecursive | ImportAssetOptions.ForceSynchronousImport | ImportAssetOptions.ForceUpdate;
            if (AssetPipelineService.Refresh(options))
                return Failure("FLX-ASSET-DETERMINISM-FIRST-0006", "The first forced import pass failed.");
            var first = CaptureDeterminismState();
            if (AssetPipelineService.Refresh(options))
                return Failure("FLX-ASSET-DETERMINISM-SECOND-0006", "The second forced import pass failed.");
            var second = CaptureDeterminismState();
            var mismatches = CompareDeterminismStates(first, second).Take(100).ToArray();
            var report = new
            {
                schemaVersion = 1,
                revision = AssetDatabaseQueryService.Revision,
                firstRecords = first.RecordSignatures.Count,
                secondRecords = second.RecordSignatures.Count,
                firstPublications = first.PublicationSignatures.Count,
                secondPublications = second.PublicationSignatures.Count,
                mismatchCount = mismatches.Length,
                mismatches,
            };
            return mismatches.Length == 0
                ? CliCommandResult.Success(report)
                : CliCommandResult.Failure("FLX-ASSET-NONDETERMINISTIC-0004", "Two forced imports produced different identities or artifact keys.", report);
        }

        /// <summary>Deletes unleased immutable artifacts not reachable from current manifests.</summary>
        [CliCommand("assets.clean-unused-artifacts", Description = "Delete every unleased immutable artifact not reachable from a current manifest.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandResult CleanUnusedArtifacts()
        {
            if (AssetPipelineService.CleanUnusedArtifacts(out var result))
                return CliCommandResult.Failure("FLX-ASSET-ARTIFACT-CLEAN-0006", "Unused artifact cleanup failed.", DescribeCleanup(result));
            if (result.BlockedByInvalidManifest)
                return CliCommandResult.Failure("FLX-ASSET-ARTIFACT-MANIFEST-0004", "Unused artifacts were not deleted because a current manifest is invalid.", DescribeCleanup(result));
            return CliCommandResult.Success(DescribeCleanup(result));
        }

        private sealed class DeterminismState
        {
            public readonly Dictionary<string, string> RecordSignatures = new Dictionary<string, string>(StringComparer.Ordinal);
            public readonly Dictionary<string, string> PublicationSignatures = new Dictionary<string, string>(StringComparer.Ordinal);
        }

        private static DeterminismState CaptureDeterminismState()
        {
            var result = new DeterminismState();
            foreach (var record in AssetDatabaseQueryService.GetRecords())
            {
                var objectId = ObjectId(record);
                var key = objectId.ToString();
                result.RecordSignatures[key] = string.Join("|", record.CanonicalPath, record.SourcePath, record.SubAssetKey,
                    record.TypeName, record.ProcessorID, record.MetaSemanticHash.ToString(CultureInfo.InvariantCulture), record.Status.ToString());
                foreach (var publication in AssetDatabaseQueryService.GetPublications(objectId))
                {
                    var publicationKey = key + "|" + publication.TargetID;
                    result.PublicationSignatures[publicationKey] = publication.Artifact + "|" + publication.InputFingerprint;
                }
            }
            return result;
        }

        private static IEnumerable<object> CompareDeterminismStates(DeterminismState first, DeterminismState second)
        {
            foreach (var key in first.RecordSignatures.Keys.Union(second.RecordSignatures.Keys).OrderBy(x => x, StringComparer.Ordinal))
            {
                first.RecordSignatures.TryGetValue(key, out var before);
                second.RecordSignatures.TryGetValue(key, out var after);
                if (!string.Equals(before, after, StringComparison.Ordinal))
                    yield return new { kind = "record", objectId = key, first = before, second = after };
            }
            foreach (var key in first.PublicationSignatures.Keys.Union(second.PublicationSignatures.Keys).OrderBy(x => x, StringComparer.Ordinal))
            {
                first.PublicationSignatures.TryGetValue(key, out var before);
                second.PublicationSignatures.TryGetValue(key, out var after);
                if (!string.Equals(before, after, StringComparison.Ordinal))
                    yield return new { kind = "publication", identity = key, first = before, second = after };
            }
        }

        private static AssetObjectId ParseObjectId(string value)
        {
            if (AssetObjectId.TryParse(value, out var objectId))
                return objectId;
            var separator = value?.LastIndexOf(':') ?? -1;
            if (separator > 0 && Guid.TryParse(value.Substring(0, separator), out var compositeGuid) && compositeGuid != Guid.Empty &&
                long.TryParse(value.Substring(separator + 1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var localId) && localId != 0)
                return new AssetObjectId(new AssetGuid(compositeGuid), localId);
            if (Guid.TryParse(value, out var guid) && guid != Guid.Empty)
                return AssetObjectId.Main(new AssetGuid(guid));
            throw new ArgumentException($"Asset identity '{value}' must be guid or guid:fileId.", nameof(value));
        }

        private static AssetDatabaseRecordInfo ResolveRecord(AssetObjectId objectId)
        {
            var records = AssetDatabaseQueryService.GetRecords();
            for (var i = 0; i < records.Length; i++)
            {
                if (records[i].SourceAssetID == objectId.Asset.Value && records[i].LocalId == objectId.LocalId)
                    return records[i];
            }
            throw new FileNotFoundException($"Asset object '{objectId}' is not present in the source database.");
        }

        private static AssetObjectId ObjectId(AssetDatabaseRecordInfo record)
        {
            return new AssetObjectId(new AssetGuid(record.SourceAssetID), record.LocalId);
        }

        private static bool IsArtifactSource(AssetDatabaseRecordInfo record)
        {
            return record.SourceKind == AssetSourceKind.ImportedSource || record.SourceKind == AssetSourceKind.TextDocument;
        }

        private static bool IsBlocking(AssetRecordStatus status)
        {
            return status != AssetRecordStatus.Ready && status != AssetRecordStatus.Stale && status != AssetRecordStatus.Building;
        }

        private static string RequireLibraryChild(string name)
        {
            var libraryRoot = Path.GetFullPath(Globals.ProjectLibraryFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var result = Path.GetFullPath(Path.Combine(libraryRoot, name));
            var comparison = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!result.StartsWith(libraryRoot + Path.DirectorySeparatorChar, comparison))
                throw new InvalidOperationException($"Generated path '{result}' is outside Project Library '{libraryRoot}'.");
            return result;
        }

        private static object DescribeRecord(AssetDatabaseRecordInfo record)
        {
            return new
            {
                backingId = record.ID,
                sourceGuid = record.SourceAssetID,
                record.LocalId,
                objectId = ObjectId(record).ToString(),
                record.TypeName,
                record.CanonicalPath,
                record.SourcePath,
                record.MetaPath,
                record.SubAssetKey,
                processor = record.ProcessorID,
                record.MetaSemanticHash,
                sourceKind = record.SourceKind.ToString(),
                status = record.Status.ToString(),
                record.Revision,
                record.IsMain,
            };
        }

        private static object DescribeDependency(AssetDatabaseDependencyInfo dependency)
        {
            return new
            {
                owner = dependency.Owner.ToString(),
                targetId = dependency.TargetID,
                kind = dependency.Kind,
                targetObject = dependency.TargetObject.IsValid ? dependency.TargetObject.ToString() : null,
                dependency.SourcePath,
                exactArtifact = dependency.ExactArtifact,
                customDependency = dependency.CustomDependency,
                contentHash = dependency.ContentHash,
                origin = new { path = dependency.OriginPath, line = dependency.OriginLine, column = dependency.OriginColumn },
            };
        }

        private static object DescribePublication(AssetDatabasePublicationInfo publication)
        {
            return new
            {
                objectId = publication.Object.ToString(),
                targetId = publication.TargetID,
                artifact = publication.Artifact,
                manifestHash = publication.ManifestHash,
                inputFingerprint = publication.InputFingerprint,
                publication.SourceRevision,
                publication.ImporterRegistryGeneration,
                publication.PublishedUtcTicks,
                publication.IsLastKnownGood,
            };
        }

        private static object DescribeDiagnostic(AssetPipelineDiagnostic diagnostic)
        {
            return new
            {
                code = diagnostic.Code.ToString(),
                severity = diagnostic.Severity.ToString(),
                stage = diagnostic.Stage.ToString(),
                assetGuid = diagnostic.AssetGuid,
                sourcePath = diagnostic.SourcePath,
                processor = diagnostic.ProcessorId,
                diagnostic.Target,
                diagnostic.OutputKind,
                location = new
                {
                    file = diagnostic.Location.File,
                    line = diagnostic.Location.Line,
                    column = diagnostic.Location.Column,
                    graphNode = diagnostic.Location.GraphNode,
                    graphPin = diagnostic.Location.GraphPin,
                },
                diagnostic.Message,
                diagnostic.Remediation,
            };
        }

        private static object DescribeCleanup(AssetArtifactCleanupInfo result)
        {
            return new
            {
                schemaVersion = 1,
                result.TotalArtifactBytes,
                result.ReachableBytes,
                result.CandidateBytes,
                result.ReclaimedBytes,
                result.ScannedFiles,
                result.ReachableFiles,
                result.LeasedFiles,
                result.CandidateFiles,
                result.DeletedFiles,
                result.BlockedByInvalidManifest,
                deletedPaths = string.IsNullOrEmpty(result.DeletedPaths) ? Array.Empty<string>() : result.DeletedPaths.Split('\n'),
                diagnostics = result.Diagnostics.Select(DescribeDiagnostic).ToArray(),
            };
        }

        private static CliCommandResult SuccessSummary(string action, object details)
        {
            var records = AssetDatabaseQueryService.GetRecords();
            var diagnostics = AssetDatabaseQueryService.GetDiagnostics();
            return CliCommandResult.Success(new
            {
                schemaVersion = 1,
                action,
                revision = AssetDatabaseQueryService.Revision,
                records = records.Length,
                ready = records.Count(x => x.Status == AssetRecordStatus.Ready),
                failed = records.Count(x => IsBlocking(x.Status)),
                diagnostics = diagnostics.Select(DescribeDiagnostic).ToArray(),
                details,
            });
        }

        private static CliCommandResult Failure(string code, string message)
        {
            return CliCommandResult.Failure(code, message, new
            {
                revision = AssetDatabaseQueryService.Revision,
                diagnostics = AssetDatabaseQueryService.GetDiagnostics().Select(DescribeDiagnostic).ToArray(),
            });
        }
    }

    public sealed partial class Editor
    {
        internal void AssetHeadlessCommand(string command, string argument)
        {
            var requestId = Guid.NewGuid().ToString("N");
            try
            {
                var (registeredName, arguments) = MapHeadlessAssetCommand(command, argument);
                var registered = CliCommandRegistry.RequireCommand(CliCommandRegistry.Discover(), registeredName);
                var warnings = new List<CliCommandMessage>();
                var context = new CliCommandContext(
                    requestId,
                    Globals.ProjectFolder,
                    default,
                    (message, progress) => WriteHeadlessJson(new { type = "progress", requestId, progress, message }),
                    warning =>
                    {
                        warnings.Add(warning);
                        WriteHeadlessJson(new { type = "diagnostic", requestId, severity = "warning", warning.Code, warning.Message, warning.Details });
                    });
                WriteHeadlessJson(new { type = "started", requestId, command = "--" + command });
                var invocation = CliCommandRegistry.BeginInvoke(registered, arguments, true, context);
                if (invocation.IsCompleted)
                {
                    CompleteHeadlessAssetCommand(requestId, command, invocation.Result, warnings);
                    return;
                }

                Action update = null;
                update = () =>
                {
                    try
                    {
                        invocation.Update(TimeSpan.FromMilliseconds(10));
                        if (!invocation.IsCompleted)
                            return;
                        EditorUpdate -= update;
                        CompleteHeadlessAssetCommand(requestId, command, invocation.Result, warnings);
                    }
                    catch (Exception ex)
                    {
                        EditorUpdate -= update;
                        CompleteHeadlessAssetCommand(requestId, command,
                            CliCommandResult.Failure("FLX-ASSET-COMMAND-0006", ex.Message, new { exception = ex.ToString() }), warnings);
                    }
                };
                EditorUpdate += update;
            }
            catch (Exception ex)
            {
                CompleteHeadlessAssetCommand(requestId, command,
                    CliCommandResult.Failure("FLX-ASSET-COMMAND-0002", ex.Message, new { exception = ex.ToString() }), Array.Empty<CliCommandMessage>());
            }
        }

        private static (string Name, JObject Arguments) MapHeadlessAssetCommand(string command, string argument)
        {
            var arguments = new JObject();
            switch (command)
            {
            case "asset-refresh":
                return ("assets.refresh", arguments);
            case "asset-rebuild-database":
                return ("assets.rebuild-database", arguments);
            case "asset-reimport-all":
                return ("assets.reimport-all", arguments);
            case "asset-verify-determinism":
                return ("assets.verify-determinism", arguments);
            case "asset-dump":
                arguments["asset"] = argument;
                return ("assets.dump", arguments);
            case "asset-dependencies":
                arguments["asset"] = argument;
                return ("assets.dependencies", arguments);
            case "asset-referencers":
                arguments["asset"] = argument;
                return ("assets.referencers", arguments);
            case "asset-validate-project":
                return ("assets.validate-project", arguments);
            case "asset-clean-unused-artifacts":
                return ("assets.clean-unused-artifacts", arguments);
            default:
                throw new ArgumentException($"Unsupported headless asset command '--{command}'.", nameof(command));
            }
        }

        private static void CompleteHeadlessAssetCommand(string requestId, string command, CliCommandResult result, IEnumerable<CliCommandMessage> reportedWarnings)
        {
            var warnings = reportedWarnings.Concat(result.Warnings).ToArray();
            WriteHeadlessJson(new
            {
                schemaVersion = 1,
                type = "result",
                requestId,
                command = "--" + command,
                success = result.Succeeded,
                exitCode = result.Succeeded ? 0 : 6,
                data = result.Data,
                errors = result.Errors,
                warnings,
            });
            Engine.RequestExit(result.Succeeded ? 0 : 6);
        }

        private static void WriteHeadlessJson(object value)
        {
            var json = JsonConvert.SerializeObject(value, Formatting.None);
            Console.Out.WriteLine(json);
            Console.Out.Flush();
            Editor.Log("ASSET_CLI_JSON " + json);
        }
    }
}
