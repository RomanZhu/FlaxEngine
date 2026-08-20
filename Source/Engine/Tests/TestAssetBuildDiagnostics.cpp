// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Build/AssetBuildDiagnostics.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("AssetBuildDiagnostics canonical records retain stable actionable fields")
{
    AssetPipelineDiagnostic input;
    input.Code = AssetPipelineDiagnosticCode::BuildFailed;
    input.Severity = AssetPipelineDiagnosticSeverity::Warning;
    input.Stage = AssetPipelineDiagnosticStage::Publication;
    input.AssetGuid = Guid::New();
    input.SourcePath = TEXT("Content/Source.asset");
    input.ProcessorId = TEXT("Flax.Test");
    input.Target = TEXT("Windows-x64-Editor");
    input.OutputKind = TEXT("Runtime");
    input.Location.File = TEXT("Content/Source.asset.meta");
    input.Location.Line = 4;
    input.Location.Column = 8;
    input.Location.GraphNode = TEXT("NodeA");
    input.Message = TEXT("Processor failed.");
    input.Remediation = TEXT("Inspect the processor log.");
    input.Related.Add(TEXT("Content/Dependency.asset"));
    StringAnsi json;
    AssetPipelineDiagnostic error;
    REQUIRE_FALSE(AssetBuildDiagnostics::DiagnosticToJson(input, json, error));
    CHECK(json.Contains("ASSET_BUILD_FAILED"));
    AssetPipelineDiagnostic parsed;
    REQUIRE_FALSE(AssetBuildDiagnostics::DiagnosticFromJson(json, parsed, error));
    CHECK(parsed.Code == input.Code);
    CHECK(parsed.Severity == input.Severity);
    CHECK(parsed.Stage == input.Stage);
    CHECK(parsed.AssetGuid == input.AssetGuid);
    CHECK(parsed.ProcessorId == input.ProcessorId);
    CHECK(parsed.OutputKind == input.OutputKind);
    CHECK(parsed.Location.Line == 4);
    CHECK(parsed.Related == input.Related);
    CHECK(AssetBuildDiagnostics::DiagnosticFromJson(StringAnsiView("{}"), parsed, error));
}

TEST_CASE("AssetBuildDiagnostics explains typed key changes without persisting absolute paths")
{
    ArtifactKeyComponent source;
    source.Name = "source";
    source.Type = "content-hash";
    source.Value = "old";
    ArtifactKeyComponent path;
    path.Name = "debug-path";
    path.Type = "string";
    path.Value = "C:\\Private\\Source.asset";
    Array<ArtifactKeyComponent> previous;
    previous.Add(source);
    previous.Add(path);
    source.Value = "new";
    ArtifactKeyComponent tool;
    tool.Name = "tool";
    tool.Type = "semantic-version";
    tool.Value = "2";
    Array<ArtifactKeyComponent> current;
    current.Add(source);
    current.Add(tool);
    Array<AssetKeyDifference> differences;
    AssetBuildDiagnostics::DiffKeyComponents(previous, current, differences);
    REQUIRE(differences.Count() == 3);
    CHECK(differences[0].Name == "source");
    CHECK(differences[0].PreviousValue == "old");
    CHECK(differences[0].CurrentValue == "new");
    CHECK(differences[1].PreviousValue == "<absolute-path-redacted>");
    CHECK(differences[2].Name == "tool");
}

#endif
