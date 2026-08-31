// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/Identity/AssetIdentitySerialization.h"
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

TEST_CASE("Asset object identifiers use structured canonical JSON")
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
