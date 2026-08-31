// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.Content.Settings;
using FlaxEngine;
using FlaxEngine.Utilities;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    internal sealed class CliRequest
    {
        [JsonProperty("schemaVersion")]
        public int SchemaVersion { get; set; }

        [JsonProperty("operation")]
        public string Operation { get; set; }

        [JsonProperty("requestId")]
        public string RequestId { get; set; }

        [JsonProperty("projectPath")]
        public string ProjectPath { get; set; }

        [JsonProperty("preset")]
        public string Preset { get; set; }

        [JsonProperty("target")]
        public string Target { get; set; }

        [JsonProperty("outputPath")]
        public string OutputPath { get; set; }

        [JsonProperty("customDefines")]
        public string[] CustomDefines { get; set; }

        [JsonProperty("options")]
        public CliBuildOptions Options { get; set; }

        [JsonProperty("asset")]
        public CliAssetOptions Asset { get; set; }

        [JsonProperty("command")]
        public CliCommandOptions Command { get; set; }

        [JsonProperty("scriptedImporterWorker")]
        public ScriptedImporterWorkerRequest ScriptedImporterWorker { get; set; }

        [JsonProperty("eventPath")]
        public string EventPath { get; set; }

        [JsonProperty("resultPath")]
        public string ResultPath { get; set; }
    }

    internal sealed class CliBuildOptions
    {
        [JsonProperty("clean")]
        public bool Clean { get; set; }

        [JsonProperty("runAfterBuild")]
        public bool RunAfterBuild { get; set; }
    }

    internal sealed partial class CliRequestService
    {
        private readonly object _writeLocker = new object();
        private CliRequest _request;
        private string _outputPath;
        private bool _completed;
        private bool _buildFailed;

        public void Execute(string requestPath)
        {
            try
            {
                if (string.IsNullOrWhiteSpace(requestPath) || !Path.IsPathRooted(requestPath))
                    throw new InvalidOperationException("The CLI request path must be absolute.");
                _request = JsonConvert.DeserializeObject<CliRequest>(File.ReadAllText(requestPath));
                if (_request == null)
                    throw new InvalidOperationException("The CLI request file is empty.");
                if (_request.SchemaVersion != 1)
                    throw new InvalidOperationException($"Unsupported CLI request schema {_request.SchemaVersion}.");
                if (string.IsNullOrWhiteSpace(_request.RequestId))
                    throw new InvalidOperationException("The CLI request ID is missing.");
                if (string.IsNullOrWhiteSpace(_request.ResultPath) || !Path.IsPathRooted(_request.ResultPath))
                    throw new InvalidOperationException("The CLI result path must be absolute.");
                if (!string.IsNullOrWhiteSpace(_request.EventPath) && !Path.IsPathRooted(_request.EventPath))
                    throw new InvalidOperationException("The CLI event path must be absolute.");
                ValidateProjectPath();

                switch (_request.Operation)
                {
                case "build":
                    ExecuteBuild();
                    break;
                case "asset":
                    ExecuteAsset();
                    break;
                case "command":
                    ExecuteCommand();
                    break;
                case "scriptedImporterWorker":
                    ExecuteScriptedImporterWorker();
                    break;
                default:
                    throw new InvalidOperationException($"Unsupported CLI operation '{_request.Operation}'.");
                }
            }
            catch (Exception ex)
            {
                Fail(ex);
            }
        }

        private void ExecuteBuild()
        {
                var settings = GameSettings.Load<BuildSettings>();
                var preset = settings.GetPreset(_request.Preset);
                if (preset == null)
                    throw new InvalidOperationException($"Build preset '{_request.Preset}' does not exist.");
                var target = preset.GetTarget(_request.Target);
                if (target == null)
                    throw new InvalidOperationException($"Build target '{_request.Target}' does not exist in preset '{_request.Preset}'.");
                target = target.DeepClone();
                if (!string.IsNullOrWhiteSpace(_request.OutputPath))
                    target.Output = _request.OutputPath;
                if (_request.CustomDefines != null)
                    target.CustomDefines = _request.CustomDefines;
                _outputPath = Path.GetFullPath(Path.IsPathRooted(target.Output) ? target.Output : Path.Combine(Globals.ProjectFolder, target.Output));

                GameCooker.Event += OnBuildEvent;
                GameCooker.Progress += OnBuildProgress;
                TryWriteEvent(new { type = "started", requestId = _request.RequestId, operation = _request.Operation });
                TryWriteEvent(new { type = "phase", requestId = _request.RequestId, name = "Queued" });

                var options = _request.Options != null && _request.Options.RunAfterBuild ? BuildOptions.AutoRun : BuildOptions.None;
                Editor.Instance.Windows.GameCookerWin.Build(preset, target, options);
                Editor.Instance.Windows.GameCookerWin.ExitOnBuildQueueEnd(Complete);
        }

        private void ExecuteScriptedImporterWorker()
        {
            if (!AssetDatabase.IsAssetImportWorkerProcess())
                throw new InvalidOperationException("Scripted importer worker requests require isolated worker mode.");
            var worker = _request.ScriptedImporterWorker ?? throw new InvalidOperationException("The scripted importer worker payload is missing.");
            var physicalPath = AssetDatabase.ResolvePhysicalPathInternal(worker.AssetPath);
            var metadataPath = physicalPath + ".meta";
            if (!string.Equals(worker.SourceHash, ScriptedImporterWorkerCoordinator.HashFile(physicalPath), StringComparison.Ordinal) ||
                !string.Equals(worker.MetadataHash, ScriptedImporterWorkerCoordinator.HashFile(metadataPath), StringComparison.Ordinal))
                throw new InvalidOperationException("The worker input snapshot no longer matches the coordinator request.");

            JObject importResult;
            try
            {
                importResult = ScriptedImporterRegistry.ExecuteWorker(worker.AssetPath, worker.ProcessorId, worker.CallbackHash,
                    () => !string.IsNullOrEmpty(worker.CancellationPath) && File.Exists(worker.CancellationPath), worker.Capabilities);
            }
            catch (ScriptedImporterWorkerCapabilityException ex)
            {
                WriteResult(new
                {
                    schemaVersion = 1,
                    requestId = _request.RequestId,
                    success = false,
                    cancelled = false,
                    exitCode = 4,
                    capabilityRequest = ex.Request,
                    errors = Array.Empty<object>(),
                });
                TryWriteEvent(new { type = "capabilityRequest", requestId = _request.RequestId, capability = ex.Request });
                Engine.RequestExit(4);
                return;
            }
            catch (OperationCanceledException ex)
            {
                WriteResult(new
                {
                    schemaVersion = 1,
                    requestId = _request.RequestId,
                    success = false,
                    cancelled = true,
                    exitCode = 3,
                    errors = new[] { new { code = "FLX-IMPORT-CANCELLED", message = ex.Message } },
                });
                TryWriteEvent(new { type = "result", requestId = _request.RequestId, success = false, cancelled = true, exitCode = 3 });
                Engine.RequestExit(3);
                return;
            }
            if (!string.Equals(worker.SourceHash, ScriptedImporterWorkerCoordinator.HashFile(physicalPath), StringComparison.Ordinal) ||
                !string.Equals(worker.MetadataHash, ScriptedImporterWorkerCoordinator.HashFile(metadataPath), StringComparison.Ordinal))
                throw new InvalidOperationException("The importer changed its source or metadata; worker output was rejected.");
            WriteResult(new
            {
                schemaVersion = 1,
                requestId = _request.RequestId,
                success = true,
                exitCode = 0,
                importResult,
                errors = Array.Empty<object>(),
            });
            TryWriteEvent(new { type = "result", requestId = _request.RequestId, success = true, exitCode = 0 });
            Engine.RequestExit(0);
        }

        private void ValidateProjectPath()
        {
            if (string.IsNullOrWhiteSpace(_request.ProjectPath))
                return;
            var expected = Path.GetFullPath(_request.ProjectPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var actual = Path.GetFullPath(Globals.ProjectFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var comparison = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!string.Equals(expected, actual, comparison))
                throw new InvalidOperationException($"CLI request project '{expected}' does not match the open project '{actual}'.");
        }

        private void OnBuildEvent(GameCooker.EventType type)
        {
            if (type == GameCooker.EventType.BuildStarted)
                TryWriteEvent(new { type = "phase", requestId = _request.RequestId, name = "GameCooker" });
            else if (type == GameCooker.EventType.BuildFailed)
                _buildFailed = true;
        }

        private void OnBuildProgress(string message, float progress)
        {
            TryWriteEvent(new { type = "progress", requestId = _request.RequestId, value = progress, message });
        }

        private void Complete(bool failed)
        {
            if (_completed)
                return;
            _completed = true;
            GameCooker.Event -= OnBuildEvent;
            GameCooker.Progress -= OnBuildProgress;
            failed |= _buildFailed || !BuildOutputHasFiles();
            if (!failed)
                TryWriteEvent(new { type = "artifact", requestId = _request.RequestId, kind = "output", path = _outputPath });
            var result = new
            {
                schemaVersion = 1,
                requestId = _request.RequestId,
                success = !failed,
                exitCode = failed ? 6 : 0,
                artifacts = failed ? Array.Empty<object>() : new[] { new { kind = "output", path = _outputPath } },
                errors = failed ? new[] { new { code = "FLX-BUILD-0006", message = "Game Cooker failed." } } : Array.Empty<object>(),
            };
            WriteResult(result);
            TryWriteEvent(new { type = "result", requestId = _request.RequestId, success = !failed, exitCode = failed ? 6 : 0 });
        }

        private bool BuildOutputHasFiles()
        {
            if (string.IsNullOrWhiteSpace(_outputPath) || !Directory.Exists(_outputPath))
                return false;
            using var files = Directory.EnumerateFiles(_outputPath, "*", SearchOption.AllDirectories).GetEnumerator();
            return files.MoveNext();
        }

        private void Fail(Exception exception)
        {
            Editor.LogError(exception.ToString());
            try
            {
                if (_request == null)
                    return;
                TryWriteEvent(new { type = "diagnostic", requestId = _request.RequestId, severity = "error", code = "FLX-REQUEST-0002", message = exception.Message });
                if (!string.IsNullOrWhiteSpace(_request.ResultPath) && Path.IsPathRooted(_request.ResultPath))
                {
                    WriteResult(new
                    {
                        schemaVersion = 1,
                        requestId = _request.RequestId,
                        success = false,
                        exitCode = 2,
                        artifacts = Array.Empty<object>(),
                        errors = new[] { new { code = "FLX-REQUEST-0002", message = exception.Message } },
                    });
                }
                TryWriteEvent(new { type = "result", requestId = _request.RequestId, success = false, exitCode = 2 });
            }
            catch (Exception writeException)
            {
                Editor.LogError(writeException.ToString());
            }
            finally
            {
                Engine.RequestExit(1);
            }
        }

        private void WriteEvent(object value)
        {
            if (string.IsNullOrWhiteSpace(_request?.EventPath))
                return;
            lock (_writeLocker)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(_request.EventPath));
                File.AppendAllText(_request.EventPath, JsonConvert.SerializeObject(value) + Environment.NewLine);
            }
        }

        private void TryWriteEvent(object value)
        {
            try
            {
                WriteEvent(value);
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                _request.EventPath = null;
            }
        }

        private void WriteResult(object value)
        {
            var directory = Path.GetDirectoryName(_request.ResultPath);
            Directory.CreateDirectory(directory);
            var temporaryPath = _request.ResultPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllText(temporaryPath, JsonConvert.SerializeObject(value, Formatting.Indented));
            File.Move(temporaryPath, _request.ResultPath, true);
        }
    }

    public sealed partial class Editor
    {
        internal void CliRequestCommand(string requestPath)
        {
            new CliRequestService().Execute(requestPath);
        }
    }
}
