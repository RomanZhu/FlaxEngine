// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json.Nodes;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal sealed class ConfigStore(AppPaths paths)
{
    public JsonObject Load(string? projectFile = null)
    {
        var result = Read(paths.ConfigFile);
        if (projectFile != null)
            Merge(result, Read(projectFile));
        return result;
    }

    public string? GetString(string key, string? projectFile = null)
    {
        var node = GetNode(Load(projectFile), key);
        return node is JsonValue jsonValue && jsonValue.TryGetValue<string>(out var value) ? value : null;
    }

    public JsonNode? Get(string key, string? projectFile = null) => GetNode(Load(projectFile), key)?.DeepClone();

    public bool GetBoolean(string key, bool fallback = false, string? projectFile = null)
    {
        var node = GetNode(Load(projectFile), key);
        return node is JsonValue jsonValue && jsonValue.TryGetValue<bool>(out var value) ? value : fallback;
    }

    public JsonObject Set(string key, string? value, bool unset, string? projectFile = null)
    {
        var path = projectFile ?? paths.ConfigFile;
        var root = Read(path);
        var segments = key.Split('.', StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length == 0)
            throw CommandLine.Usage("A configuration key is required.");
        JsonObject parent = root;
        for (var index = 0; index < segments.Length - 1; index++)
        {
            if (parent[segments[index]] is not JsonObject child)
            {
                child = new JsonObject();
                parent[segments[index]] = child;
            }
            parent = child;
        }
        if (unset)
            parent.Remove(segments[^1]);
        else
            parent[segments[^1]] = ParseValue(value!);
        AtomicFile.WriteText(path, root.ToJsonString(JsonSupport.Options) + Environment.NewLine);
        return root;
    }

    private static JsonNode ParseValue(string value)
    {
        if (bool.TryParse(value, out var boolean)) return JsonValue.Create(boolean)!;
        if (long.TryParse(value, out var integer)) return JsonValue.Create(integer)!;
        return JsonValue.Create(value)!;
    }

    private static JsonObject Read(string path)
    {
        if (!File.Exists(path))
            return new JsonObject();
        try
        {
            return JsonNode.Parse(File.ReadAllText(path), documentOptions: new() { AllowTrailingCommas = true, CommentHandling = System.Text.Json.JsonCommentHandling.Skip }) as JsonObject ?? new JsonObject();
        }
        catch (System.Text.Json.JsonException ex)
        {
            throw new CliException(ExitCode.ContextRequired, "FLX-CONFIG-0004", $"Configuration file '{path}' is invalid.", new { exception = ex.Message });
        }
    }

    private static JsonNode? GetNode(JsonObject root, string key)
    {
        JsonNode? current = root;
        foreach (var segment in key.Split('.', StringSplitOptions.RemoveEmptyEntries))
        {
            if (current is not JsonObject value || !value.TryGetPropertyValue(segment, out current))
                return null;
        }
        return current;
    }

    private static void Merge(JsonObject target, JsonObject source)
    {
        foreach (var pair in source)
        {
            if (pair.Value is JsonObject sourceObject)
            {
                if (target[pair.Key] is not JsonObject targetObject)
                {
                    targetObject = new JsonObject();
                    target[pair.Key] = targetObject;
                }
                Merge(targetObject, sourceObject);
            }
            else
            {
                target[pair.Key] = pair.Value?.DeepClone();
            }
        }
    }
}
