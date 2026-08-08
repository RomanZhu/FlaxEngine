// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using Flax.CLI.Commands;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

/// <summary>
/// Small MCP 2024-11-05 stdio adapter over the existing Flax CLI dispatcher.
/// </summary>
internal sealed class McpServer(CommandDispatcher dispatcher, CommandContext parentContext, string? instanceSelector)
{
    private readonly CommandDispatcher _dispatcher = dispatcher;
    private readonly CommandContext _parentContext = parentContext;
    private readonly string? _instanceSelector = instanceSelector;
    private readonly ConcurrentDictionary<string, CancellationTokenSource> _requests = new();
    private readonly SemaphoreSlim _toolGate = new(1, 1);
    private readonly SemaphoreSlim _writeGate = new(1, 1);

    public async Task<CliResult> RunAsync()
    {
        var requests = 0;
        var pending = new List<Task>();
        while (!_parentContext.CancellationToken.IsCancellationRequested)
        {
            var line = await Console.In.ReadLineAsync(_parentContext.CancellationToken);
            if (line == null)
                break;
            if (string.IsNullOrWhiteSpace(line))
                continue;
            requests++;
            pending.RemoveAll(x => x.IsCompleted);
            pending.Add(DispatchLineAsync(line));
        }
        await Task.WhenAll(pending);
        return CliResult.Ok(new { protocol = "mcp", requests, closed = true });
    }

    private async Task DispatchLineAsync(string line)
    {
        JsonObject? request;
        try
        {
            request = JsonNode.Parse(line) as JsonObject;
            if (request == null)
            {
                await WriteErrorAsync(null, -32600, "MCP request must be a JSON object.");
                return;
            }
        }
        catch (JsonException ex)
        {
            await WriteErrorAsync(null, -32700, "MCP request is invalid JSON: " + ex.Message);
            return;
        }

        var method = request["method"]?.GetValue<string>();
        var hasId = request.ContainsKey("id");
        var id = request["id"]?.DeepClone();
        if (string.IsNullOrWhiteSpace(method))
        {
            if (hasId)
                await WriteErrorAsync(id, -32600, "MCP request is missing method.");
            return;
        }

        if (method == "notifications/cancelled")
        {
            var requestId = (request["params"] as JsonObject)?["requestId"];
            if (requestId != null && _requests.TryGetValue(RequestKey(requestId), out var pendingRequest))
                pendingRequest.Cancel();
            return;
        }

        CancellationTokenSource? requestCancellation = null;
        string? requestKey = null;
        if (hasId)
        {
            requestKey = RequestKey(id);
            requestCancellation = CancellationTokenSource.CreateLinkedTokenSource(_parentContext.CancellationToken);
            if (!_requests.TryAdd(requestKey, requestCancellation))
            {
                requestCancellation.Dispose();
                await WriteErrorAsync(id, -32600, "MCP request id is already active.");
                return;
            }
        }
        var cancellationToken = requestCancellation?.Token ?? _parentContext.CancellationToken;

        try
        {
            switch (method)
            {
            case "notifications/initialized":
                return;
            case "initialize":
                if (hasId)
                    await WriteResultAsync(id, new JsonObject
                    {
                        ["protocolVersion"] = "2024-11-05",
                        ["capabilities"] = new JsonObject { ["tools"] = new JsonObject { ["listChanged"] = false } },
                        ["serverInfo"] = new JsonObject { ["name"] = "flax-cli", ["version"] = "0.1.0" },
                    });
                return;
            case "ping":
                if (hasId)
                    await WriteResultAsync(id, new JsonObject());
                return;
            case "tools/list":
                if (hasId)
                    await WriteResultAsync(id, new JsonObject { ["tools"] = await GetToolsAsync(cancellationToken) });
                return;
            case "tools/call":
                if (hasId)
                    await HandleToolCallAsync(id, request["params"] as JsonObject, cancellationToken);
                return;
            default:
                if (hasId)
                    await WriteErrorAsync(id, -32601, $"MCP method '{method}' is not supported.");
                return;
            }
        }
        catch (OperationCanceledException)
        {
            if (hasId)
                await WriteErrorAsync(id, -32800, "MCP request was cancelled.");
        }
        catch (Exception ex)
        {
            if (hasId)
                await WriteErrorAsync(id, -32000, ex.Message);
        }
        finally
        {
            if (requestKey != null && _requests.TryRemove(requestKey, out var completedRequest))
                completedRequest.Dispose();
        }
    }

