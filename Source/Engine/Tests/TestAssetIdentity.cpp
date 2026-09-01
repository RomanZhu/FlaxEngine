// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/Identity/AssetIdentitySerialization.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/SceneReference.h"
#include "Engine/Content/SoftAssetReference.h"
#include "Engine/Content/WeakAssetReference.h"
#include "Engine/Serialization/JsonWriters.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Asset object identifiers preserve source and local file identity")
{
    const AssetGuid source(Guid(0x01234567, 0x89abcdef, 0x01234567, 0x89abcdef));
    const AssetObjectId original(source, 18274497224001756LL);
    AssetObjectId parsed;
    REQUIRE_FALSE(AssetObjectId::Parse(original.ToString(), parsed));
    CHECK(parsed == original);
    CHECK_FALSE(parsed.IsMainObject());
    CHECK(AssetObjectId::Main(source).IsMainObject());
}

TEST_CASE("Default asset references have empty persistent identity")
{
    AssetReference<Asset> strong;
    AssetReference<Asset> strongNull(nullptr);
    SoftAssetReference<Asset> soft;
    SoftAssetReference<Asset> softNull(nullptr);
    WeakAssetReference<Asset> weak;
    WeakAssetReference<Asset> weakNull(nullptr);

    CHECK(strong.GetID() == Guid::Empty);
    CHECK(strongNull.GetID() == Guid::Empty);
    CHECK(soft.GetID() == Guid::Empty);
    CHECK(softNull.GetID() == Guid::Empty);
    CHECK(weak.GetID() == Guid::Empty);
    CHECK(weakNull.GetID() == Guid::Empty);
}

TEST_CASE("Private asset object identifiers preserve reconciliation identity")
{
    const AssetObjectId original(AssetGuid(Guid(0x01234567, 0x89abcdef, 0x01234567, 0x89abcdef)), 42);
    rapidjson_flax::StringBuffer buffer;
    CompactJsonWriter writer(buffer);
    Serialization::Serialize(writer, original, nullptr);
    CHECK(StringAnsiView(buffer.GetString()).Contains("\"guid\":\"0123456789abcdef0123456789abcdef\""));
    CHECK(StringAnsiView(buffer.GetString()).Contains("\"fileId\":42"));

    rapidjson_flax::Document document;
    document.Parse(buffer.GetString(), buffer.GetSize());
    REQUIRE_FALSE(document.HasParseError());
    AssetObjectId roundTrip;
    Serialization::Deserialize(document, roundTrip, nullptr);
    CHECK(roundTrip == original);
}

TEST_CASE("Public scene asset references use GUID-only JSON")
{
    SceneReference original;
    original.ID = Guid(0x01234567, 0x89abcdef, 0x01234567, 0x89abcdef);
    rapidjson_flax::StringBuffer buffer;
    CompactJsonWriter writer(buffer);
    Serialization::Serialize(writer, original, nullptr);
    CHECK(StringAnsiView(buffer.GetString()) == "\"0123456789abcdef0123456789abcdef\"");

    rapidjson_flax::Document document;
    document.Parse(buffer.GetString(), buffer.GetSize());
    SceneReference roundTrip;
    Serialization::Deserialize(document, roundTrip, nullptr);
    CHECK(roundTrip == original);

    document.Parse("{\"guid\":\"0123456789abcdef0123456789abcdef\",\"fileId\":1}");
    roundTrip.ID = original.ID;
    Serialization::Deserialize(document, roundTrip, nullptr);
    CHECK(roundTrip.ID == Guid::Empty);
}
