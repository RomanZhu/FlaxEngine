// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>Immutable request passed from the editor coordinator to an importer worker.</summary>
    internal sealed class ScriptedImporterWorkerRequest
    {
        [JsonProperty("assetPath")]
        public string AssetPath { get; set; }

        [JsonProperty("processorId")]
        public string ProcessorId { get; set; }

        [JsonProperty("sourceHash")]
        public string SourceHash { get; set; }

        [JsonProperty("metadataHash")]
        public string MetadataHash { get; set; }

        [JsonProperty("callbackHash")]
        public string CallbackHash { get; set; }

        [JsonProperty("cancellationPath")]
        public string CancellationPath { get; set; }

        [JsonProperty("capabilities")]
        public ScriptedImporterWorkerCapabilityGrant[] Capabilities { get; set; } = Array.Empty<ScriptedImporterWorkerCapabilityGrant>();
    }

    /// <summary>One logical read requested by a worker without an existing filesystem capability.</summary>
    internal sealed class ScriptedImporterWorkerCapabilityRequest
    {
        [JsonProperty("kind")]
        public string Kind { get; set; }

        [JsonProperty("sourceGuid")]
        public Guid SourceGuid { get; set; }

        [JsonProperty("sourcePath")]
        public string SourcePath { get; set; }

        [JsonProperty("outputKind")]
        public string OutputKind { get; set; }
    }

    /// <summary>Coordinator-validated immutable file capability granted to a restarted worker.</summary>
    internal sealed class ScriptedImporterWorkerCapabilityGrant
    {
        [JsonProperty("kind")]
        public string Kind { get; set; }

        [JsonProperty("sourceGuid")]
        public Guid SourceGuid { get; set; }

        [JsonProperty("sourcePath")]
        public string SourcePath { get; set; }

        [JsonProperty("outputKind")]
        public string OutputKind { get; set; }

        [JsonProperty("readPath")]
        public string ReadPath { get; set; }

        [JsonProperty("contentHash")]
        public string ContentHash { get; set; }

        [JsonProperty("artifactKey")]
        public string ArtifactKey { get; set; }

        [JsonIgnore]
        internal string OriginalSourcePath { get; set; }
    }

    /// <summary>Stops an attempt so its coordinator can validate and explicitly grant one immutable read.</summary>
    internal sealed class ScriptedImporterWorkerCapabilityException : Exception
    {
        internal ScriptedImporterWorkerCapabilityException(ScriptedImporterWorkerCapabilityRequest request)
            : base("The scripted importer requires an additional coordinator-validated read capability.")
        {
            Request = request;
        }

        internal ScriptedImporterWorkerCapabilityRequest Request { get; }
    }

    /// <summary>Starts and supervises isolated scripted-importer worker editor processes.</summary>
    internal static class ScriptedImporterWorkerCoordinator
    {
        private const int WorkerTimeoutMilliseconds = 300000;
        private const int MaximumCapabilityRestarts = 128;

        internal static JObject Run(string canonicalPath, string processorId, string callbackHash, bool verifyDeterminism)
        {
            var first = RunOnce(canonicalPath, processorId, callbackHash);
            if (!verifyDeterminism)
                return first;
            var second = RunOnce(canonicalPath, processorId, callbackHash);
            var firstCanonical = Canonicalize(NormalizeTransientObjectIds(first)).ToString(Formatting.None);
            var secondCanonical = Canonicalize(NormalizeTransientObjectIds(second)).ToString(Formatting.None);
            if (string.Equals(firstCanonical, secondCanonical, StringComparison.Ordinal))
                return first;
            var report = QuarantineMismatch(processorId, first, second);
            throw new InvalidDataException($"Scripted importer '{processorId}' produced non-deterministic outputs or dependencies. Comparison report: {report}");
        }

        private static JObject RunOnce(string canonicalPath, string processorId, string callbackHash)
        {
            if (AssetDatabase.IsAssetImportWorkerProcess())
                throw new InvalidOperationException("An importer worker cannot start another importer worker.");

            var physicalPath = AssetDatabase.ResolvePhysicalPathInternal(canonicalPath);
            var metadataPath = physicalPath + ".meta";
            if (!File.Exists(physicalPath) || !File.Exists(metadataPath))
                throw new FileNotFoundException("Scripted import requires a source and adjacent metadata.", physicalPath);

            var requestId = Guid.NewGuid().ToString("N");
            var workerRoot = Path.Combine(Globals.ProjectLibraryFolder, "Temp", "ScriptedImporterWorkers", requestId);
            var capabilityRoot = Path.Combine(Globals.ProjectLibraryFolder, "Temp", "ScriptedImporterCapabilities", requestId);
            var requestPath = Path.Combine(workerRoot, "request.json");
            var resultPath = Path.Combine(workerRoot, "result.json");
            var eventPath = Path.Combine(workerRoot, "events.jsonl");
            var cancellationPath = Path.Combine(workerRoot, "cancel.requested");
            Directory.CreateDirectory(workerRoot);
            Directory.CreateDirectory(capabilityRoot);

            var sourceHash = HashFile(physicalPath);
            var metadataHash = HashFile(metadataPath);
            var capabilities = new List<ScriptedImporterWorkerCapabilityGrant>();
            var workerRequest = new ScriptedImporterWorkerRequest
            {
                AssetPath = canonicalPath,
                ProcessorId = processorId,
                SourceHash = sourceHash,
                MetadataHash = metadataHash,
                CallbackHash = callbackHash,
                CancellationPath = cancellationPath,
            };
            void RequestCancellation()
            {
                try
                {
                    File.WriteAllText(cancellationPath, "coordinator");
                }
                catch (IOException)
                {
                }
            }
            Engine.RequestingExit += RequestCancellation;

            try
            {
                var executable = Environment.ProcessPath;
                if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable))
                    throw new InvalidOperationException("Cannot locate the current editor executable for importer isolation.");
                for (var attempt = 0; attempt <= MaximumCapabilityRestarts; attempt++)
                {
                    workerRequest.Capabilities = capabilities.OrderBy(GetCapabilityKey, StringComparer.Ordinal).ToArray();
                    var request = new
                    {
                        schemaVersion = 1,
                        operation = "scriptedImporterWorker",
                        requestId,
                        projectPath = Globals.ProjectFolder,
                        eventPath,
                        resultPath,
                        scriptedImporterWorker = workerRequest,
                    };
                    File.WriteAllText(requestPath, JsonConvert.SerializeObject(request, Formatting.Indented));
                    if (File.Exists(resultPath))
                        File.Delete(resultPath);

                    var start = CreateStartInfo(executable, workerRoot, requestPath, attempt);
                    var readablePaths = GetWorkerReadablePaths(physicalPath, metadataPath, capabilityRoot);
                    using var process = ScriptedImporterWorkerSandbox.Start(start, workerRoot, readablePaths);
                    var timedOut = !process.WaitForExit(WorkerTimeoutMilliseconds);
                    if (timedOut)
                    {
                        RequestCancellation();
                        if (!process.WaitForExit(5000))
                        {
                            process.Kill();
                            process.WaitForExit();
                            throw new TimeoutException($"Scripted importer worker exceeded {WorkerTimeoutMilliseconds / 1000} seconds, ignored cooperative cancellation, and was terminated.");
                        }
                    }
                    if (!File.Exists(resultPath))
                        throw new InvalidDataException($"Scripted importer worker exited with code {process.ExitCode} without a result.");

                    var result = JObject.Parse(File.ReadAllText(resultPath));
                    if ((bool?)result["cancelled"] ?? false)
                        throw new OperationCanceledException("Scripted importer worker acknowledged cancellation; no output was published.");
                    if (timedOut)
                        throw new OperationCanceledException("Scripted importer completed after its deadline; staged output was discarded.");
                    var capabilityRequest = result["capabilityRequest"]?.ToObject<ScriptedImporterWorkerCapabilityRequest>();
                    if (capabilityRequest != null)
                    {
                        if (attempt == MaximumCapabilityRestarts)
                            throw new InvalidOperationException($"Scripted importer exceeded {MaximumCapabilityRestarts} dynamic dependency capabilities.");
                        var grant = ValidateAndStageCapability(capabilityRequest, capabilityRoot);
                        var key = GetCapabilityKey(grant);
                        if (capabilities.Any(x => string.Equals(GetCapabilityKey(x), key, StringComparison.Ordinal)))
                            throw new InvalidOperationException($"Scripted importer repeatedly requested already granted capability '{key}'.");
                        capabilities.Add(grant);
                        continue;
                    }
                    if (!(bool?)result["success"] ?? false)
                        throw new InvalidOperationException((string)result["errors"]?[0]?["message"] ?? $"Scripted importer worker failed with exit code {process.ExitCode}.");
                    var importResult = result["importResult"] as JObject ?? throw new InvalidDataException("Scripted importer worker returned no staged import result.");
                    ValidateInputsUnchanged(physicalPath, sourceHash, metadataPath, metadataHash, capabilities);
                    return importResult;
                }
                throw new InvalidOperationException("Scripted importer capability negotiation did not converge.");
            }
            finally
            {
                Engine.RequestingExit -= RequestCancellation;
                if (Directory.Exists(workerRoot))
                    Directory.Delete(workerRoot, true);
                if (Directory.Exists(capabilityRoot))
                    Directory.Delete(capabilityRoot, true);
            }
        }

        private static ProcessStartInfo CreateStartInfo(string executable, string workerRoot, string requestPath, int attempt)
        {
            var start = new ProcessStartInfo(executable)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = workerRoot,
            };
            start.ArgumentList.Add("-project");
            start.ArgumentList.Add(Globals.ProjectFolder);
            start.ArgumentList.Add("-headless");
            start.ArgumentList.Add("-skipCompile");
            start.ArgumentList.Add("-log");
            start.ArgumentList.Add("-logfile=" + Path.Combine(workerRoot, $"worker-{attempt:D3}.log"));
            start.ArgumentList.Add("-assetImportWorker");
            start.ArgumentList.Add("-cliRequest");
            start.ArgumentList.Add(requestPath);
            start.Environment["TZ"] = "UTC";
            start.Environment["TEMP"] = workerRoot;
            start.Environment["TMP"] = workerRoot;
            return start;
        }

        private static HashSet<string> GetWorkerReadablePaths(string physicalPath, string metadataPath, string capabilityRoot)
        {
            var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                physicalPath,
                metadataPath,
                capabilityRoot,
                Globals.BinariesFolder,
                Globals.EngineContentFolder,
                Path.Combine(Globals.ProjectFolder, "Binaries"),
                Path.Combine(Globals.ProjectLibraryFolder, "AssetDatabase"),
                Path.Combine(Globals.StartupFolder, "Flax.flaxproj"),
            };
            foreach (var projectFile in Directory.EnumerateFiles(Globals.ProjectFolder, "*.flaxproj", SearchOption.TopDirectoryOnly))
                result.Add(projectFile);
            foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (!assembly.IsDynamic && !string.IsNullOrWhiteSpace(assembly.Location) && File.Exists(assembly.Location))
                    result.Add(assembly.Location);
            }
            return result;
        }

        private static ScriptedImporterWorkerCapabilityGrant ValidateAndStageCapability(
            ScriptedImporterWorkerCapabilityRequest request, string capabilityRoot)
        {
            if (string.Equals(request.Kind, "source", StringComparison.Ordinal))
                return ValidateAndStageSource(request, capabilityRoot);
            if (string.Equals(request.Kind, "artifact", StringComparison.Ordinal))
                return ValidateAndStageArtifact(request, capabilityRoot);
            throw new InvalidOperationException($"Scripted importer requested unsupported capability kind '{request.Kind}'.");
        }

        private static ScriptedImporterWorkerCapabilityGrant ValidateAndStageSource(
            ScriptedImporterWorkerCapabilityRequest request, string capabilityRoot)
        {
            AssetDatabaseRecordInfo? record = null;
            if (request.SourceGuid != Guid.Empty)
            {
                var path = AssetDatabase.GUIDToAssetPath(request.SourceGuid.ToString("N"));
                if (!string.IsNullOrEmpty(path))
                    record = AssetDatabase.GetMainRecord(path);
            }
            else if (!string.IsNullOrWhiteSpace(request.SourcePath) && !Path.IsPathRooted(request.SourcePath))
            {
                record = AssetDatabase.GetMainRecord(request.SourcePath.Replace('\\', '/'));
            }
            if (!record.HasValue || !record.Value.IsMain || record.Value.SourceAssetID == Guid.Empty)
                throw new InvalidOperationException("The worker requested a source capability that is not a registered canonical source.");
            var originalPath = Path.GetFullPath(record.Value.SourcePath);
            if (!File.Exists(originalPath))
                throw new FileNotFoundException("The requested registered source dependency is missing.", originalPath);
            var bytes = File.ReadAllBytes(originalPath);
            var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            var snapshotPath = Path.Combine(capabilityRoot, hash + ".source");
            if (!File.Exists(snapshotPath))
                File.WriteAllBytes(snapshotPath, bytes);
            return new ScriptedImporterWorkerCapabilityGrant
            {
                Kind = "source",
                SourceGuid = record.Value.SourceAssetID,
                SourcePath = record.Value.CanonicalPath.Replace('\\', '/'),
                ReadPath = snapshotPath,
                ContentHash = hash,
                OriginalSourcePath = originalPath,
            };
        }

        private static ScriptedImporterWorkerCapabilityGrant ValidateAndStageArtifact(
            ScriptedImporterWorkerCapabilityRequest request, string capabilityRoot)
        {
            if (request.SourceGuid == Guid.Empty || string.IsNullOrWhiteSpace(request.OutputKind) ||
                Path.IsPathRooted(request.OutputKind) || request.OutputKind.Contains(".."))
                throw new InvalidOperationException("The worker requested an invalid artifact capability.");
            var sourcePath = AssetDatabase.GUIDToAssetPath(request.SourceGuid.ToString("N"));
            AssetDatabaseRecordInfo? record = string.IsNullOrEmpty(sourcePath) ? null : AssetDatabase.GetMainRecord(sourcePath);
            if (!record.HasValue || record.Value.SourceAssetID == Guid.Empty)
                throw new InvalidOperationException("The worker requested an artifact capability for an unregistered source.");
            var json = ScriptedImporterFacade.ReadArtifactOutputForCoordinator(record.Value.SourceAssetID, request.OutputKind);
            if (string.IsNullOrEmpty(json))
                throw new InvalidOperationException(ScriptedImporterFacade.GetLastError());
            var envelope = JObject.Parse(json);
            var artifactKey = (string)envelope["artifactKey"];
            var contentHash = (string)envelope["contentHash"];
            var bytes = Convert.FromBase64String((string)envelope["data"] ?? string.Empty);
            var actualHash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            if (artifactKey?.Length != 64 || contentHash?.Length != 64 ||
                !string.Equals(contentHash, actualHash, StringComparison.Ordinal))
                throw new InvalidDataException("The coordinator rejected an invalid immutable artifact response.");
            var snapshotPath = Path.Combine(capabilityRoot, contentHash + ".artifact");
            if (!File.Exists(snapshotPath))
                File.WriteAllBytes(snapshotPath, bytes);
            return new ScriptedImporterWorkerCapabilityGrant
            {
                Kind = "artifact",
                SourceGuid = record.Value.SourceAssetID,
                SourcePath = record.Value.CanonicalPath.Replace('\\', '/'),
                OutputKind = request.OutputKind,
                ReadPath = snapshotPath,
                ContentHash = contentHash,
                ArtifactKey = artifactKey,
            };
        }

        private static void ValidateInputsUnchanged(string physicalPath, string sourceHash, string metadataPath,
            string metadataHash, IEnumerable<ScriptedImporterWorkerCapabilityGrant> capabilities)
        {
            if (!string.Equals(sourceHash, HashFile(physicalPath), StringComparison.Ordinal) ||
                !string.Equals(metadataHash, HashFile(metadataPath), StringComparison.Ordinal))
                throw new InvalidOperationException("Source or importer metadata changed while the isolated importer was running; publication was discarded.");
            foreach (var grant in capabilities.Where(x => x.Kind == "source"))
            {
                if (string.IsNullOrEmpty(grant.OriginalSourcePath) || !File.Exists(grant.OriginalSourcePath) ||
                    !string.Equals(grant.ContentHash, HashFile(grant.OriginalSourcePath), StringComparison.Ordinal))
                    throw new InvalidOperationException($"Source dependency '{grant.SourcePath}' changed during capability negotiation; publication was discarded.");
            }
        }

        private static string GetCapabilityKey(ScriptedImporterWorkerCapabilityGrant grant)
        {
            return grant.Kind == "artifact"
                ? $"artifact:{grant.SourceGuid:N}/{grant.OutputKind}"
                : $"source:{grant.SourceGuid:N}/{grant.SourcePath}";
        }

        internal static string HashFile(string path)
        {
            return Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
        }

        private static JToken Canonicalize(JToken value)
        {
            if (value is JObject obj)
                return new JObject(obj.Properties().OrderBy(x => x.Name, StringComparer.Ordinal)
                    .Select(x => new JProperty(x.Name, Canonicalize(x.Value))));
            if (value is JArray array)
                return new JArray(array.Select(Canonicalize));
            return value.DeepClone();
        }

        private static JObject NormalizeTransientObjectIds(JObject result)
        {
            var normalized = (JObject)result.DeepClone();
            foreach (var obj in normalized["objects"]?.OfType<JObject>() ?? Enumerable.Empty<JObject>())
            {
                if (!Guid.TryParseExact((string)obj["transientId"], "N", out var transientId))
                    continue;
                obj.Remove("transientId");
                var data = Convert.FromBase64String((string)obj["data"] ?? string.Empty);
                if (string.Equals((string)obj["format"], "json", StringComparison.Ordinal))
                {
                    var json = JObject.Parse(Encoding.UTF8.GetString(data));
                    json["ID"] = "00000000000000000000000000000000";
                    data = Encoding.UTF8.GetBytes(Canonicalize(json).ToString(Formatting.None));
                }
                else
                {
                    ReplaceFirst(data, transientId.ToByteArray(), new byte[16]);
                }
                obj["data"] = Convert.ToBase64String(data);
            }
            return normalized;
        }

        private static void ReplaceFirst(byte[] data, byte[] search, byte[] replacement)
        {
            for (var i = 0; i <= data.Length - search.Length; i++)
            {
                var matches = true;
                for (var j = 0; j < search.Length; j++)
                    matches &= data[i + j] == search[j];
                if (!matches)
                    continue;
                Buffer.BlockCopy(replacement, 0, data, i, replacement.Length);
                return;
            }
        }

        private static string QuarantineMismatch(string processorId, JObject first, JObject second)
        {
            var providerHash = (string)first["providerHash"];
            if (string.IsNullOrWhiteSpace(providerHash))
                providerHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(processorId))).ToLowerInvariant();
            var directory = Path.Combine(Globals.ProjectLibraryFolder, "Artifacts", "Quarantine", "ManagedImporters");
            Directory.CreateDirectory(directory);
            var path = Path.Combine(directory, providerHash + ".json");
            File.WriteAllText(path, new JObject
            {
                ["processorId"] = processorId,
                ["providerHash"] = providerHash,
                ["deterministic"] = false,
                ["first"] = Canonicalize(first),
                ["second"] = Canonicalize(second),
            }.ToString(Formatting.Indented));
            return path;
        }
    }
}