    private async Task<JsonArray> GetToolsAsync(CancellationToken cancellationToken)
    {
        var tools = new JsonArray
        {
            new JsonObject
            {
                ["name"] = "flax_command",
                ["description"] = "Invoke a registered Flax typed CLI command. Destructive commands require confirm=true.",
                ["inputSchema"] = new JsonObject
                {
                    ["type"] = "object",
                    ["properties"] = new JsonObject
                    {
                        ["command"] = new JsonObject { ["type"] = "string", ["description"] = "Dotted typed command name." },
                        ["arguments"] = new JsonObject { ["type"] = "object", ["additionalProperties"] = true },
                        ["confirm"] = new JsonObject { ["type"] = "boolean", ["default"] = false },
                    },
                    ["required"] = new JsonArray("command"),
                    ["additionalProperties"] = false,
                },
            },
            new JsonObject
            {
                ["name"] = "flax_generator",
                ["description"] = "Invoke a registered project-owned Flax generator. Destructive generators require confirm=true.",
                ["inputSchema"] = new JsonObject
                {
                    ["type"] = "object",
                    ["properties"] = new JsonObject
                    {
                        ["generator"] = new JsonObject { ["type"] = "string", ["description"] = "Dotted project generator name." },
                        ["arguments"] = new JsonObject { ["type"] = "object", ["additionalProperties"] = true },
                        ["dryRun"] = new JsonObject { ["type"] = "boolean", ["default"] = false },
                        ["confirm"] = new JsonObject { ["type"] = "boolean", ["default"] = false },
                    },
                    ["required"] = new JsonArray("generator"),
                    ["additionalProperties"] = false,
                },
            },
        };

        try
        {
            var listed = await ExecuteNestedAsync(["commands", "list"], cancellationToken);
            if (listed.ExitCode == ExitCode.Success && listed.Data is JsonElement element && element.ValueKind == JsonValueKind.Array)
            {
                foreach (var descriptor in element.EnumerateArray())
                {
                    if (!descriptor.TryGetProperty("name", out var nameElement) || nameElement.ValueKind != JsonValueKind.String)
                        continue;
                    var name = nameElement.GetString();
                    if (string.IsNullOrWhiteSpace(name))
                        continue;
                    tools.Add(new JsonObject
                    {
                        ["name"] = "flax.command." + name,
                        ["description"] = descriptor.TryGetProperty("description", out var description) && description.ValueKind == JsonValueKind.String
                            ? description.GetString()
                            : "Invoke the Flax typed command '" + name + "'.",
                        ["inputSchema"] = DescribeInputSchema(descriptor),
                    });
                }
            }
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            // The generic tool remains available when no project or Editor is present.
        }

        try
        {
            var listed = await ExecuteNestedAsync(["generators", "list"], cancellationToken);
            if (listed.ExitCode == ExitCode.Success && listed.Data is JsonElement element && element.ValueKind == JsonValueKind.Array)
            {
                foreach (var descriptor in element.EnumerateArray())
                {
                    if (!descriptor.TryGetProperty("name", out var nameElement) || nameElement.ValueKind != JsonValueKind.String)
                        continue;
                    var name = nameElement.GetString();
                    if (string.IsNullOrWhiteSpace(name))
                        continue;
                    tools.Add(new JsonObject
                    {
                        ["name"] = "flax.generator." + name,
                        ["description"] = descriptor.TryGetProperty("description", out var description) && description.ValueKind == JsonValueKind.String
                            ? description.GetString()
                            : "Invoke the Flax project generator '" + name + "'.",
                        ["inputSchema"] = DescribeInputSchema(descriptor),
                    });
                }
            }
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            // The generic tool remains available when no project or Editor is present.
        }

        return tools;
    }

