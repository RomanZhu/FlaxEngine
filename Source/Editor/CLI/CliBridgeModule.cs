// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using FlaxEditor.Modules;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Process = System.Diagnostics.Process;
using Stopwatch = System.Diagnostics.Stopwatch;

namespace FlaxEditor
{
    internal sealed class CliBridgeRequest
    {
        [JsonProperty("schemaVersion")]
        public int SchemaVersion { get; set; }

        [JsonProperty("requestId")]
        public string RequestId { get; set; }

        [JsonProperty("token")]
        public string Token { get; set; }

        [JsonProperty("action")]
        public string Action { get; set; }

        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("arguments")]
        public JObject Arguments { get; set; }

        [JsonProperty("confirm")]
        public bool Confirm { get; set; }

        [JsonProperty("timeoutSeconds")]
        public double TimeoutSeconds { get; set; } = 30.0;
    }

    internal sealed class CliBridgeResponse
    {
        [JsonProperty("schemaVersion")]
        public int SchemaVersion { get; set; } = 1;

        [JsonProperty("requestId")]
        public string RequestId { get; set; }

        [JsonProperty("success")]
        public bool Success { get; set; }

        [JsonProperty("data")]
        public object Data { get; set; }

        [JsonProperty("errors")]
        public CliCommandMessage[] Errors { get; set; } = Array.Empty<CliCommandMessage>();

        [JsonProperty("warnings")]
        public CliCommandMessage[] Warnings { get; set; } = Array.Empty<CliCommandMessage>();

        [JsonProperty("events")]
        public object[] Events { get; set; } = Array.Empty<object>();
    }

    internal sealed class CliBridgePendingRequest
    {
        public CliBridgeRequest Request;
        public CancellationToken CancellationToken;
        public TaskCompletionSource<CliBridgeResponse> Completion;
    }

    internal sealed class CliBridgeActiveCommand
    {
        public CliBridgePendingRequest Pending;
        public CliCommandInvocation Invocation;
        public List<object> Events;
        public List<CliCommandMessage> Warnings;
    }

    internal sealed class CliBridgeLogEntry
    {
        [JsonProperty("cursor")]
        public long Cursor { get; set; }

        [JsonProperty("timestampUtc")]
        public DateTime TimestampUtc { get; set; }

        [JsonProperty("level")]
        public string Level { get; set; }

        [JsonProperty("message")]
        public string Message { get; set; }

        [JsonProperty("stackTrace")]
        public string StackTrace { get; set; }
    }

    /// <summary>
    /// Hosts the authenticated local Flax CLI bridge for a running Editor instance.
    /// </summary>
    internal sealed class CliBridgeModule : EditorModule
    {
        private const int ProtocolVersion = 1;
        private const int MaxRequestCharacters = 1024 * 1024;
        private const int MaxLogEntries = 2048;
        private const int MaxPendingRequests = 64;
        private const int MaxCommandEvents = 4096;
        private const int MaxCommandWarnings = 256;
        private const double CommandUpdateBudgetMilliseconds = 3.0;
        private readonly CancellationTokenSource _shutdown = new CancellationTokenSource();
        private readonly ManualResetEventSlim _serverReady = new ManualResetEventSlim();
        private readonly ConcurrentQueue<CliBridgePendingRequest> _pending = new ConcurrentQueue<CliBridgePendingRequest>();
        private readonly object _logsLocker = new object();
        private readonly List<CliBridgeLogEntry> _logs = new List<CliBridgeLogEntry>(MaxLogEntries);
        private CliBridgeActiveCommand _activeCommand;
        private Task _serverTask;
        private Socket _unixListener;
        private string _instanceId;
        private string _token;
        private string _runtimeDirectory;
        private string _manifestPath;
        private string _tokenPath;
        private string _endpoint;
        private string _transport;
        private string _publishedState;
        private long _logCursor;
        private int _pendingCount;

        public CliBridgeModule(Editor editor)
        : base(editor)
        {
            InitOrder = 1000;
        }

        /// <inheritdoc />
        public override void OnEndInit()
        {
            if (Editor.IsHeadlessMode)
                return;

            try
            {
                _runtimeDirectory = GetRuntimeDirectory();
                Directory.CreateDirectory(_runtimeDirectory);
                RestrictUnixPath(_runtimeDirectory, true);
                _instanceId = Guid.NewGuid().ToString("N");
                _token = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));
                _manifestPath = Path.Combine(_runtimeDirectory, $"editor-{Environment.ProcessId}-{_instanceId}.instance.json");
                _tokenPath = Path.Combine(_runtimeDirectory, $"editor-{Environment.ProcessId}-{_instanceId}.token");
                File.WriteAllText(_tokenPath, _token, new UTF8Encoding(false));
                RestrictUnixPath(_tokenPath, false);

                if (Path.DirectorySeparatorChar == '\\')
                {
                    _transport = "namedPipe";
                    _endpoint = $"flax-cli-{Environment.ProcessId}-{_instanceId}";
                    _serverTask = Task.Run(RunNamedPipeServer);
                }
                else
                {
                    _transport = "unixSocket";
                    _endpoint = Path.Combine(_runtimeDirectory, $"flax-{Environment.ProcessId}-{_instanceId.Substring(0, 8)}.sock");
                    _serverTask = Task.Run(RunUnixSocketServer);
                }

                if (!_serverReady.Wait(TimeSpan.FromSeconds(2.0)))
                    throw new TimeoutException("The local IPC listener did not become ready.");
                WriteManifest();
                Debug.Logger.LogHandler.SendLog += OnLog;
                Debug.Logger.LogHandler.SendExceptionLog += OnExceptionLog;
                FlaxEditor.Editor.Log($"Flax CLI bridge listening for instance {_instanceId}.");
            }
            catch (Exception ex)
            {
                _shutdown.Cancel();
                FlaxEditor.Editor.LogWarning("Failed to start the Flax CLI bridge: " + ex.Message);
                CleanupFiles();
            }
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            if (_shutdown.IsCancellationRequested)
                return;

            var timer = Stopwatch.StartNew();
            if (_activeCommand == null && _pending.TryDequeue(out var pending))
            {
                Interlocked.Decrement(ref _pendingCount);
                if (pending.CancellationToken.IsCancellationRequested)
                {
                    pending.Completion.TrySetCanceled(pending.CancellationToken);
                }
                else
                {
                    try
                    {
                        StartRequest(pending);
                    }
                    catch (CliCommandProtocolException ex)
                    {
                        pending.Completion.TrySetResult(Failure(pending.Request?.RequestId, ex.Code, ex.Message));
                    }
                    catch (Exception ex)
                    {
                        pending.Completion.TrySetResult(Failure(pending.Request?.RequestId, "FLX-BRIDGE-0001", ex.Message, new { exception = ex.ToString() }));
                    }
                }
            }

            if (_activeCommand != null)
            {
                var remaining = TimeSpan.FromMilliseconds(CommandUpdateBudgetMilliseconds) - timer.Elapsed;
                if (remaining > TimeSpan.Zero)
                    UpdateActiveCommand(remaining);
            }

            var state = GetStateName();
            if (!string.Equals(_publishedState, state, StringComparison.Ordinal))
            {
                try
                {
                    WriteManifest();
                }
                catch (Exception ex)
                {
                    _publishedState = state;
                    FlaxEditor.Editor.LogWarning("Failed to refresh the Flax CLI instance manifest: " + ex.Message);
                }
            }
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            Debug.Logger.LogHandler.SendLog -= OnLog;
            Debug.Logger.LogHandler.SendExceptionLog -= OnExceptionLog;
            _shutdown.Cancel();
            try
            {
                _unixListener?.Dispose();
            }
            catch
            {
                // The listener is best-effort during shutdown.
            }
            while (_pending.TryDequeue(out var pending))
            {
                Interlocked.Decrement(ref _pendingCount);
                pending.Completion.TrySetCanceled();
            }
            try
            {
                _activeCommand?.Invocation.Cancel();
            }
            catch (Exception ex)
            {
                FlaxEditor.Editor.LogWarning("Failed to cancel the active CLI command during shutdown: " + ex.Message);
            }
            _activeCommand?.Pending.Completion.TrySetCanceled();
            _activeCommand = null;
            try
            {
                _serverTask?.Wait(250);
            }
            catch
            {
                // Listener shutdown is best-effort; discovery will validate stale state.
            }
            CleanupFiles();
        }

        private async Task RunNamedPipeServer()
        {
            while (!_shutdown.IsCancellationRequested)
            {
                NamedPipeServerStream pipe = null;
                try
                {
                    pipe = new NamedPipeServerStream(
                        _endpoint,
                        PipeDirection.InOut,
                        NamedPipeServerStream.MaxAllowedServerInstances,
                        PipeTransmissionMode.Byte,
                        PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                    _serverReady.Set();
                    await pipe.WaitForConnectionAsync(_shutdown.Token).ConfigureAwait(false);
                    var connectedPipe = pipe;
                    pipe = null;
                    _ = Task.Run(() => HandleConnection(connectedPipe), _shutdown.Token);
                }
                catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
                {
                    pipe?.Dispose();
                    break;
                }
                catch (Exception ex)
                {
                    pipe?.Dispose();
                    if (!_shutdown.IsCancellationRequested)
                        FlaxEditor.Editor.LogWarning("Flax CLI named-pipe listener failed: " + ex.Message);
                }
            }
        }

        private async Task RunUnixSocketServer()
        {
            try
            {
                if (File.Exists(_endpoint))
                    File.Delete(_endpoint);
                _unixListener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                _unixListener.Bind(new UnixDomainSocketEndPoint(_endpoint));
                RestrictUnixPath(_endpoint, false);
                _unixListener.Listen(16);
                _serverReady.Set();
                while (!_shutdown.IsCancellationRequested)
                {
                    var socket = await _unixListener.AcceptAsync(_shutdown.Token).ConfigureAwait(false);
                    _ = Task.Run(() => HandleConnection(new NetworkStream(socket, true)), _shutdown.Token);
                }
            }
            catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
            {
            }
            catch (ObjectDisposedException) when (_shutdown.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                if (!_shutdown.IsCancellationRequested)
                    FlaxEditor.Editor.LogWarning("Flax CLI Unix-socket listener failed: " + ex.Message);
            }
        }

        private async Task HandleConnection(Stream stream)
        {
            using (stream)
            using (var reader = new StreamReader(stream, new UTF8Encoding(false), false, 8192, true))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false), 8192, true) { AutoFlush = true })
            {
                CliBridgeRequest request = null;
                CancellationTokenSource requestCancellation = null;
                try
                {
                    var line = await ReadRequestLine(reader, _shutdown.Token).ConfigureAwait(false);
                    if (string.IsNullOrWhiteSpace(line))
                        throw new CliCommandProtocolException("FLX-BRIDGE-REQUEST-0002", "The bridge request is empty.");
                    request = JsonConvert.DeserializeObject<CliBridgeRequest>(line);
                    if (request == null || request.SchemaVersion != ProtocolVersion)
                        throw new CliCommandProtocolException("FLX-BRIDGE-PROTOCOL-0004", "The bridge request protocol is unsupported.");
                    if (!TokenEquals(request.Token, _token))
                        throw new CliCommandProtocolException("FLX-BRIDGE-AUTH-0003", "The bridge authentication token is invalid.");
                    if (string.IsNullOrWhiteSpace(request.RequestId) || string.IsNullOrWhiteSpace(request.Action))
                        throw new CliCommandProtocolException("FLX-BRIDGE-REQUEST-0002", "The bridge request ID and action are required.");

                    var timeout = TimeSpan.FromSeconds(Math.Max(1.0, Math.Min(3600.0, request.TimeoutSeconds)));
                    requestCancellation = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
                    var pending = new CliBridgePendingRequest
                    {
                        Request = request,
                        CancellationToken = requestCancellation.Token,
                        Completion = new TaskCompletionSource<CliBridgeResponse>(TaskCreationOptions.RunContinuationsAsynchronously),
                    };
                    if (Interlocked.Increment(ref _pendingCount) > MaxPendingRequests)
                    {
                        Interlocked.Decrement(ref _pendingCount);
                        throw new CliCommandProtocolException("FLX-BRIDGE-BUSY-0005", "The live Editor request queue is full. Retry the request later.");
                    }
                    _pending.Enqueue(pending);
                    var response = await pending.Completion.Task.WaitAsync(timeout, _shutdown.Token).ConfigureAwait(false);
                    await writer.WriteLineAsync(JsonConvert.SerializeObject(response, Formatting.None)).ConfigureAwait(false);
                }
                catch (CliCommandProtocolException ex)
                {
                    await writer.WriteLineAsync(JsonConvert.SerializeObject(Failure(request?.RequestId, ex.Code, ex.Message), Formatting.None)).ConfigureAwait(false);
                }
                catch (TimeoutException)
                {
                    requestCancellation?.Cancel();
                    await writer.WriteLineAsync(JsonConvert.SerializeObject(Failure(request?.RequestId, "FLX-BRIDGE-TIMEOUT-0006", "The live Editor request timed out."), Formatting.None)).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
                {
                }
                catch (Exception ex)
                {
                    try
                    {
                        await writer.WriteLineAsync(JsonConvert.SerializeObject(Failure(request?.RequestId, "FLX-BRIDGE-0001", ex.Message), Formatting.None)).ConfigureAwait(false);
                    }
                    catch
                    {
                        // The client disconnected before the failure could be returned.
                    }
                }
                finally
                {
                    requestCancellation?.Dispose();
                }
            }
        }

        private static async Task<string> ReadRequestLine(StreamReader reader, CancellationToken cancellationToken)
        {
            var result = new StringBuilder();
            var buffer = new char[4096];
            while (true)
            {
                var read = await reader.ReadAsync(buffer.AsMemory(), cancellationToken).ConfigureAwait(false);
                if (read == 0)
                    return result.Length == 0 ? null : result.ToString();
                for (var i = 0; i < read; i++)
                {
                    if (buffer[i] == '\n')
                    {
                        if (result.Length != 0 && result[result.Length - 1] == '\r')
                            result.Length--;
                        return result.ToString();
                    }
                    result.Append(buffer[i]);
                    if (result.Length > MaxRequestCharacters)
                        throw new CliCommandProtocolException("FLX-BRIDGE-REQUEST-0002", "The bridge request exceeds the one-megabyte limit.");
                }
            }
        }

        private CliBridgeResponse ExecuteRequest(CliBridgeRequest request, CancellationToken cancellationToken)
        {
            try
            {
                switch (request.Action)
                {
                case "status":
                case "editor.status":
                    return Success(request.RequestId, GetStatus());
                case "commands.list":
                    return Success(request.RequestId, CliCommandRegistry.Discover().Select(CliCommandRegistry.Describe).ToArray());
                case "commands.info":
                {
                    var commands = CliCommandRegistry.Discover();
                    return Success(request.RequestId, CliCommandRegistry.Describe(CliCommandRegistry.Require(commands, request.Name)));
                }
                case "command.invoke":
                    throw new InvalidOperationException("Command invocations must be scheduled through the cooperative bridge path.");
                case "editor.play":
                    Editor.Simulation.RequestStartPlayScenes();
                    return Success(request.RequestId, new { requested = "play", status = GetStatus() });
                case "editor.pause":
                    Editor.Simulation.RequestPausePlay();
                    return Success(request.RequestId, new { requested = "pause", status = GetStatus() });
                case "editor.resume":
                    Editor.Simulation.RequestResumePlay();
                    return Success(request.RequestId, new { requested = "resume", status = GetStatus() });
                case "editor.stop":
                    Editor.Simulation.RequestStopPlay();
                    return Success(request.RequestId, new { requested = "stop", status = GetStatus() });
                case "editor.step":
                    Editor.Simulation.RequestPlayOneFrame();
                    return Success(request.RequestId, new { requested = "step", status = GetStatus() });
                case "editor.focus":
                    Editor.Windows.MainWindow?.Focus();
                    return Success(request.RequestId, new { focused = Editor.Windows.MainWindow != null });
                case "editor.saveAll":
                    Editor.SaveAll();
                    return Success(request.RequestId, new { saved = true });
                case "editor.recompile":
                {
                    var requested = !ScriptsBuilder.IsCompiling;
                    if (requested)
                        ScriptsBuilder.CheckForCompile();
                    return Success(request.RequestId, new { requested, status = GetStatus() });
                }
                case "console":
                    return Success(request.RequestId, ReadLogs(request.Arguments));
                default:
                    throw new CliCommandProtocolException("FLX-BRIDGE-ACTION-0002", $"Unsupported live Editor action '{request.Action}'.");
                }
            }
            catch (CliCommandProtocolException ex)
            {
                return Failure(request.RequestId, ex.Code, ex.Message);
            }
            catch (Exception ex)
            {
                FlaxEditor.Editor.LogError(ex.ToString());
                return Failure(request.RequestId, "FLX-BRIDGE-0006", ex.Message, new { exception = ex.ToString() });
            }
        }

        private void StartRequest(CliBridgePendingRequest pending)
        {
            if (!string.Equals(pending.Request.Action, "command.invoke", StringComparison.Ordinal))
            {
                pending.Completion.TrySetResult(ExecuteRequest(pending.Request, pending.CancellationToken));
                return;
            }

            var events = new List<object>();
            var warnings = new List<CliCommandMessage>();
            var eventLimitReported = false;
            var context = new CliCommandContext(
                pending.Request.RequestId,
                Globals.ProjectFolder,
                pending.CancellationToken,
                (message, progress) =>
                {
                    if (events.Count < MaxCommandEvents)
                    {
                        events.Add(new { type = "progress", value = progress, message });
                    }
                    else if (!eventLimitReported)
                    {
                        eventLimitReported = true;
                        warnings.Add(new CliCommandMessage("FLX-BRIDGE-EVENTS-0005", "Additional command progress events were omitted."));
                    }
                },
                warning =>
                {
                    if (warnings.Count < MaxCommandWarnings)
                        warnings.Add(warning);
                });
            var commands = CliCommandRegistry.Discover();
            var command = CliCommandRegistry.Require(commands, pending.Request.Name);
            var invocation = CliCommandRegistry.BeginInvoke(command, pending.Request.Arguments, pending.Request.Confirm, context);
            _activeCommand = new CliBridgeActiveCommand
            {
                Pending = pending,
                Invocation = invocation,
                Events = events,
                Warnings = warnings,
            };
        }

        private void UpdateActiveCommand(TimeSpan timeBudget)
        {
            var active = _activeCommand;
            try
            {
                if (active.Pending.CancellationToken.IsCancellationRequested)
                {
                    active.Invocation.Cancel();
                    active.Pending.Completion.TrySetCanceled(active.Pending.CancellationToken);
                    _activeCommand = null;
                    return;
                }

                if (!active.Invocation.IsCompleted)
                    active.Invocation.Update(timeBudget);
                if (!active.Invocation.IsCompleted)
                    return;

                var result = active.Invocation.Result;
                active.Warnings.AddRange(result.Warnings);
                active.Pending.Completion.TrySetResult(new CliBridgeResponse
                {
                    RequestId = active.Pending.Request.RequestId,
                    Success = result.Succeeded,
                    Data = result.Data,
                    Errors = result.Errors.ToArray(),
                    Warnings = active.Warnings.ToArray(),
                    Events = active.Events.ToArray(),
                });
            }
            catch (OperationCanceledException) when (active.Pending.CancellationToken.IsCancellationRequested)
            {
                active.Pending.Completion.TrySetCanceled(active.Pending.CancellationToken);
            }
            catch (CliCommandProtocolException ex)
            {
                active.Pending.Completion.TrySetResult(Failure(active.Pending.Request.RequestId, ex.Code, ex.Message));
            }
            catch (Exception ex)
            {
                FlaxEditor.Editor.LogError(ex.ToString());
                active.Pending.Completion.TrySetResult(Failure(active.Pending.Request.RequestId, "FLX-BRIDGE-0006", ex.Message));
            }
            finally
            {
                if (active.Pending.Completion.Task.IsCompleted)
                    _activeCommand = null;
            }
        }

        private object GetStatus()
        {
            var playMode = Editor.StateMachine.IsPlayMode;
            return new
            {
                instanceId = _instanceId,
                kind = "editor",
                state = GetStateName(),
                projectPath = Globals.ProjectFolder,
                engineVersion = Globals.EngineVersion,
                playMode,
                paused = playMode && Editor.StateMachine.PlayingState.IsPaused,
                compiling = ScriptsBuilder.IsCompiling,
                loadedScenes = Level.ScenesCount,
                capabilities = GetCapabilities(),
            };
        }

        private string GetStateName()
        {
            if (ScriptsBuilder.IsCompiling)
                return "compiling";
            if (!Editor.StateMachine.IsPlayMode)
                return "ready";
            return Editor.StateMachine.PlayingState.IsPaused ? "paused" : "playing";
        }

        private object ReadLogs(JObject arguments)
        {
            var cursor = arguments?["cursor"]?.Value<long>() ?? 0L;
            var limit = Math.Max(1, Math.Min(1000, arguments?["limit"]?.Value<int>() ?? 200));
            var level = arguments?["level"]?.Value<string>();
            lock (_logsLocker)
            {
                var query = _logs.Where(x => x.Cursor > cursor && (string.IsNullOrWhiteSpace(level) || string.Equals(x.Level, level, StringComparison.OrdinalIgnoreCase))).ToArray();
                var entries = query.Take(limit).ToArray();
                return new
                {
                    entries,
                    nextCursor = entries.Length == 0 ? cursor : entries[entries.Length - 1].Cursor,
                    truncated = query.Length > entries.Length,
                    oldestCursor = _logs.Count == 0 ? 0L : _logs[0].Cursor,
                    latestCursor = _logCursor,
                };
            }
        }

        private void OnLog(LogType level, string message, FlaxEngine.Object context, string stackTrace)
        {
            AddLog(level.ToString(), message, stackTrace);
        }

        private void OnExceptionLog(Exception exception, FlaxEngine.Object context)
        {
            AddLog(LogType.Error.ToString(), exception?.Message, exception?.StackTrace);
        }

        private void AddLog(string level, string message, string stackTrace)
        {
            lock (_logsLocker)
            {
                _logs.Add(new CliBridgeLogEntry
                {
                    Cursor = ++_logCursor,
                    TimestampUtc = DateTime.UtcNow,
                    Level = level,
                    Message = message,
                    StackTrace = stackTrace,
                });
                if (_logs.Count > MaxLogEntries)
                    _logs.RemoveRange(0, _logs.Count - MaxLogEntries);
            }
        }

        private void WriteManifest()
        {
            var process = Process.GetCurrentProcess();
            var manifest = new
            {
                schemaVersion = 1,
                instanceId = _instanceId,
                pid = process.Id,
                processStartTimeUtc = process.StartTime.ToUniversalTime(),
                kind = "editor",
                projectPath = Path.GetFullPath(Globals.ProjectFolder),
                engineVersion = Globals.EngineVersion,
                engineNickname = Editor.GameProject?.EngineNickname,
                protocolVersion = ProtocolVersion,
                transport = _transport,
                endpoint = _endpoint,
                tokenPath = _tokenPath,
                state = GetStateName(),
                capabilities = GetCapabilities(),
            };
            var temporaryPath = _manifestPath + ".tmp";
            File.WriteAllText(temporaryPath, JsonConvert.SerializeObject(manifest, Formatting.Indented), new UTF8Encoding(false));
            File.Move(temporaryPath, _manifestPath, true);
            RestrictUnixPath(_manifestPath, false);
            _publishedState = GetStateName();
        }

        private static string[] GetCapabilities()
        {
            return new[] { "commands", "playMode", "console", "saveAll", "focus", "recompile" };
        }

        private static CliBridgeResponse Success(string requestId, object data)
        {
            return new CliBridgeResponse { RequestId = requestId, Success = true, Data = data };
        }

        private static CliBridgeResponse Failure(string requestId, string code, string message, object details = null)
        {
            return new CliBridgeResponse
            {
                RequestId = requestId,
                Success = false,
                Errors = new[] { new CliCommandMessage(code, message, details) },
            };
        }

        private static bool TokenEquals(string provided, string expected)
        {
            if (provided == null || expected == null)
                return false;
            var left = Encoding.UTF8.GetBytes(provided);
            var right = Encoding.UTF8.GetBytes(expected);
            return left.Length == right.Length && CryptographicOperations.FixedTimeEquals(left, right);
        }

        private static string GetRuntimeDirectory()
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (Path.DirectorySeparatorChar == '\\')
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Flax", "CLI", "runtime");
            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
                return Path.Combine(Environment.GetEnvironmentVariable("FLAX_CACHE_PATH") ?? Path.Combine(home, "Library", "Caches", "Flax", "CLI"), "runtime");
            return Path.Combine(Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR") ?? Path.GetTempPath(), $"flax-{Environment.UserName}");
        }

        private static void RestrictUnixPath(string path, bool directory)
        {
            if (Path.DirectorySeparatorChar == '\\')
                return;
            try
            {
                File.SetUnixFileMode(path, directory
                    ? UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute
                    : UnixFileMode.UserRead | UnixFileMode.UserWrite);
            }
            catch (PlatformNotSupportedException)
            {
                // The containing user runtime directory remains the access boundary.
            }
        }

        private void CleanupFiles()
        {
            TryDeleteFile(_manifestPath);
            TryDeleteFile(_tokenPath);
            if (_transport == "unixSocket")
                TryDeleteFile(_endpoint);
        }

        private static void TryDeleteFile(string path)
        {
            try
            {
                if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
                    File.Delete(path);
            }
            catch
            {
                // Discovery validates process ownership and can prune a stale descriptor later.
            }
        }
    }
}
