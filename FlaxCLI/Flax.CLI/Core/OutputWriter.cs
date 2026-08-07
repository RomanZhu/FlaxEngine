// Copyright (c) Wojciech Figat. All rights reserved.

using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Flax.CLI.Core;

internal sealed class OutputWriter(TextWriter stdout, TextWriter stderr)
{
    public void Write(string command, CliResult result, GlobalOptions options, TimeSpan duration, string requestId)
    {
        var envelope = new
        {
            schemaVersion = "1.0",
            success = result.ExitCode == ExitCode.Success,
            command,
            data = result.Data,
            errors = result.Errors,
            warnings = result.Warnings,
            events = result.Events,
            meta = new
            {
                cliVersion = Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "0.1.0",
                durationMs = (long)duration.TotalMilliseconds,
                requestId,
            },
        };

        switch (options.Format)
        {
        case "json":
            stdout.WriteLine(JsonSerializer.Serialize(envelope, JsonSupport.Options));
            break;
        case "ndjson":
            foreach (var item in result.Events)
                stdout.WriteLine(JsonSerializer.Serialize(item, CompactJson));
            stdout.WriteLine(JsonSerializer.Serialize(new { type = "result", envelope }, CompactJson));
            break;
        case "tsv":
            WriteTsv(result);
            break;
        default:
            WriteHuman(result, options.Quiet);
            break;
        }
    }

    private void WriteHuman(CliResult result, bool quiet)
    {
        foreach (var error in result.Errors)
            stderr.WriteLine($"{error.Code}: {Sanitize(error.Message)}");
        foreach (var warning in result.Warnings)
            stderr.WriteLine($"warning {warning.Code}: {Sanitize(warning.Message)}");
        if (quiet || result.Data == null)
            return;
        if (result.Data is string text)
        {
            stdout.WriteLine(Sanitize(text));
            return;
        }
        stdout.WriteLine(JsonSerializer.Serialize(result.Data, JsonSupport.Options));
    }

    private void WriteTsv(CliResult result)
    {
        if (result.ExitCode != ExitCode.Success)
        {
            foreach (var error in result.Errors)
                stdout.WriteLine($"error\t{Cell(error.Code)}\t{Cell(error.Message)}");
            return;
        }

        var node = JsonSerializer.SerializeToNode(result.Data, JsonSupport.Options);
        if (node is JsonArray array)
        {
            foreach (var item in array)
                WriteTsvNode(item);
        }
        else
        {
            WriteTsvNode(node);
        }
    }

    private void WriteTsvNode(JsonNode? node)
    {
        if (node is JsonObject value)
            stdout.WriteLine(string.Join('\t', value.Select(x => Cell(Scalar(x.Value)))));
        else if (node != null)
            stdout.WriteLine(Cell(Scalar(node)));
    }

    private static string Scalar(JsonNode? node)
    {
        if (node is JsonValue value && value.TryGetValue<string>(out var text))
            return text;
        return node?.ToJsonString(CompactJson) ?? string.Empty;
    }

    private static string Cell(string value) => Sanitize(value).Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');

    private static string Sanitize(string value)
    {
        value = value.Replace("\u001b", string.Empty, StringComparison.Ordinal);
        return new string(value.Where(character => character == '\t' || character == '\r' || character == '\n' || !char.IsControl(character)).ToArray());
    }

    private static JsonSerializerOptions CompactJson { get; } = new(JsonSupport.Options) { WriteIndented = false };
}
