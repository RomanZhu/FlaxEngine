// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Config.h"

#if COMPILE_WITH_ASSETS_IMPORTER

#include "Engine/Content/Build/LegacyImporterAdapter.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    struct LegacyFixtureArgument
    {
        String ForbiddenContentTarget;
        int32 Quality = 0;
    };

    CreateAssetResult HarmlessLegacyImporter(CreateAssetContext& context)
    {
        const LegacyFixtureArgument* options = static_cast<const LegacyFixtureArgument*>(context.CustomArg);
        if (!options || options->Quality != 7)
            return CreateAssetResult::Error;
        Array<byte> input;
        if (File::ReadAllBytes(context.InputPath, input) || context.AllocateChunk(0))
            return CreateAssetResult::InvalidPath;
        context.Data.Header.TypeName = TEXT("Tests.LegacyArtifact");
        context.Data.SerializedVersion = 3;
        context.Data.Header.Chunks[0]->Data.Copy(input);
        const char forbiddenMetadata[] = "{\"ImportPath\":\"forbidden\",\"ImportUsername\":\"forbidden\"}";
        context.Data.Metadata.Copy(reinterpret_cast<const byte*>(forbiddenMetadata), ARRAY_COUNT(forbiddenMetadata) - 1);
        // Artifact mode must ignore a legacy callback's attempt to redirect its target.
        context.TargetAssetPath = options->ForbiddenContentTarget;
        return CreateAssetResult::Ok;
    }
}

TEST_CASE("LegacyImporterAdapter produces a GUID-stable artifact without Content mutation")
{
    const String root = Globals::TemporaryFolder / (TEXT("LegacyImporterAdapter-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = content / TEXT("source.legacytest");
    const String forbiddenTarget = content / TEXT("must-not-exist.flax");
    const byte sourceBytes[] = { 1, 3, 3, 7 };
    REQUIRE_FALSE(File::WriteAllBytes(source, sourceBytes, ARRAY_COUNT(sourceBytes)));

    const Guid intendedId(10, 20, 30, 40);
    PreparedAsset prepared;
    prepared.AssetID = intendedId;
    prepared.DatabaseRevision = 4;
    AssetDependency sourceDependency;
    sourceDependency.Kind = AssetDependencyKind::SourceFile;
    sourceDependency.StableIdentity = TEXT("Content/source.legacytest");
    sourceDependency.Content = ContentHash::Compute(sourceBytes, ARRAY_COUNT(sourceBytes));
    prepared.Dependencies.Add(sourceDependency);
    DeclaredArtifactOutput output;
    output.Kind = "runtime";
    output.Extension = ".flax";
    output.EffectiveAssetID = intendedId;
    prepared.Outputs.Add(output);
    ArtifactBuildInput input;
    input.StableIdentity = sourceDependency.StableIdentity;
    input.Path = source;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    AssetCancellationSource cancellation;
    ArtifactBuildContext buildContext(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(buildContext.Initialize(diagnostic));
    LegacyFixtureArgument options;
    options.ForbiddenContentTarget = forbiddenTarget;
    options.Quality = 7;
    CreateAssetFunction callback = &HarmlessLegacyImporter;
    REQUIRE_FALSE(LegacyImporterAdapter::Build(callback, buildContext, TEXT("Content/source.legacytest"), StringAnsiView("runtime"),
        TEXT("artifact.flax"), intendedId, TEXT("Tests.LegacyArtifact"), &options, diagnostic));
    REQUIRE_FALSE(buildContext.Close(diagnostic));
    CHECK_FALSE(FileSystem::FileExists(forbiddenTarget));
    REQUIRE(buildContext.GetFiles().Count() == 1);

    const String artifactPath = buildContext.GetFiles()[0].AbsolutePath;
    auto storage = ContentStorageManager::GetStorage(artifactPath);
    REQUIRE(storage);
    Array<FlaxStorage::Entry> entries;
    storage->GetEntries(entries);
    REQUIRE(entries.Count() == 1);
    CHECK(entries[0].ID == intendedId);
    CHECK(entries[0].TypeName == TEXT("Tests.LegacyArtifact"));
    AssetInitData data;
    REQUIRE_FALSE(storage->LoadAssetHeader(intendedId, data));
    CHECK(data.Metadata.IsInvalid());
    REQUIRE(data.Header.Chunks[0]);
    REQUIRE_FALSE(storage->LoadAssetChunk(data.Header.Chunks[0]));
    CHECK(data.Header.Chunks[0]->Data.Length() == ARRAY_COUNT(sourceBytes));
    storage = nullptr;
    ContentStorageManager::EnsureAccess(artifactPath);
}

#endif
