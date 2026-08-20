// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("ArtifactStore centrally calculates contained deterministic paths")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactStorePaths-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ArtifactStore::EnsureLayout(library, diagnostic));
    CHECK(FileSystem::DirectoryExists(ArtifactStore::GetGcPath(library)));

    ArtifactTarget target;
    target.Platform = "Windows";
    target.Architecture = "x64";
    target.Graphics = "DX12";
    target.Role = "Editor";
    const Guid assetId(1, 2, 3, 4);
    const ArtifactKey key(ContentHash::Compute("artifact-a", 10));
    ArtifactStoragePath first;
    const bool firstFailed = ArtifactStore::TryGetArtifactPath(library, target, ArtifactTargetDimension::Platform | ArtifactTargetDimension::Graphics,
        assetId, StringAnsiView("Runtime/Main"), key, StringAnsiView(".flax"), first, diagnostic);
    REQUIRE_FALSE(firstFailed);
    CHECK(AssetPathPolicy::IsSameOrChild(first.Get(), library));
    CHECK(first.Get().EndsWith(TEXT(".flax")));
    CHECK_FALSE(first.Get().Contains(TEXT("Runtime/Main")));
    ArtifactStoragePath repeated;
    REQUIRE_FALSE(ArtifactStore::TryGetArtifactPath(library, target, ArtifactTargetDimension::Platform | ArtifactTargetDimension::Graphics,
        assetId, StringAnsiView("Runtime/Main"), key, StringAnsiView(".flax"), repeated, diagnostic));
    CHECK(repeated.Get() == first.Get());

    const ArtifactKey otherKey(ContentHash::Compute("artifact-b", 10));
    ArtifactStoragePath different;
    REQUIRE_FALSE(ArtifactStore::TryGetArtifactPath(library, target, ArtifactTargetDimension::Platform | ArtifactTargetDimension::Graphics,
        assetId, StringAnsiView("Runtime/Main"), otherKey, StringAnsiView(".flax"), different, diagnostic));
    CHECK(different.Get() != first.Get());

    String relative;
    REQUIRE_FALSE(ArtifactStore::TryMakeLibraryRelative(library, first.Get(), relative, diagnostic));
    CHECK_FALSE(relative.StartsWith('/'));
    ArtifactStoragePath resolved;
    REQUIRE_FALSE(ArtifactStore::TryResolveLibraryRelative(library, relative, resolved, diagnostic));
    CHECK(resolved.Get() == first.Get());
    CHECK(ArtifactStore::TryResolveLibraryRelative(library, TEXT("../Content/escape.flax"), resolved, diagnostic));
}

TEST_CASE("ArtifactStore rejects unsafe extensions and bounds long identifiers")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactStoreLongPaths-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ArtifactStore::EnsureLayout(library, diagnostic));
    ArtifactTarget target;
    target.Platform = "Windows";
    target.Role = "Editor";
    StringAnsi longKind;
    for (int32 i = 0; i < 1000; i++)
        longKind += 'x';
    ArtifactStoragePath path;
    const bool longFailed = ArtifactStore::TryGetArtifactPath(library, target, ArtifactTargetDimension::Platform,
        Guid(1, 2, 3, 4), longKind, ArtifactKey(ContentHash::Compute("key", 3)), StringAnsiView(".bin"), path, diagnostic);
    REQUIRE_FALSE(longFailed);
    CHECK(path.Get().Length() < library.Length() + 240);
    CHECK(ArtifactStore::TryGetArtifactPath(library, target, ArtifactTargetDimension::Platform,
        Guid(1, 2, 3, 4), longKind, ArtifactKey(ContentHash::Compute("key", 3)), StringAnsiView("../bad"), path, diagnostic));
}
