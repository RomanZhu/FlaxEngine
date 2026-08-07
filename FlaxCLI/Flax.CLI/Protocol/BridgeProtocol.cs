// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json.Serialization;

namespace Flax.CLI.Protocol;

internal sealed class EditorInstanceManifest
{
    public int SchemaVersion { get; set; }
    public required string InstanceId { get; set; }
    public int Pid { get; set; }
    public DateTime ProcessStartTimeUtc { get; set; }
    public required string Kind { get; set; }
    public required string ProjectPath { get; set; }
    public string? EngineVersion { get; set; }
    public string? EngineNickname { get; set; }
    public int ProtocolVersion { get; set; }
    public required string Transport { get; set; }
    public required string Endpoint { get; set; }
    public required string TokenPath { get; set; }
    public string? State { get; set; }
    public string[] Capabilities { get; set; } = [];

    [JsonIgnore]
    public string ManifestPath { get; set; } = string.Empty;
}

internal sealed class EditorBridgeRequest
{
    public int SchemaVersion { get; set; } = 1;
    public required string RequestId { get; set; }
    public required string Token { get; set; }
    public required string Action { get; set; }
    public string? Name { get; set; }
    public System.Text.Json.Nodes.JsonObject Arguments { get; set; } = new();
    public bool Confirm { get; set; }
    public double TimeoutSeconds { get; set; } = 30;
}

internal sealed record EditorBridgeInvocation(EditorInstanceManifest Instance, System.Text.Json.JsonElement Response);
