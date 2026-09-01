// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Flax.CLI.Core;
using Flax.CLI.Protocol;

namespace Flax.CLI.Services;

internal sealed class EditorBridgeClient(AppPaths paths)
{
    private const int ProtocolVersion = 1;
    private const int MaxResponseCharacters = 4 * 1024 * 1024;
    private static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan DefaultAssetImportTimeout = TimeSpan.FromHours(1);
    private static readonly TimeSpan ResponseTimeoutGrace = TimeSpan.FromSeconds(5);

    public IReadOnlyList<EditorInstanceManifest> Discover()
    {
        if (!Directory.Exists(paths.RuntimeDirectory))
            return [];

        var result = new List<EditorInstanceManifest>();
        foreach (var manifestPath in Directory.EnumerateFiles(paths.RuntimeDirectory, "*.instance.json", SearchOption.TopDirectoryOnly))
        {
            EditorInstanceManifest? manifest;
            try
            {
                manifest = JsonSerializer.Deserialize<EditorInstanceManifest>(File.ReadAllText(manifestPath), JsonSupport.Options);
            }
            catch (JsonException)
            {
                continue;
            }
            catch (IOException)
            {
                continue;
            }
            if (!IsValidManifest(manifest))
                continue;
            var validManifest = manifest!;
            validManifest.ManifestPath = manifestPath;
            if (!IsLive(validManifest))
            {
                PruneStale(validManifest);
                continue;
            }
            result.Add(validManifest);
        }
        return result.OrderBy(x => x.ProjectPath, ProjectRegistry.PathComparer).ThenBy(x => x.Pid).ToArray();
    }

