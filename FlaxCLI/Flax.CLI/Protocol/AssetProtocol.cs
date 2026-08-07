// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json.Nodes;

namespace Flax.CLI.Protocol;

internal sealed class AssetRequest
{
    public int SchemaVersion { get; set; } = 1;
    public string Operation { get; set; } = "asset";
    public required string RequestId { get; set; }
    public required string ProjectPath { get; set; }
    public required AssetRequestOptions Asset { get; set; }
    public required string EventPath { get; set; }
    public required string ResultPath { get; set; }
}

internal sealed class AssetRequestOptions
{
    public required string Action { get; set; }
    public string? Path { get; set; }
    public string? Destination { get; set; }
    public string[]? Sources { get; set; }
    public string? AssetType { get; set; }
    public string? PropertyPath { get; set; }
    public JsonNode? Value { get; set; }
    public bool Recursive { get; set; }
    public bool Force { get; set; }
    public bool Save { get; set; } = true;
}
