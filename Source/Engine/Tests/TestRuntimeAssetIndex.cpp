// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/RuntimeAssetIndex.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Runtime asset index is deterministic and never refers to Library")
{
    Array<RuntimeAssetIndexEntry> entries;
    RuntimeAssetIndexEntry first;
    first.ID = AssetObjectId(Guid(2, 0, 0, 0), 1);
    first.BackingAssetID = Guid(20, 0, 0, 0);
    first.TypeName = TEXT("FlaxEngine.Texture");
    first.CanonicalPath = TEXT("Content/Textures/First.png");
    first.PackagedPath = TEXT("Content/Data_1.flax");
    first.PackageID = Guid(200, 0, 0, 0);
    first.Offset = 128;
    first.Size = 1024;
    first.AssetFormatVersion = 4;
    first.Flags = RuntimeAssetIndexFlags::ExactArtifact;
    first.ExactArtifact = ArtifactKey(ContentHash::Compute("first", 5));
    RuntimeAssetIndexEntry second;
    second.ID = AssetObjectId(Guid(1, 0, 0, 0), 42);
    second.BackingAssetID = Guid(10, 0, 0, 0);
    second.TypeName = TEXT("FlaxEngine.Material");
    second.CanonicalPath = TEXT("Content/Materials/Second.json");
    second.PackagedPath = TEXT("Content/Data_0.flax");
    second.PackageID = Guid(100, 0, 0, 0);
    second.ChunkID = 1;
    second.Offset = 2048;
    second.Size = 4096;
    second.AssetFormatVersion = 1;
    second.Flags = RuntimeAssetIndexFlags::ExactArtifact;
    second.ExactArtifact = ArtifactKey(ContentHash::Compute("second", 6));
    first.Dependencies.Add(second.ID);
    first.PreloadBudgetBytes = RuntimeAssetIndex::DefaultPreloadBudgetBytes;
    RuntimeAssetPreload preload;
    preload.ID = second.ID;
    preload.Priority = MAX_uint32 - 1;
    preload.EstimatedBytes = second.Size;
    preload.Required = true;
    first.Preload.Add(preload);
    entries.Add(first);
    entries.Add(second);

    CHECK_FALSE(RuntimeAssetIndex::ContainsLibraryPath(TEXT("Content/Data_0.flax")));
    CHECK(RuntimeAssetIndex::ContainsLibraryPath(TEXT("C:/Project/Library/Artifacts/ab/cd.flax")));
    CHECK(RuntimeAssetIndex::ContainsLibraryPath(TEXT("library/temp/preview.flax")));

    AssetPipelineDiagnostic diagnostic;
    StringAnsi json;
    REQUIRE_FALSE(RuntimeAssetIndex::WriteCanonicalJson(entries, json, diagnostic));
    StringAnsi again;
    REQUIRE_FALSE(RuntimeAssetIndex::WriteCanonicalJson(entries, again, diagnostic));
    CHECK(json == again);
    CHECK(json.Contains("\"formatVersion\": 5"));
    CHECK(json.Contains("\"contentHash\":"));
    CHECK(json.Contains("00000001000000000000000000000000:42"));
    CHECK(json.Contains("FlaxEngine.Material"));
    CHECK(json.Contains("FlaxEngine.Texture"));
    CHECK_FALSE(json.Contains("Library"));
    Array<RuntimeAssetIndexEntry> parsed;
    REQUIRE_FALSE(RuntimeAssetIndex::Parse(json, parsed, diagnostic));
    REQUIRE(parsed.Count() == 2);
    CHECK(parsed[0].ID.Guid == second.ID.Guid);
    CHECK(parsed[1].ExactArtifact == first.ExactArtifact);
    REQUIRE(parsed[1].Dependencies.Count() == 1);
    CHECK(parsed[1].Dependencies[0] == second.ID);
    REQUIRE(parsed[1].Preload.Count() == 1);
    CHECK(parsed[1].Preload[0].Required);
    CHECK(parsed[1].Preload[0].EstimatedBytes == second.Size);

    RuntimeAssetIndexEntry libraryEntry;
    libraryEntry.ID = AssetObjectId(Guid(3, 0, 0, 0), 1);
    libraryEntry.BackingAssetID = Guid(30, 0, 0, 0);
    libraryEntry.TypeName = TEXT("FlaxEngine.Texture");
    libraryEntry.PackagedPath = TEXT("Library/Artifacts/runtime.flax");
    libraryEntry.PackageID = Guid(300, 0, 0, 0);
    libraryEntry.Size = 1;
    entries.Add(libraryEntry);
    CHECK(RuntimeAssetIndex::WriteCanonicalJson(entries, json, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
}