    private async Task HandleToolCallAsync(JsonNode? id, JsonObject? parameters, CancellationToken cancellationToken)
    {
        var tool = parameters?["name"]?.GetValue<string>();
        if (string.IsNullOrWhiteSpace(tool))
        {
            await WriteErrorAsync(id, -32602, "tools/call requires params.name.");
            return;
        }

        var arguments = parameters?["arguments"] as JsonObject ?? new JsonObject();
        var confirm = parameters?["confirm"]?.GetValue<bool>() ?? arguments["confirm"]?.GetValue<bool>() ?? false;
        if (tool.Equals("flax_command", StringComparison.OrdinalIgnoreCase))
        {
            var command = arguments["command"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(command))
            {
                await WriteErrorAsync(id, -32602, "flax_command requires arguments.command.");
                return;
            }
            arguments = arguments["arguments"] as JsonObject ?? new JsonObject();
            await InvokeToolAsync(id, command, arguments, confirm, cancellationToken);
            return;
        }

        if (tool.Equals("flax_generator", StringComparison.OrdinalIgnoreCase))
        {
            var generator = arguments["generator"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(generator))
            {
                await WriteErrorAsync(id, -32602, "flax_generator requires arguments.generator.");
                return;
            }
            var dryRun = arguments["dryRun"]?.GetValue<bool>() ?? false;
            arguments = arguments["arguments"] as JsonObject ?? new JsonObject();
            await InvokeGeneratorToolAsync(id, generator, arguments, dryRun, confirm, cancellationToken);
            return;
        }

        const string commandPrefix = "flax.command.";
        if (tool.StartsWith(commandPrefix, StringComparison.OrdinalIgnoreCase))
        {
            arguments.Remove("confirm");
            await InvokeToolAsync(id, tool[commandPrefix.Length..], arguments, confirm, cancellationToken);
            return;
        }

        const string generatorPrefix = "flax.generator.";
        if (tool.StartsWith(generatorPrefix, StringComparison.OrdinalIgnoreCase))
        {
            arguments.Remove("confirm");
            await InvokeGeneratorToolAsync(id, tool[generatorPrefix.Length..], arguments, false, confirm, cancellationToken);
            return;
        }

        await WriteErrorAsync(id, -32602, $"Unknown MCP tool '{tool}'.");
    }

    private async Task InvokeToolAsync(JsonNode? id, string command, JsonObject arguments, bool confirm, CancellationToken cancellationToken)
    {
        var tokens = new List<string> { "command", command, "--arguments", arguments.ToJsonString(JsonSupport.Options) };
        if (confirm)
            tokens.Add("--yes");
        await InvokeNestedToolAsync(id, tokens, cancellationToken);
    }

    private async Task InvokeGeneratorToolAsync(JsonNode? id, string generator, JsonObject arguments, bool dryRun, bool confirm, CancellationToken cancellationToken)
    {
        var tokens = new List<string> { "generators", "run", generator, "--arguments", arguments.ToJsonString(JsonSupport.Options) };
        if (dryRun)
            tokens.Add("--dry-run");
        if (confirm)
            tokens.Add("--yes");
        await InvokeNestedToolAsync(id, tokens, cancellationToken);
    }

