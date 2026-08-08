// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_GAME && (BUILD_DEBUG || BUILD_DEVELOPMENT)
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace FlaxEngine
{
    /// <summary>Authenticated development Player control and deterministic raw input bridge.</summary>
    internal static class CliPlayerBridge
    {
        private const int ProtocolVersion = 1;
        private const int MaxRequestCharacters = 1024 * 1024;
        private static readonly ConcurrentQueue<Pending> Requests = new ConcurrentQueue<Pending>();
        private static readonly CancellationTokenSource Stop = new CancellationTokenSource();
        private static readonly HashSet<KeyboardKeys> PressedKeys = new HashSet<KeyboardKeys>();
        private static readonly HashSet<MouseButton> PressedButtons = new HashSet<MouseButton>();
        private static string _token, _endpoint, _manifest, _tokenPath, _instanceId;
        private static bool _paused;
        private static bool _stepPending;
        private static float _resumeTimeScale = 1.0f;
        private static Task _listener;

        public static void Initialize()
        {
            if (_listener != null || Engine.IsEditor) return;
            try
            {
                var runtime = GetRuntimeDirectory();
                Directory.CreateDirectory(runtime);
                RestrictUnixPath(runtime, true);
                _instanceId = "player-" + Process.GetCurrentProcess().Id + "-" + Guid.NewGuid().ToString("N");
                _token = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));
                _tokenPath = Path.Combine(runtime, _instanceId + ".token");
                _manifest = Path.Combine(runtime, _instanceId + ".instance.json");
                _endpoint = "flax-player-" + Process.GetCurrentProcess().Id + "-" + Guid.NewGuid().ToString("N");
                File.WriteAllText(_tokenPath, _token, new UTF8Encoding(false));
                RestrictUnixPath(_tokenPath, false);
                WriteManifest();
                Scripting.Update += OnUpdate;
                AppDomain.CurrentDomain.ProcessExit += OnProcessExit;
                _listener = Task.Run(Listen, Stop.Token);
            }
            catch (Exception ex) { Debug.LogWarning("CLI Player bridge could not start: " + ex.Message); }
        }

        private static void OnUpdate()
        {
            if (_stepPending)
            {
                _stepPending = false;
                _paused = true;
                Time.TimeScale = 0.0f;
            }
            while (Requests.TryDequeue(out var pending))
            {
                try { pending.Completion.TrySetResult(Execute(pending.Request)); }
                catch (Exception ex) { pending.Completion.TrySetException(ex); }
            }
        }

        private static object Execute(PlayerRequest request)
        {
            switch (request.Action)
            {
            case "status": case "player.status": return Status();
            case "player.pause": return Pause();
            case "player.resume": return Resume();
            case "player.step": return Step();
            case "player.quit":
                ResetInput();
                CleanupFiles();
                Engine.RequestExit(0);
                return new { requested = "quit" };
            case "runtime.input.key": return InjectKey(request.Arguments);
            case "runtime.input.pointer": return InjectPointer(request.Arguments);
            case "runtime.input.reset": return ResetInput();
            case "runtime.input.inspect": return CliInputProbe.Capture(request.Arguments);
            case "runtime.input.gamepad": case "runtime.input.action": throw new InvalidOperationException("Only raw keyboard and mouse injection is supported by the stable Player input ABI.");
            case "performance": return new { timestampUtc = DateTime.UtcNow, frame = new { deltaTime = Time.DeltaTime, timeScale = Time.TimeScale } };
            default: throw new InvalidOperationException("Unsupported Player bridge action '" + request.Action + "'.");
            }
        }

        private static object Pause()
        {
            if (!_paused)
                _resumeTimeScale = Time.TimeScale;
            _paused = true;
            _stepPending = false;
            Time.TimeScale = 0.0f;
            return new { requested = "pause", status = Status() };
        }

        private static object Resume()
        {
            _paused = false;
            _stepPending = false;
            Time.TimeScale = _resumeTimeScale;
            return new { requested = "resume", status = Status() };
        }

        private static object Step()
        {
            if (!_paused)
                throw new InvalidOperationException("The Player must be paused before requesting a single-frame step.");
            if (_stepPending)
                throw new InvalidOperationException("A single-frame step is already pending.");
            _stepPending = true;
            Time.TimeScale = Math.Abs(_resumeTimeScale) > float.Epsilon ? _resumeTimeScale : 1.0f;
            return new { requested = "step", status = Status() };
        }

        private static object Status() => new
        {
            instanceId = _instanceId,
            kind = "player",
            state = _stepPending ? "stepping" : _paused ? "paused" : "running",
            projectPath = Path.GetFullPath(Globals.ProjectFolder),
            engineVersion = Globals.EngineVersion,
            playMode = true,
            paused = _paused,
            stepPending = _stepPending,
            timeScale = Time.TimeScale,
            resumeTimeScale = _resumeTimeScale,
            capabilities = new[] { "player", "runtimeInput", "performance" },
        };

        private static object InjectKey(Newtonsoft.Json.Linq.JObject arguments)
        {
            if (Input.Keyboard == null) throw new InvalidOperationException("The runtime keyboard is unavailable.");
            if (!Enum.TryParse(StringArg(arguments, "key"), true, out KeyboardKeys key)) throw new ArgumentException("Unknown keyboard key.");
            if (key is KeyboardKeys.None or KeyboardKeys.MAX) throw new ArgumentException("The keyboard key cannot be None or MAX.");
            var state = (StringArg(arguments, "state") ?? "press").ToLowerInvariant();
            if (state is not ("down" or "up" or "press")) throw new ArgumentException("Keyboard state must be down, up, or press.");
            if (state is "down" or "press")
                Input.Keyboard.OnKeyDown(key);
            if (state == "down")
                PressedKeys.Add(key);
            if (state is "up" or "press")
            {
                Input.Keyboard.OnKeyUp(key);
                PressedKeys.Remove(key);
            }
            return new { injected = true, device = "keyboard", key = key.ToString(), state };
        }

        private static object ResetInput()
        {
            var releasedKeys = 0;
            var releasedButtons = 0;
            if (Input.Keyboard != null)
            {
                foreach (var key in PressedKeys)
                {
                    Input.Keyboard.OnKeyUp(key);
                    releasedKeys++;
                }
            }
            PressedKeys.Clear();
            if (Input.Mouse != null)
            {
                var position = Input.MousePosition;
                foreach (var button in PressedButtons)
                {
                    Input.Mouse.OnMouseUp(position, button);
                    releasedButtons++;
                }
            }
            PressedButtons.Clear();
            return new { reset = true, releasedKeys, releasedButtons };
        }

        private static object InjectPointer(Newtonsoft.Json.Linq.JObject arguments)
        {
            if (Input.Mouse == null) throw new InvalidOperationException("The runtime mouse is unavailable.");
            var state = (StringArg(arguments, "state") ?? "move").ToLowerInvariant();
            if (state is not ("move" or "relative" or "wheel" or "down" or "up" or "press"))
                throw new ArgumentException("Pointer state must be move, relative, wheel, down, up, or press.");
            if (state == "relative")
            {
                var relative = new Float2(FloatArg(arguments, "dx", 0.0f), FloatArg(arguments, "dy", 0.0f));
                Input.Mouse.OnMouseMoveRelative(relative);
                return new { injected = true, device = "pointer", mode = "relative", dx = relative.X, dy = relative.Y, state };
            }
            var position = new Float2(FloatArg(arguments, "x", Input.MousePosition.X), FloatArg(arguments, "y", Input.MousePosition.Y));
            if (state == "move") Input.Mouse.OnMouseMove(position);
            else if (state == "wheel") Input.Mouse.OnMouseWheel(position, FloatArg(arguments, "delta", 0.0f));
            else
            {
                if (!Enum.TryParse(StringArg(arguments, "button"), true, out MouseButton button)) throw new ArgumentException("Unknown mouse button.");
                if (button is MouseButton.None or MouseButton.MAX) throw new ArgumentException("The mouse button cannot be None or MAX.");
                if (state is "down" or "press")
                    Input.Mouse.OnMouseDown(position, button);
                if (state == "down")
                    PressedButtons.Add(button);
                if (state is "up" or "press")
                {
                    Input.Mouse.OnMouseUp(position, button);
                    PressedButtons.Remove(button);
                }
            }
            return new { injected = true, device = "pointer", x = position.X, y = position.Y, state };
        }

        private static string StringArg(Newtonsoft.Json.Linq.JObject arguments, string name) => arguments?[name]?.ToString();

        private static float FloatArg(Newtonsoft.Json.Linq.JObject arguments, string name, float fallback)
        {
            var value = StringArg(arguments, name);
            return float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) ? parsed : fallback;
        }

        private static async Task Listen()
        {
            while (!Stop.IsCancellationRequested)
            {
                try
                {
                    using var pipe = new NamedPipeServerStream(_endpoint, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
                    await pipe.WaitForConnectionAsync(Stop.Token).ConfigureAwait(false);
                    using var reader = new StreamReader(pipe, new UTF8Encoding(false), false, 8192, true);
                    using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
                    var requestText = await reader.ReadLineAsync().ConfigureAwait(false) ?? string.Empty;
                    if (requestText.Length > MaxRequestCharacters)
                        throw new InvalidOperationException("The Player bridge request exceeds the one-megabyte limit.");
                    var request = JsonConvert.DeserializeObject<PlayerRequest>(requestText);
                    if (request == null || request.SchemaVersion != ProtocolVersion || !TokenEquals(request.Token, _token))
                    {
                        await writer.WriteLineAsync(JsonConvert.SerializeObject(new PlayerResponse { RequestId = request?.RequestId, Success = false, Errors = new[] { "FLX-PLAYER-AUTH-0003" } })).ConfigureAwait(false);
                        continue;
                    }
                    var pending = new Pending(request); Requests.Enqueue(pending);
                    try
                    {
                        var value = await pending.Completion.Task.WaitAsync(TimeSpan.FromSeconds(Math.Max(1, request.TimeoutSeconds))).ConfigureAwait(false);
                        await writer.WriteLineAsync(JsonConvert.SerializeObject(new PlayerResponse { RequestId = request.RequestId, Success = true, Data = value })).ConfigureAwait(false);
                    }
                    catch (Exception ex) { await writer.WriteLineAsync(JsonConvert.SerializeObject(new PlayerResponse { RequestId = request.RequestId, Success = false, Errors = new[] { ex.Message } })).ConfigureAwait(false); }
                }
                catch (OperationCanceledException) { break; }
                catch (Exception ex) { Debug.LogWarning("CLI Player bridge request failed: " + ex.Message); }
            }
        }

        private static void WriteManifest()
        {
            var value = new { schemaVersion = 1, instanceId = _instanceId, pid = Process.GetCurrentProcess().Id, processStartTimeUtc = Process.GetCurrentProcess().StartTime.ToUniversalTime(), kind = "player", projectPath = Path.GetFullPath(Globals.ProjectFolder), engineVersion = Globals.EngineVersion, protocolVersion = ProtocolVersion, transport = "namedPipe", endpoint = _endpoint, tokenPath = _tokenPath, state = "running", capabilities = new[] { "player", "runtimeInput", "performance" } };
            var temporary = _manifest + ".tmp";
            File.WriteAllText(temporary, JsonConvert.SerializeObject(value, Formatting.Indented), new UTF8Encoding(false));
            if (File.Exists(_manifest)) File.Delete(_manifest);
            File.Move(temporary, _manifest);
            RestrictUnixPath(_manifest, false);
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

        private static bool TokenEquals(string provided, string expected)
        {
            if (provided == null || expected == null)
                return false;
            var left = Encoding.UTF8.GetBytes(provided);
            var right = Encoding.UTF8.GetBytes(expected);
            return left.Length == right.Length && CryptographicOperations.FixedTimeEquals(left, right);
        }

        private static void OnProcessExit(object sender, EventArgs args)
        {
            Stop.Cancel();
            CleanupFiles();
        }

        private static void CleanupFiles()
        {
            TryDeleteFile(_manifest);
            TryDeleteFile(_tokenPath);
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
                // Process-exit cleanup is best-effort.
            }
        }

        private sealed class Pending
        {
            public readonly PlayerRequest Request;
            public readonly TaskCompletionSource<object> Completion = new TaskCompletionSource<object>(TaskCreationOptions.RunContinuationsAsynchronously);
            public Pending(PlayerRequest request) { Request = request; }
        }
        private sealed class PlayerRequest
        {
            [JsonProperty("schemaVersion")] public int SchemaVersion { get; set; }
            [JsonProperty("requestId")] public string RequestId { get; set; }
            [JsonProperty("token")] public string Token { get; set; }
            [JsonProperty("action")] public string Action { get; set; }
            [JsonProperty("arguments")] public Newtonsoft.Json.Linq.JObject Arguments { get; set; }
            [JsonProperty("timeoutSeconds")] public double TimeoutSeconds { get; set; } = 30;
        }
        private sealed class PlayerResponse
        {
            [JsonProperty("schemaVersion")] public int SchemaVersion { get; set; } = ProtocolVersion;
            [JsonProperty("requestId")] public string RequestId { get; set; }
            [JsonProperty("success")] public bool Success { get; set; }
            [JsonProperty("data")] public object Data { get; set; }
            [JsonProperty("errors")] public string[] Errors { get; set; } = Array.Empty<string>();
        }
    }
}
#endif
