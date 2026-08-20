// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetRecord.h"
#include "Engine/Engine/Globals.h"
#include <ThirdParty/catch2/catch.hpp>
#include <type_traits>

static_assert(!std::is_constructible<CanonicalAssetPath, ArtifactStoragePath>::value, "Artifact paths must not implicitly become canonical paths.");
static_assert(!std::is_constructible<SourceFilePath, CanonicalAssetPath>::value, "Canonical and source path conversions must be explicit.");
static_assert(!std::is_convertible<ArtifactStoragePath, String>::value, "Artifact paths must be unwrapped explicitly.");

TEST_CASE("Asset semantic path policy")
{
    const String content = Globals::ProjectContentFolder;
    const String library = Globals::ProjectLibraryFolder;

    const CanonicalAssetPath canonical(content / TEXT("Textures/Test.png"));
    const SourceFilePath source(content / TEXT("Textures/Test.png"));
    const MetaFilePath meta(content / TEXT("Textures/Test.png.meta"));
    const ArtifactStoragePath artifact(library / TEXT("Artifacts/aa/test.flax"));
    CHECK(AssetPathPolicy::IsCanonicalPathValid(canonical, content));
    CHECK(AssetPathPolicy::IsSourcePathValid(source, content));
    CHECK(AssetPathPolicy::IsMetaPathValid(meta, content));
    CHECK(AssetPathPolicy::IsArtifactPathValid(artifact, library));
    CHECK_FALSE(AssetPathPolicy::IsCanonicalPathValid(CanonicalAssetPath(artifact.Get()), content));

    CHECK(AssetPathPolicy::IsPackageEntryPathValid(PackageEntryPath(TEXT("Content/Data_0.flaxpac"))));
    CHECK_FALSE(AssetPathPolicy::IsPackageEntryPathValid(PackageEntryPath(TEXT("../Content/Data.flaxpac"))));
    CHECK(String(AssetPathPolicy::GetDebugLabel(AssetPathKind::ArtifactStorage)) == TEXT("artifact-storage"));
}

TEST_CASE("Legacy AssetRecord adapter preserves identity")
{
    const AssetInfo info(Guid::New(), TEXT("FlaxEngine.Texture"), Globals::ProjectContentFolder / TEXT("Legacy.flax"));
    const AssetRecord record = AssetRecord::FromLegacy(info);
    CHECK(record.ID == info.ID);
    CHECK(record.SourceAssetID == info.ID);
    CHECK(record.SourceKind == AssetSourceKind::LegacyBinary);
    CHECK(record.IsMainAsset());

    const AssetInfo adapted = record.ToAssetInfo();
    CHECK(adapted.ID == info.ID);
    CHECK(adapted.TypeName == info.TypeName);
    CHECK(adapted.Path == info.Path);
}

TEST_CASE("Project asset paths normalize portably and reject escapes")
{
    AssetPathPolicy::ProjectPath normalized;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, TEXT("Content/Textures/../Textures/Caf\u00e9.png"), normalized, diagnostic));
    CHECK(normalized.ProjectRelativePath == TEXT("Content/Textures/Caf\u00e9.png"));
    CHECK(normalized.PortabilityKey == TEXT("content/textures/caf\u00e9.png"));
    CHECK(normalized.DisplayPath == TEXT("Content/Textures/../Textures/Caf\u00e9.png"));

    CHECK(AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, TEXT("Content/../Library/escape.bin"), normalized, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PathCollision);
    CHECK(AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, TEXT("Content/CON.png"), normalized, diagnostic));

    AssetPathPolicy::ProjectPath first;
    AssetPathPolicy::ProjectPath second;
    REQUIRE_FALSE(AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, TEXT("Content/Textures/Stone.png"), first, diagnostic));
    REQUIRE_FALSE(AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, TEXT("Content/textures/stone.png"), second, diagnostic));
    Array<AssetPathPolicy::ProjectPath> paths;
    paths.Add(first);
    paths.Add(second);
    Array<AssetPipelineDiagnostic> diagnostics;
    AssetPathPolicy::FindPortabilityCollisions(paths, diagnostics);
    REQUIRE(diagnostics.Count() == 1);
    CHECK(diagnostics[0].Related.Count() == 2);
}
