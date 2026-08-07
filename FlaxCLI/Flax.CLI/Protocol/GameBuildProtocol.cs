// Copyright (c) Wojciech Figat. All rights reserved.

namespace Flax.CLI.Protocol;

internal sealed class GameBuildRequest
{
    public int SchemaVersion { get; set; } = 1;
    public string Operation { get; set; } = "build";
    public required string RequestId { get; set; }
    public required string ProjectPath { get; set; }
    public required string Preset { get; set; }
    public required string Target { get; set; }
    public string? OutputPath { get; set; }
    public string[]? CustomDefines { get; set; }
    public GameBuildRequestOptions Options { get; set; } = new();
    public required string EventPath { get; set; }
    public required string ResultPath { get; set; }
}

internal sealed class GameBuildRequestOptions
{
    public bool Clean { get; set; }
    public bool RunAfterBuild { get; set; }
}
