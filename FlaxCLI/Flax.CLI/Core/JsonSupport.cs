// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using System.Text.Json.Serialization;

namespace Flax.CLI.Core;

internal static class JsonSupport
{
    public static JsonSerializerOptions Options { get; } = Create();

    private static JsonSerializerOptions Create()
    {
        var result = new JsonSerializerOptions
        {
            AllowTrailingCommas = true,
            PropertyNameCaseInsensitive = true,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            WriteIndented = true,
        };
        result.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        return result;
    }
}
