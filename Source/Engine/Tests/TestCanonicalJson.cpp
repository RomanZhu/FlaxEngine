// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Canonical JSON is sorted and byte idempotent")
{
    const StringAnsi input = "{\"unknown\":{\"z\":1,\"a\":2},\"b\":true,\"a\":1.25}";
    const StringAnsi expected =
        "{\n"
        "  \"a\": 1.25,\n"
        "  \"b\": true,\n"
        "  \"unknown\": {\n"
        "    \"a\": 2,\n"
        "    \"z\": 1\n"
        "  }\n"
        "}\n";
    CanonicalJsonError error;
    StringAnsi first;
    StringAnsi second;
    REQUIRE_FALSE(CanonicalJsonWriter::Canonicalize(input, first, error));
    CHECK(first == expected);
    REQUIRE_FALSE(CanonicalJsonWriter::Canonicalize(first, second, error));
    CHECK(second == first);

    StringAnsi reordered;
    REQUIRE_FALSE(CanonicalJsonWriter::Canonicalize("{\"a\":1.25,\"unknown\":{\"a\":2,\"z\":1},\"b\":true}", reordered, error));
    CHECK(reordered == first);
}

TEST_CASE("Canonical JSON preserves explicit root schema order")
{
    Array<StringAnsi> order;
    order.Add("metaVersion");
    order.Add("guid");
    CanonicalJsonError error;
    StringAnsi output;
    REQUIRE_FALSE(CanonicalJsonWriter::Canonicalize("{\"extra\":1,\"guid\":\"b\",\"metaVersion\":1}", output, error, &order));
    CHECK(output.StartsWith("{\n  \"metaVersion\": 1,\n  \"guid\": \"b\","));
    CHECK(output.EndsWith("\n"));
}

TEST_CASE("Canonical JSON rejects duplicate keys and non finite values")
{
    CanonicalJsonError error;
    StringAnsi output;
    CHECK(CanonicalJsonWriter::Canonicalize("{\"a\":1,\"a\":2}", output, error));
    CHECK(error.Code == CanonicalJsonErrorCode::DuplicateKey);
    CHECK(CanonicalJsonWriter::Canonicalize("{\"value\":NaN}", output, error));
    CHECK((error.Code == CanonicalJsonErrorCode::ParseError || error.Code == CanonicalJsonErrorCode::NonFiniteNumber));
}
