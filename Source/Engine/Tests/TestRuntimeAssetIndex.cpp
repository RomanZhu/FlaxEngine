// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/RuntimeAssetIndex.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Runtime asset index is deterministic and never refers to Library")
{
    Array<RuntimeAssetIndexEntry> entries;
    RuntimeAssetIndexEntry first;
    first.ID = Guid(2, 0, 0, 0);
    first.TypeName = TEXT("FlaxEngine.Texture");
    first.PackagedPath = TEXT("Content/Data_1.flax");
    entries.Add(first);
    RuntimeAssetIndexEntry second;
    second.ID = Guid(1, 0, 0, 0);
    second.TypeName = TEXT("FlaxEngine.Material");
    second.PackagedPath = TEXT("Content/Data_0.flax");
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
    CHECK(json.Contains("\"formatVersion\": 1"));
    CHECK(json.Contains("FlaxEngine.Material"));
    CHECK(json.Contains("FlaxEngine.Texture"));
    CHECK_FALSE(json.Contains("Library"));

    RuntimeAssetIndexEntry libraryEntry;
    libraryEntry.ID = Guid(3, 0, 0, 0);
    libraryEntry.TypeName = TEXT("FlaxEngine.Texture");
    libraryEntry.PackagedPath = TEXT("Library/Artifacts/runtime.flax");
    entries.Add(libraryEntry);
    CHECK(RuntimeAssetIndex::WriteCanonicalJson(entries, json, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
}
