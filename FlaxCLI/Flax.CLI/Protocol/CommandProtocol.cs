// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json.Nodes;

namespace Flax.CLI.Protocol;

internal sealed class EditorCommandRequest
{
    public int SchemaVersion { get; set; } = 1;
    public string Operation { get; set; } = "command";
    public required string RequestId { get; set; }
    public required string ProjectPath { get; set; }
    public required EditorCommandRequestOptions Command { get; set; }
    public required string EventPath { get; set; }
    public required string ResultPath { get; set; }
}

internal sealed class EditorCommandRequestOptions
{
    public required string Action { get; set; }
    public string? Name { get; set; }
    public JsonObject Arguments { get; set; } = new();
    public bool Confirm { get; set; }
}
