// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Builtin/BuiltinAssetCatalogFormat.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Built-in catalog format persists object GUID identity")
{
    BuiltinAssetCatalogSerializedEntry entry;
    entry.ID = Guid(1, 2, 3, 4);
    entry.TypeName = TEXT("FlaxEngine.Texture");
    entry.RelativePath = TEXT("Engine/Test.flax");
    entry.Uri = TEXT("builtin://Engine/Test#42");
    Array<BuiltinAssetCatalogSerializedEntry> entries;
    entries.Add(entry);

    AssetPipelineDiagnostic diagnostic;
    Array<byte> bytes;
    REQUIRE_FALSE(BuiltinAssetCatalogFormat::ToBytes(entries, bytes, diagnostic));
    REQUIRE(bytes.Count() >= 8);
    CHECK(bytes[4] == BuiltinAssetCatalogFormat::Version);
    CHECK(bytes[5] == 0);
    CHECK(bytes[6] == 0);
    CHECK(bytes[7] == 0);

    const int32 headerSize = sizeof(uint32) * 3 + sizeof(ContentHash);
    const int32 payloadSize = sizeof(uint32) + sizeof(Guid) + sizeof(uint32) * 3 +
        StringAnsi(entry.TypeName).Length() + StringAnsi(entry.RelativePath).Length() + StringAnsi(entry.Uri).Length();
    CHECK(bytes.Count() == headerSize + payloadSize);

    Array<BuiltinAssetCatalogSerializedEntry> loaded;
    REQUIRE_FALSE(BuiltinAssetCatalogFormat::FromBytes(Span<byte>(bytes.Get(), bytes.Count()), loaded, diagnostic));
    REQUIRE(loaded.Count() == 1);
    CHECK(loaded[0].ID == entry.ID);
    CHECK(loaded[0].TypeName == entry.TypeName);
    CHECK(loaded[0].RelativePath == entry.RelativePath);
    CHECK(loaded[0].Uri == entry.Uri);
}

TEST_CASE("Built-in catalog format rejects legacy runtime identity data")
{
    BuiltinAssetCatalogSerializedEntry entry;
    entry.ID = Guid(5, 6, 7, 8);
    entry.TypeName = TEXT("FlaxEngine.Material");
    entry.RelativePath = TEXT("Engine/Material.flax");
    entry.Uri = TEXT("builtin://Engine/Material");
    Array<BuiltinAssetCatalogSerializedEntry> entries;
    entries.Add(entry);

    AssetPipelineDiagnostic diagnostic;
    Array<byte> bytes;
    REQUIRE_FALSE(BuiltinAssetCatalogFormat::ToBytes(entries, bytes, diagnostic));
    bytes[4] = 1;
    CHECK(BuiltinAssetCatalogFormat::IsLegacyVersion(Span<byte>(bytes.Get(), bytes.Count())));

    Array<BuiltinAssetCatalogSerializedEntry> loaded;
    CHECK(BuiltinAssetCatalogFormat::FromBytes(Span<byte>(bytes.Get(), bytes.Count()), loaded, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
    CHECK(loaded.IsEmpty());
}

TEST_CASE("Built-in catalog format rejects duplicate object GUIDs")
{
    BuiltinAssetCatalogSerializedEntry entry;
    entry.ID = Guid(9, 10, 11, 12);
    entry.TypeName = TEXT("FlaxEngine.Texture");
    entry.RelativePath = TEXT("Engine/First.flax");
    entry.Uri = TEXT("builtin://Engine/First");
    Array<BuiltinAssetCatalogSerializedEntry> entries;
    entries.Add(entry);
    entry.RelativePath = TEXT("Engine/Second.flax");
    entry.Uri = TEXT("builtin://Engine/Second");
    entries.Add(entry);

    AssetPipelineDiagnostic diagnostic;
    Array<byte> bytes;
    CHECK(BuiltinAssetCatalogFormat::ToBytes(entries, bytes, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
    CHECK(bytes.IsEmpty());
}