    public EditorInstanceManifest? Select(string? projectPath, string? selector, bool required, string? kind = null)
    {
        var candidates = Discover().AsEnumerable();
        if (!string.IsNullOrWhiteSpace(kind))
            candidates = candidates.Where(x => string.Equals(x.Kind, kind, StringComparison.OrdinalIgnoreCase));
        if (!string.IsNullOrWhiteSpace(projectPath))
        {
            var canonicalProject = Path.GetFullPath(projectPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            candidates = candidates.Where(x => ProjectRegistry.PathComparer.Equals(Path.GetFullPath(x.ProjectPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar), canonicalProject));
        }
        if (!string.IsNullOrWhiteSpace(selector))
        {
            candidates = candidates.Where(x =>
                string.Equals(x.InstanceId, selector, StringComparison.OrdinalIgnoreCase) ||
                x.InstanceId.StartsWith(selector, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(x.Pid.ToString(), selector, StringComparison.Ordinal));
        }
        var matches = candidates.ToArray();
        if (matches.Length == 1)
            return matches[0];
        if (matches.Length == 0)
        {
            if (!required)
                return null;
            throw new CliException(ExitCode.ContextRequired, "FLX-BRIDGE-NOTFOUND-0004", $"No compatible running Flax {kind ?? "Editor"} instance matched the request.", new { project = projectPath, instance = selector, kind });
        }
        throw new CliException(ExitCode.ContextRequired, "FLX-BRIDGE-AMBIGUOUS-0004", "More than one running Flax Editor instance matched the request. Select one with --instance.", new { candidates = matches.Select(View).ToArray() });
    }

    public async Task<EditorBridgeInvocation> InvokeAsync(EditorInstanceManifest instance, string action, string? name, JsonObject? arguments, bool confirm, CommandContext context)
    {
        RequireCapability(instance, action, name);
        if (!IsUnderRuntimeDirectory(instance.TokenPath))
            throw new CliException(ExitCode.Authorization, "FLX-BRIDGE-AUTH-0003", "The Editor bridge token path is outside the Flax CLI runtime directory.");
        string token;
        try
        {
            token = File.ReadAllText(instance.TokenPath).Trim();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new CliException(ExitCode.Authorization, "FLX-BRIDGE-AUTH-0003", "The Editor bridge token could not be read.", new { exception = ex.Message });
        }

        var requestId = Guid.NewGuid().ToString("N");
        var requestTimeout = ResolveRequestTimeout(action, name, arguments, context.Options.Timeout);
        var request = new EditorBridgeRequest
        {
            RequestId = requestId,
            Token = token,
            Action = action,
            Name = name,
            Arguments = arguments ?? new JsonObject(),
            Confirm = confirm,
            TimeoutSeconds = Math.Max(1, Math.Min(3600, requestTimeout.TotalSeconds)),
        };
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(context.CancellationToken);
        try
        {
            timeout.CancelAfter(context.Options.Timeout ?? requestTimeout + ResponseTimeoutGrace);
            await using var stream = await ConnectAsync(instance, timeout.Token);
            using var reader = new StreamReader(stream, new UTF8Encoding(false), false, 8192, true);
            await using var writer = new StreamWriter(stream, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
            var serializerOptions = new JsonSerializerOptions(JsonSupport.Options) { WriteIndented = false };
            await writer.WriteLineAsync(JsonSerializer.Serialize(request, serializerOptions).AsMemory(), timeout.Token);
            var line = await ReadResponseLineAsync(reader, timeout.Token);
            if (string.IsNullOrWhiteSpace(line))
                throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-PROTOCOL-0006", "The Editor bridge returned an empty response.");
            JsonElement response;
            try
            {
                using var document = JsonDocument.Parse(line);
                response = document.RootElement.Clone();
            }
            catch (JsonException ex)
            {
                throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-PROTOCOL-0006", "The Editor bridge response is invalid JSON.", new { exception = ex.Message });
            }
            if (!TryGetProperty(response, "schemaVersion", out var schemaVersion) || !schemaVersion.TryGetInt32(out var responseProtocol) || responseProtocol != ProtocolVersion)
                throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-PROTOCOL-0006", "The Editor bridge response protocol is unsupported.");
            if (!TryGetProperty(response, "requestId", out var returnedId) || returnedId.ValueKind != JsonValueKind.String || returnedId.GetString() != requestId)
                throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-PROTOCOL-0006", "The Editor bridge response request ID does not match.", new
                {
                    expectedRequestId = requestId,
                    returnedRequestId = returnedId.ValueKind == JsonValueKind.String ? returnedId.GetString() : null,
                });
            if (!TryGetProperty(response, "success", out var success) || success.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-PROTOCOL-0006", "The Editor bridge response is missing its success state.");
            return new EditorBridgeInvocation(instance, response);
        }
        catch (OperationCanceledException) when (!context.CancellationToken.IsCancellationRequested)
        {
            throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-TIMEOUT-0006", "The live Editor request timed out.");
        }
        catch (Exception ex) when (ex is IOException or SocketException)
        {
            throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-CONNECT-0006", "The running Editor bridge disconnected or could not be reached.", new { exception = ex.Message });
        }
    }

    internal static TimeSpan ResolveRequestTimeout(string action, string? name, JsonObject? arguments, TimeSpan? configuredTimeout)
    {
        if (configuredTimeout.HasValue)
            return configuredTimeout.Value;
        return IsAssetImportInvocation(action, name, arguments) ? DefaultAssetImportTimeout : DefaultRequestTimeout;
    }

    private static bool IsAssetImportInvocation(string action, string? name, JsonObject? arguments)
    {
        if (!string.Equals(action, "command.invoke", StringComparison.OrdinalIgnoreCase) || arguments == null)
            return false;
        if (string.Equals(name, "assets.execute", StringComparison.OrdinalIgnoreCase))
            return IsImportOperation(arguments["operation"]);
        if (!string.Equals(name, "assets.batch", StringComparison.OrdinalIgnoreCase) || arguments["operations"] is not JsonArray operations)
            return false;
        return operations.Any(IsImportOperation);
    }

    private static bool IsImportOperation(JsonNode? node)
    {
        if (node is not JsonObject operationObject)
            return false;
        var operation = operationObject["action"]?.GetValue<string>();
        return string.Equals(operation, "import", StringComparison.OrdinalIgnoreCase) ||
               string.Equals(operation, "reimport", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryGetProperty(JsonElement value, string name, out JsonElement property)
    {
        if (value.TryGetProperty(name, out property))
            return true;
        foreach (var candidate in value.EnumerateObject())
        {
            if (string.Equals(candidate.Name, name, StringComparison.OrdinalIgnoreCase))
            {
                property = candidate.Value;
                return true;
            }
        }
        property = default;
        return false;
    }

    public static object View(EditorInstanceManifest instance) => new
    {
        instance.InstanceId,
        instance.Pid,
        instance.Kind,
        instance.ProjectPath,
        instance.EngineVersion,
        instance.EngineNickname,
        instance.ProtocolVersion,
        instance.Transport,
        instance.State,
        instance.Capabilities,
    };

    private static async Task<string?> ReadResponseLineAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        var result = new StringBuilder();
        var buffer = new char[4096];
        while (true)
        {
            var read = await reader.ReadAsync(buffer.AsMemory(), cancellationToken);
            if (read == 0)
                return result.Length == 0 ? null : result.ToString();
            for (var i = 0; i < read; i++)
            {
                if (buffer[i] == '\n')
                {
                    if (result.Length != 0 && result[^1] == '\r')
                        result.Length--;
                    return result.ToString();
                }
                result.Append(buffer[i]);
                if (result.Length > MaxResponseCharacters)
                    throw new CliException(ExitCode.OperationFailed, "FLX-BRIDGE-PROTOCOL-0006", "The Editor bridge returned an oversized response.");
            }
        }
    }

    private async Task<Stream> ConnectAsync(EditorInstanceManifest instance, CancellationToken cancellationToken)
    {
        if (instance.Transport.Equals("namedPipe", StringComparison.OrdinalIgnoreCase))
        {
            if (instance.Endpoint.IndexOfAny([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) >= 0)
                throw new CliException(ExitCode.Authorization, "FLX-BRIDGE-ENDPOINT-0003", "The named-pipe endpoint is invalid.");
            var pipe = new NamedPipeClientStream(".", instance.Endpoint, PipeDirection.InOut, PipeOptions.Asynchronous, TokenImpersonationLevel.None);
            try
            {
                await pipe.ConnectAsync(cancellationToken);
                return pipe;
            }
            catch
            {
                pipe.Dispose();
                throw;
            }
        }
        if (instance.Transport.Equals("unixSocket", StringComparison.OrdinalIgnoreCase))
        {
            if (!IsUnderRuntimeDirectory(instance.Endpoint))
                throw new CliException(ExitCode.Authorization, "FLX-BRIDGE-ENDPOINT-0003", "The Unix-socket endpoint is outside the Flax CLI runtime directory.");
            var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            try
            {
                await socket.ConnectAsync(new UnixDomainSocketEndPoint(instance.Endpoint), cancellationToken);
                return new NetworkStream(socket, ownsSocket: true);
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }
        throw new CliException(ExitCode.ContextRequired, "FLX-BRIDGE-TRANSPORT-0004", $"Unsupported Editor bridge transport '{instance.Transport}'.");
    }

    private bool IsLive(EditorInstanceManifest manifest)
    {
        try
        {
            var process = Process.GetProcessById(manifest.Pid);
            if (process.HasExited)
                return false;
            if (manifest.ProcessStartTimeUtc != default && Math.Abs((process.StartTime.ToUniversalTime() - manifest.ProcessStartTimeUtc).TotalSeconds) > 2)
                return false;
            return File.Exists(manifest.TokenPath);
        }
        catch
        {
            return false;
        }
    }

    private bool IsValidManifest(EditorInstanceManifest? manifest)
    {
        if (manifest == null || manifest.SchemaVersion != 1 || manifest.ProtocolVersion != ProtocolVersion || manifest.Pid <= 0 ||
            string.IsNullOrWhiteSpace(manifest.InstanceId) || string.IsNullOrWhiteSpace(manifest.Kind) ||
            string.IsNullOrWhiteSpace(manifest.ProjectPath) || !Path.IsPathRooted(manifest.ProjectPath) ||
            string.IsNullOrWhiteSpace(manifest.Transport) || string.IsNullOrWhiteSpace(manifest.Endpoint) ||
            manifest.Capabilities == null || !IsUnderRuntimeDirectory(manifest.TokenPath))
        {
            return false;
        }
        if (manifest.Transport.Equals("namedPipe", StringComparison.OrdinalIgnoreCase))
            return manifest.Endpoint.IndexOfAny([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) < 0;
        if (manifest.Transport.Equals("unixSocket", StringComparison.OrdinalIgnoreCase))
            return IsUnderRuntimeDirectory(manifest.Endpoint);
        return false;
    }

    private static void RequireCapability(EditorInstanceManifest instance, string action, string? name)
    {
        var capability = action switch
        {
            "commands.list" or "commands.info" or "generators.list" or "generators.info" => "commands",
            "command.invoke" when name != null && (name.StartsWith("visject.", StringComparison.OrdinalIgnoreCase) || name.StartsWith("material.graph.", StringComparison.OrdinalIgnoreCase) || name.StartsWith("animation.graph.", StringComparison.OrdinalIgnoreCase)) => "visject",
            "command.invoke" when name != null && name.StartsWith("dev.eval-csharp", StringComparison.OrdinalIgnoreCase) => "evalCSharp",
            "command.invoke" => "commands",
            "generator.invoke" => "commands",
            "editor.play" or "editor.pause" or "editor.resume" or "editor.stop" or "editor.step" => "playMode",
            "editor.focus" => "focus",
            "editor.saveAll" => "saveAll",
            "editor.close" => "close",
            "editor.recompile" => "recompile",
            "console" or "console.clear" => "console",
            "performance" => "performance",
            "selection.get" or "selection.set" or "selection.clear" => "selection",
            "capture.viewport" or "capture.game" => "capture",
            "player.status" or "player.pause" or "player.resume" or "player.step" or "player.quit" => "player",
            "runtime.input.key" or "runtime.input.pointer" or "runtime.input.gamepad" or "runtime.input.action" or "runtime.input.reset" or "runtime.input.inspect" => "runtimeInput",
            "eval-csharp" or "dev.eval-csharp" => "evalCSharp",
            _ => null,
        };
        if (capability != null && !instance.Capabilities.Contains(capability, StringComparer.OrdinalIgnoreCase))
        {
            throw new CliException(
                ExitCode.ContextRequired,
                "FLX-BRIDGE-CAPABILITY-0004",
                $"The selected Editor instance does not advertise the '{capability}' capability required by '{action}'.",
                new { instance = View(instance), requiredCapability = capability });
        }
    }

    private void PruneStale(EditorInstanceManifest manifest)
    {
        TryDelete(manifest.ManifestPath);
        if (IsUnderRuntimeDirectory(manifest.TokenPath))
            TryDelete(manifest.TokenPath);
        if (manifest.Transport.Equals("unixSocket", StringComparison.OrdinalIgnoreCase) && IsUnderRuntimeDirectory(manifest.Endpoint))
            TryDelete(manifest.Endpoint);
    }

    private bool IsUnderRuntimeDirectory(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || !Path.IsPathRooted(value))
            return false;
        try
        {
            var root = Path.GetFullPath(paths.RuntimeDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var path = Path.GetFullPath(value);
            return ProjectRegistry.PathComparer.Equals(path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar), root) ||
                   path.StartsWith(root + Path.DirectorySeparatorChar, OperatingSystem.IsWindows() || OperatingSystem.IsMacOS() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
                File.Delete(path);
        }
        catch
        {
            // Stale cleanup is best-effort.
        }
    }
}
