// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Artifacts/ArtifactManifest.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    ArtifactManifest ValidManifest()
    {
        ArtifactManifest manifest;
        manifest.AssetID = Guid(1, 2, 3, 4);
        manifest.DatabaseRevision = 42;
        manifest.ProcessorID = TEXT("Tests.Manifest");
        manifest.ProcessorImplementationVersion = 3;
        manifest.Target.Platform = "Windows";
        manifest.Target.Architecture = "x64";
        manifest.Target.Graphics = "DX12";
        manifest.Target.Configuration = "Development";
        manifest.Target.Quality = "Default";
        manifest.Target.Role = "Editor";
        manifest.Target.FeatureFlags.Add("raytracing");
        manifest.InputFingerprint = ArtifactKey(ContentHash::Compute("input", 5));
        manifest.SourceHash = ContentHash::Compute("source", 6);
        manifest.SettingsHash = ContentHash::Compute("settings", 8);
        ArtifactManifestDependency dependency;
        dependency.Kind = AssetDependencyKind::SourceFile;
        dependency.Identity = TEXT("Content/source.synthetic");
        dependency.Hash = manifest.SourceHash;
        dependency.Origin = TEXT("main source");
        manifest.Dependencies.Add(dependency);
        ArtifactManifestOutput output;
        output.Kind = "runtime";
        output.FormatVersion = 2;
        output.Key = ArtifactKey(ContentHash::Compute("artifact", 8));
        output.RelativePath = TEXT("Artifacts/Editor-Windows/asset/runtime/artifact.flax");
        output.Content = ContentHash::Compute("bytes", 5);
        output.Size = 5;
        output.Compatibility = "Tests.Runtime.v2";
        manifest.Outputs.Add(output);
        manifest.BuildID = TEXT("asset-build-1");
        manifest.BuiltAtUtc = TEXT("2026-08-20T12:00:00Z");
        ArtifactKeyComponent component;
        component.Name = "source";
        component.Type = "content-hash";
        component.Value = manifest.SourceHash.ToString();
        manifest.KeyComponents.Add(component);
        return manifest;
    }
}

TEST_CASE("ArtifactManifest canonical JSON round-trips coherently")
{
    ArtifactManifest manifest = ValidManifest();
    AssetPipelineDiagnostic diagnostic;
    StringAnsi first;
    REQUIRE_FALSE(manifest.ToJson(first, diagnostic));
    CHECK(first.EndsWith("\n"));
    CHECK(first.Contains("\"manifestVersion\": 1"));
    CHECK(first.Contains("\"relativePath\": \"Artifacts/"));

    ArtifactManifest parsed;
    REQUIRE_FALSE(ArtifactManifest::Parse(first, TEXT("manifest.json"), parsed, diagnostic));
    CHECK(parsed.AssetID == manifest.AssetID);
    CHECK(parsed.InputFingerprint == manifest.InputFingerprint);
    REQUIRE(parsed.Outputs.Count() == 1);
    CHECK(parsed.Outputs[0].Content == manifest.Outputs[0].Content);
    StringAnsi second;
    REQUIRE_FALSE(parsed.ToJson(second, diagnostic));
    CHECK(second == first);
}

TEST_CASE("ArtifactManifest rejects traversal duplicates and missing fields")
{
    AssetPipelineDiagnostic diagnostic;
    ArtifactManifest manifest = ValidManifest();
    manifest.Outputs[0].RelativePath = TEXT("../escape.flax");
    StringAnsi json;
    CHECK(manifest.ToJson(json, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);

    manifest = ValidManifest();
    manifest.Outputs.Add(manifest.Outputs[0]);
    CHECK(manifest.ToJson(json, diagnostic));

    const StringAnsi missing = "{\"manifestVersion\":1}";
    ArtifactManifest parsed;
    CHECK(ArtifactManifest::Parse(missing, TEXT("missing.json"), parsed, diagnostic));
    const StringAnsi duplicate = "{\"manifestVersion\":1,\"manifestVersion\":1}";
    CHECK(ArtifactManifest::Parse(duplicate, TEXT("duplicate.json"), parsed, diagnostic));
}

TEST_CASE("ArtifactManifest parser rejects malformed field shapes")
{
    const char* invalid[] =
    {
        "[]",
        "not-json",
        "{\"manifestVersion\":\"1\"}",
        "{\"manifestVersion\":1,\"assetGuid\":null}",
        "{\"manifestVersion\":1,\"assetGuid\":\"xyz\"}",
    };
    AssetPipelineDiagnostic diagnostic;
    ArtifactManifest parsed;
    for (const char* value : invalid)
        CHECK(ArtifactManifest::Parse(StringAnsiView(value), TEXT("fuzz.json"), parsed, diagnostic));
}
