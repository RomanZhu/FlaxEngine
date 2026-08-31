// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
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
    }

    /// <summary>Starts and supervises isolated scripted-importer worker editor processes.</summary>
    internal static class ScriptedImporterWorkerCoordinator
    {
        private const int WorkerTimeoutMilliseconds = 300000;

        internal static JObject Run(string canonicalPath, string processorId, string callbackHash)
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
                },
            };
            File.WriteAllText(requestPath, JsonConvert.SerializeObject(request, Formatting.Indented));

            try
            {
                var executable = Environment.ProcessPath;
                if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable))
                    throw new InvalidOperationException("Cannot locate the current editor executable for importer isolation.");
                var start = new ProcessStartInfo(executable)
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WorkingDirectory = Globals.ProjectFolder,
                };
                start.ArgumentList.Add("-project");
                start.ArgumentList.Add(Globals.ProjectFolder);
                start.ArgumentList.Add("-headless");
                start.ArgumentList.Add("-skipCompile");
                start.ArgumentList.Add("-assetImportWorker");
                start.ArgumentList.Add("-cliRequest");
                start.ArgumentList.Add(requestPath);
                using var process = Process.Start(start) ?? throw new InvalidOperationException("Failed to start the scripted importer worker process.");
                if (!process.WaitForExit(WorkerTimeoutMilliseconds))
                {
                    process.Kill(true);
                    process.WaitForExit();
                    throw new TimeoutException($"Scripted importer worker exceeded {WorkerTimeoutMilliseconds / 1000} seconds and was terminated.");
                }
                if (!File.Exists(resultPath))
                    throw new InvalidDataException($"Scripted importer worker exited with code {process.ExitCode} without a result.");

                var result = JObject.Parse(File.ReadAllText(resultPath));
                if (!(bool?)result["success"] ?? false)
                    throw new InvalidOperationException((string)result["errors"]?[0]?["message"] ?? $"Scripted importer worker failed with exit code {process.ExitCode}.");
                var importResult = result["importResult"] as JObject ?? throw new InvalidDataException("Scripted importer worker returned no staged import result.");
                if (!string.Equals(sourceHash, HashFile(physicalPath), StringComparison.Ordinal) ||
                    !string.Equals(metadataHash, HashFile(metadataPath), StringComparison.Ordinal))
                    throw new InvalidOperationException("Source or importer metadata changed while the isolated importer was running; publication was discarded.");
                return importResult;
            }
            finally
            {
                if (Directory.Exists(workerRoot))
                    Directory.Delete(workerRoot, true);
            }
        }

        internal static string HashFile(string path)
        {
            return Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
        }
    }
}
