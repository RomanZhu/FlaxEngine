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
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
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
            if (_shutdown.IsCancellationRequested || string.IsNullOrEmpty(_manifestPath))
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
                    return Success(request.RequestId, CliCommandRegistry.DiscoverCommands().Select(CliCommandRegistry.Describe).ToArray());
                case "commands.info":
                {
                    var commands = CliCommandRegistry.Discover();
                    return Success(request.RequestId, CliCommandRegistry.Describe(CliCommandRegistry.RequireCommand(commands, request.Name)));
                }
                case "command.invoke":
                    throw new InvalidOperationException("Command invocations must be scheduled through the cooperative bridge path.");
                case "generators.list":
                    return Success(request.RequestId, CliCommandRegistry.DiscoverGenerators().Select(CliCommandRegistry.Describe).ToArray());
                case "generators.info":
                {
                    var generators = CliCommandRegistry.Discover();
                    return Success(request.RequestId, CliCommandRegistry.Describe(CliCommandRegistry.RequireGenerator(generators, request.Name)));
                }
                case "generator.invoke":
                    throw new InvalidOperationException("Generator invocations must be scheduled through the cooperative bridge path.");
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
                case "player.status":
                    return Success(request.RequestId, GetStatus());
                case "player.pause":
                    Editor.Simulation.RequestPausePlay();
                    return Success(request.RequestId, new { requested = "pause", status = GetStatus() });
                case "player.resume":
                    Editor.Simulation.RequestResumePlay();
                    return Success(request.RequestId, new { requested = "resume", status = GetStatus() });
                case "player.step":
                    Editor.Simulation.RequestPlayOneFrame();
                    return Success(request.RequestId, new { requested = "step", status = GetStatus() });
                case "player.quit":
                    Editor.Simulation.RequestStopPlay();
                    return Success(request.RequestId, new { requested = "quit", status = GetStatus() });
                case "runtime.input.key":
                    return Success(request.RequestId, InjectKey(request.Arguments));
                case "runtime.input.pointer":
                    return Success(request.RequestId, InjectPointer(request.Arguments));
                case "runtime.input.reset":
                    return Success(request.RequestId, ResetInput());
                case "runtime.input.inspect":
                    return Success(request.RequestId, CliInputProbe.Capture(request.Arguments));
                case "runtime.input.gamepad":
                case "runtime.input.action":
                    throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0004", "This bridge supports raw keyboard and mouse injection; gamepad/action synthesis is not exposed by the current Flax input ABI.");
                case "editor.focus":
                    Editor.Windows.MainWindow?.Focus();
                    return Success(request.RequestId, new { focused = Editor.Windows.MainWindow != null });
                case "editor.saveAll":
                    Editor.SaveAll();
                    return Success(request.RequestId, new { saved = true });
                case "editor.close":
                {
                    var save = request.Arguments?["save"]?.Value<bool>() ?? true;
                    if (save)
                    {
                        if (Level.SaveAllScenes())
                            throw new InvalidOperationException("Failed to save one or more scenes before Editor shutdown.");
                        Editor.Instance.SaveContent();
                    }
                    else
                    {
                        foreach (var scene in Level.Scenes)
                        {
                            var node = Editor.Instance.Scene.GetActorNode(scene) as SceneNode;
                            if (node != null)
                                node.IsEdited = false;
                        }
                        Editor.Instance.Undo.MarkScenesSaved();
                    }
                    Engine.RequestExit(0);
                    return Success(request.RequestId, new { requested = true, saved = save, discarded = !save });
                }
                case "editor.recompile":
                {
                    var requested = !ScriptsBuilder.IsCompiling;
                    if (requested)
                        ScriptsBuilder.CheckForCompile();
                    return Success(request.RequestId, new { requested, status = GetStatus() });
                }
                case "console":
                    return Success(request.RequestId, ReadLogs(request.Arguments));
                case "console.clear":
                    return Success(request.RequestId, ClearLogs());
                case "performance":
                    return Success(request.RequestId, GetPerformance());
                case "selection.get":
                    return Success(request.RequestId, GetSelection());
                case "selection.set":
                    return Success(request.RequestId, SetSelection(request.Arguments));
                case "selection.clear":
                    Editor.SceneEditing.Deselect(false);
                    return Success(request.RequestId, GetSelection());
                case "capture.viewport":
                {
                    var path = ResolveCapturePath(request.Arguments);
                    var viewport = Editor.Instance.Windows.EditWin?.Viewport;
                    if (viewport == null)
                        throw new InvalidOperationException("The Editor scene viewport is unavailable in headless mode.");
                    viewport.TakeScreenshot(path);
                    return Success(request.RequestId, new { requested = true, source = "viewport", path });
                }
                case "capture.game":
                {
                    var path = ResolveCapturePath(request.Arguments);
                    var game = Editor.Instance.Windows.GameWin;
                    if (game == null)
                        throw new InvalidOperationException("The Editor game viewport is unavailable.");
                    game.TakeScreenshot(path);
                    return Success(request.RequestId, new { requested = true, source = "game", path });
                }
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
            var isCommand = string.Equals(pending.Request.Action, "command.invoke", StringComparison.Ordinal);
            var isGenerator = string.Equals(pending.Request.Action, "generator.invoke", StringComparison.Ordinal);
            if (!isCommand && !isGenerator)
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
            var command = isGenerator
                ? CliCommandRegistry.RequireGenerator(commands, pending.Request.Name)
                : CliCommandRegistry.RequireCommand(commands, pending.Request.Name);
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

        private static object InjectKey(JObject arguments)
        {
            if (Input.Keyboard == null)
                throw new InvalidOperationException("The runtime keyboard device is unavailable.");
            var name = arguments?["key"]?.Value<string>();
            if (!Enum.TryParse(name, true, out KeyboardKeys key))
                throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0002", $"Unknown keyboard key '{name}'.");
            if (key is KeyboardKeys.None or KeyboardKeys.MAX)
                throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0002", "The keyboard key cannot be None or MAX.");
            var state = arguments?["state"]?.Value<string>()?.ToLowerInvariant() ?? "press";
            if (state is not ("down" or "up" or "press"))
                throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0002", "Keyboard input state must be down, up, or press.");
            if (state is "down" or "press") Input.Keyboard.OnKeyDown(key);
            if (state is "up" or "press") Input.Keyboard.OnKeyUp(key);
            return new { injected = true, device = "keyboard", key = key.ToString(), state };
        }

        private static object ResetInput()
        {
            var releasedKeys = 0;
            var releasedButtons = 0;
            if (Input.Keyboard != null)
            {
                foreach (var key in Enum.GetValues<KeyboardKeys>())
                {
                    if (key is KeyboardKeys.None or KeyboardKeys.MAX)
                        continue;
                    Input.Keyboard.OnKeyUp(key);
                    releasedKeys++;
                }
            }
            if (Input.Mouse != null)
            {
                var position = Input.MousePosition;
                foreach (var button in Enum.GetValues<MouseButton>())
                {
                    if (button is MouseButton.None or MouseButton.MAX)
                        continue;
                    Input.Mouse.OnMouseUp(position, button);
                    releasedButtons++;
                }
            }
            return new { reset = true, releasedKeys, releasedButtons };
        }

        private static object InjectPointer(JObject arguments)
        {
            if (Input.Mouse == null)
                throw new InvalidOperationException("The runtime mouse device is unavailable.");
            var state = arguments?["state"]?.Value<string>()?.ToLowerInvariant() ?? "move";
            if (state is not ("move" or "relative" or "wheel" or "down" or "up" or "press"))
                throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0002", "Pointer state must be move, relative, wheel, down, up, or press.");
            if (state == "relative")
            {
                var relative = new Float2(arguments?["dx"]?.Value<float>() ?? 0.0f, arguments?["dy"]?.Value<float>() ?? 0.0f);
                Input.Mouse.OnMouseMoveRelative(relative);
                return new { injected = true, device = "pointer", mode = "relative", dx = relative.X, dy = relative.Y, state };
            }
            var position = new Float2(arguments?["x"]?.Value<float>() ?? Input.MousePosition.X, arguments?["y"]?.Value<float>() ?? Input.MousePosition.Y);
            if (state == "move") Input.Mouse.OnMouseMove(position);
            else if (state == "wheel") Input.Mouse.OnMouseWheel(position, arguments?["delta"]?.Value<float>() ?? 0.0f);
            else
            {
                if (!Enum.TryParse(arguments?["button"]?.Value<string>(), true, out MouseButton button))
                    throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0002", "Pointer button is required for button down/up/press.");
                if (button is MouseButton.None or MouseButton.MAX)
                    throw new CliCommandProtocolException("FLX-RUNTIME-INPUT-0002", "The pointer button cannot be None or MAX.");
                if (state is "down" or "press") Input.Mouse.OnMouseDown(position, button);
                if (state is "up" or "press") Input.Mouse.OnMouseUp(position, button);
            }
            return new { injected = true, device = "pointer", x = position.X, y = position.Y, state };
        }

        private object GetPerformance()
        {
            var stats = ProfilingTools.Stats;
            var actorCount = 0;
            var scriptCount = 0;
            var pending = new Stack<Actor>();
            foreach (var scene in Level.Scenes)
            {
                for (var i = 0; i < scene.ChildrenCount; i++)
                    pending.Push(scene.GetChild(i));
            }
            while (pending.Count != 0)
            {
                var actor = pending.Pop();
                actorCount++;
                scriptCount += actor.ScriptsCount;
                for (var i = 0; i < actor.ChildrenCount; i++)
                    pending.Push(actor.GetChild(i));
            }

            return new
            {
                timestampUtc = DateTime.UtcNow,
                profilerEnabled = ProfilingTools.Enabled,
                frame = new
                {
                    fps = stats.FPS,
                    updateTimeMs = stats.UpdateTimeMs,
                    physicsTimeMs = stats.PhysicsTimeMs,
                    drawCpuTimeMs = stats.DrawCPUTimeMs,
                    drawGpuTimeMs = stats.DrawGPUTimeMs,
                },
                memory = new
                {
                    processPhysicalBytes = stats.ProcessMemory.UsedPhysicalMemory,
                    processVirtualBytes = stats.ProcessMemory.UsedVirtualMemory,
                    systemPhysicalUsedBytes = stats.MemoryCPU.UsedPhysicalMemory,
                    systemPhysicalTotalBytes = stats.MemoryCPU.TotalPhysicalMemory,
                    gpuUsedBytes = stats.MemoryGPU.Used,
                    gpuTotalBytes = stats.MemoryGPU.Total,
                },
                rendering = new
                {
                    drawCalls = stats.DrawStats.DrawCalls,
                    dispatchCalls = stats.DrawStats.DispatchCalls,
                    vertices = stats.DrawStats.Vertices,
                    triangles = stats.DrawStats.Triangles,
                    pipelineStateChanges = stats.DrawStats.PipelineStateChanges,
                },
                scene = new
                {
                    loadedScenes = Level.ScenesCount,
                    actors = actorCount,
                    scripts = scriptCount,
                },
            };
        }

        private object GetSelection()
        {
            var actors = Editor.SceneEditing.Selection
                .OfType<ActorNode>()
                .Select(x => DescribeActor(x.Actor))
                .ToArray();
            return new
            {
                count = actors.Length,
                actors,
                unsupportedNodes = Editor.SceneEditing.SelectionCount - actors.Length,
            };
        }

        private object SetSelection(JObject arguments)
        {
            var values = arguments?["actors"] as JArray ?? throw new CliCommandProtocolException("FLX-BRIDGE-REQUEST-0002", "selection.set requires an actors array.");
            var additive = arguments?["additive"]?.Value<bool>() ?? false;
            var nodes = new List<SceneGraphNode>(values.Count);
            var seen = new HashSet<Guid>();
            foreach (var value in values)
            {
                if (!Guid.TryParse(value.Value<string>(), out var actorId) || !seen.Add(actorId))
                    throw new CliCommandProtocolException("FLX-BRIDGE-REQUEST-0002", $"Selection Actor ID '{value}' is invalid or duplicated.");
                var actor = FlaxEngine.Object.Find<Actor>(ref actorId);
                var node = actor == null ? null : Editor.Scene.GetActorNode(actor);
                if (actor == null || !actor.HasScene || node == null)
                    throw new CliCommandProtocolException("FLX-BRIDGE-REQUEST-0002", $"Selection Actor '{actorId}' was not found in a loaded scene.");
                nodes.Add(node);
            }
            Editor.SceneEditing.Select(nodes, additive, false);
            return GetSelection();
        }

        private static object DescribeActor(Actor actor)
        {
            return new
            {
                sceneId = actor.Scene?.ID ?? Guid.Empty,
                actorId = actor.ID,
                path = GetActorPath(actor),
                type = actor.TypeName,
                name = actor.Name,
            };
        }

        private static string GetActorPath(Actor actor)
        {
            var names = new Stack<string>();
            for (var current = actor; current != null; current = current.Parent)
                names.Push(current.Name);
            return "/" + string.Join("/", names);
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

        private object ClearLogs()
        {
            lock (_logsLocker)
            {
                var cleared = _logs.Count;
                _logs.Clear();
                return new { cleared, latestCursor = _logCursor };
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
            if (File.Exists(_manifestPath))
            {
                try
                {
                    File.Replace(temporaryPath, _manifestPath, null);
                }
                catch (Exception)
                {
                    // Some bundled runtimes reject a null backup path even though
                    // the API contract permits it, and their overwrite overloads
                    // have the same issue. Use only the basic file operations on
                    // that compatibility path.
                    File.Delete(_manifestPath);
                    File.Move(temporaryPath, _manifestPath);
                }
            }
            else
                File.Move(temporaryPath, _manifestPath);
            RestrictUnixPath(_manifestPath, false);
            _publishedState = GetStateName();
        }

        private static string[] GetCapabilities()
        {
            return new[] { "commands", "authoring", "settings", "bake", "eval", "evalCSharp", "visject", "player", "runtimeInput", "playMode", "playtest", "console", "saveAll", "close", "focus", "recompile", "performance", "selection", "capture" };
        }

        private static string ResolveCapturePath(JObject arguments)
        {
            var requested = arguments["path"]?.Value<string>();
            if (string.IsNullOrWhiteSpace(requested))
                throw new ArgumentException("Capture requires an output path.");
            var root = Path.GetFullPath(Globals.ProjectFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var path = Path.GetFullPath(requested, root);
            if (!path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new UnauthorizedAccessException("Capture output must remain under the project root.");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            return path;
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