    private async Task InvokeNestedToolAsync(JsonNode? id, List<string> tokens, CancellationToken cancellationToken)
    {
        if (!string.IsNullOrWhiteSpace(_instanceSelector))
        {
            tokens.Add("--instance");
            tokens.Add(_instanceSelector);
        }
        await _toolGate.WaitAsync(cancellationToken);
        CliResult result;
        try
        {
            result = await ExecuteNestedAsync(tokens, cancellationToken);
        }
        finally
        {
            _toolGate.Release();
        }
        var payload = new JsonObject
        {
            ["success"] = result.ExitCode == ExitCode.Success,
            ["data"] = ToNode(result.Data),
            ["errors"] = JsonSerializer.SerializeToNode(result.Errors, JsonSupport.Options),
            ["warnings"] = JsonSerializer.SerializeToNode(result.Warnings, JsonSupport.Options),
            ["events"] = JsonSerializer.SerializeToNode(result.Events, JsonSupport.Options),
        };
        var text = payload.ToJsonString(new JsonSerializerOptions(JsonSupport.Options) { WriteIndented = false });
        await WriteResultAsync(id, new JsonObject
        {
            ["content"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = text }),
            ["isError"] = result.ExitCode != ExitCode.Success,
        });
    }

    private async Task<CliResult> ExecuteNestedAsync(IReadOnlyList<string> tokens, CancellationToken cancellationToken)
    {
        var options = new GlobalOptions
        {
            Format = "json",
            FormatSpecified = true,
            Quiet = true,
            Verbose = _parentContext.Options.Verbose,
            NoColor = true,
            NonInteractive = true,
            Project = _parentContext.Options.Project,
            Engine = _parentContext.Options.Engine,
            Timeout = _parentContext.Options.Timeout,
            GracefulShutdownTimeout = _parentContext.Options.GracefulShutdownTimeout,
        };
        options.CommandTokens.AddRange(tokens);
        return await _dispatcher.ExecuteAsync(new CommandContext
        {
            Options = options,
            CancellationToken = cancellationToken,
            Stopwatch = Stopwatch.StartNew(),
        });
    }

    private static JsonObject DescribeInputSchema(JsonElement descriptor)
    {
        var properties = new JsonObject();
        var required = new JsonArray();
        if (descriptor.TryGetProperty("parameters", out var parameters) && parameters.ValueKind == JsonValueKind.Array)
        {
            foreach (var parameter in parameters.EnumerateArray())
            {
                if (!parameter.TryGetProperty("name", out var name) || name.ValueKind != JsonValueKind.String)
                    continue;
                var parameterName = name.GetString();
                if (string.IsNullOrWhiteSpace(parameterName))
                    continue;
                var schema = parameter.TryGetProperty("schema", out var schemaElement) ? schemaElement : default;
                var property = new JsonObject { ["type"] = MapType(schema) };
                if (parameter.TryGetProperty("description", out var description) && description.ValueKind == JsonValueKind.String)
                    property["description"] = description.GetString();
                properties[parameterName] = property;
                if (parameter.TryGetProperty("required", out var requiredElement) && requiredElement.ValueKind == JsonValueKind.True)
                    required.Add(parameterName);
            }
        }
        properties["confirm"] = new JsonObject { ["type"] = "boolean", ["default"] = false, ["description"] = "Confirm destructive commands." };
        return new JsonObject
        {
            ["type"] = "object",
            ["properties"] = properties,
            ["required"] = required,
            ["additionalProperties"] = false,
        };
    }

    private static string MapType(JsonElement schema)
    {
        if (schema.ValueKind == JsonValueKind.Object && schema.TryGetProperty("type", out var type) && type.ValueKind == JsonValueKind.String)
        {
            return type.GetString() switch
            {
                "integer" => "integer",
                "number" => "number",
                "boolean" => "boolean",
                "array" => "array",
                "object" => "object",
                _ => "string",
            };
        }
        return "object";
    }

    private static JsonNode? ToNode(object? value)
    {
        if (value is JsonElement element)
            return JsonNode.Parse(element.GetRawText());
        return JsonSerializer.SerializeToNode(value, JsonSupport.Options);
    }

    private static string RequestKey(JsonNode? id) => id?.ToJsonString() ?? "null";

    private async Task WriteResultAsync(JsonNode? id, JsonObject result)
    {
        var response = new JsonObject { ["jsonrpc"] = "2.0", ["id"] = id?.DeepClone(), ["result"] = result };
        await WriteLineAsync(response);
    }

    private async Task WriteErrorAsync(JsonNode? id, int code, string message)
    {
        var response = new JsonObject
        {
            ["jsonrpc"] = "2.0",
            ["id"] = id?.DeepClone(),
            ["error"] = new JsonObject { ["code"] = code, ["message"] = message },
        };
        await WriteLineAsync(response);
    }

    private async Task WriteLineAsync(JsonObject value)
    {
        await _writeGate.WaitAsync();
        try
        {
            await Console.Out.WriteLineAsync(value.ToJsonString(new JsonSerializerOptions(JsonSupport.Options) { WriteIndented = false }));
            await Console.Out.FlushAsync();
        }
        finally
        {
            _writeGate.Release();
        }
    }
}
