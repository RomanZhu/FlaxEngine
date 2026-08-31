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
    }

    /// <summary>Starts and supervises isolated scripted-importer worker editor processes.</summary>
    internal static class ScriptedImporterWorkerCoordinator
    {
        private const int WorkerTimeoutMilliseconds = 300000;

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
            var requestPath = Path.Combine(workerRoot, "request.json");
            var resultPath = Path.Combine(workerRoot, "result.json");
            var eventPath = Path.Combine(workerRoot, "events.jsonl");
            var cancellationPath = Path.Combine(workerRoot, "cancel.requested");
            Directory.CreateDirectory(workerRoot);

            var sourceHash = HashFile(physicalPath);
            var metadataHash = HashFile(metadataPath);
            var request = new
            {
                schemaVersion = 1,
                operation = "scriptedImporterWorker",
                requestId,
                projectPath = Globals.ProjectFolder,
                eventPath,
                resultPath,
                scriptedImporterWorker = new ScriptedImporterWorkerRequest
                {
                    AssetPath = canonicalPath,
                    ProcessorId = processorId,
                    SourceHash = sourceHash,
                    MetadataHash = metadataHash,
                    CallbackHash = callbackHash,
                    CancellationPath = cancellationPath,
                },
            };
            File.WriteAllText(requestPath, JsonConvert.SerializeObject(request, Formatting.Indented));
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
                start.ArgumentList.Add("-logfile=" + Path.Combine(workerRoot, "worker.log"));
                start.ArgumentList.Add("-assetImportWorker");
                start.ArgumentList.Add("-cliRequest");
                start.ArgumentList.Add(requestPath);
                start.Environment["TZ"] = "UTC";
                start.Environment["TEMP"] = workerRoot;
                start.Environment["TMP"] = workerRoot;
                var readablePaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
                {
                    physicalPath,
                    metadataPath,
                    Globals.BinariesFolder,
                    Globals.EngineContentFolder,
                    Path.Combine(Globals.ProjectFolder, "Binaries"),
                    Path.Combine(Globals.ProjectLibraryFolder, "AssetDatabase"),
                    Path.Combine(Globals.StartupFolder, "Flax.flaxproj"),
                };
                foreach (var projectFile in Directory.EnumerateFiles(Globals.ProjectFolder, "*.flaxproj", SearchOption.TopDirectoryOnly))
                    readablePaths.Add(projectFile);
                foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (!assembly.IsDynamic && !string.IsNullOrWhiteSpace(assembly.Location) && File.Exists(assembly.Location))
                        readablePaths.Add(assembly.Location);
                }
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
                if (!(bool?)result["success"] ?? false)
                    throw new InvalidOperationException((string)result["errors"]?[0]?["message"] ?? $"Scripted importer worker failed with exit code {process.ExitCode}.");
                if (timedOut)
                    throw new OperationCanceledException("Scripted importer completed after its deadline; staged output was discarded.");
                var importResult = result["importResult"] as JObject ?? throw new InvalidDataException("Scripted importer worker returned no staged import result.");
                if (!string.Equals(sourceHash, HashFile(physicalPath), StringComparison.Ordinal) ||
                    !string.Equals(metadataHash, HashFile(metadataPath), StringComparison.Ordinal))
                    throw new InvalidOperationException("Source or importer metadata changed while the isolated importer was running; publication was discarded.");
                return importResult;
            }
            finally
            {
                Engine.RequestingExit -= RequestCancellation;
                if (Directory.Exists(workerRoot))
                    Directory.Delete(workerRoot, true);
            }
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
