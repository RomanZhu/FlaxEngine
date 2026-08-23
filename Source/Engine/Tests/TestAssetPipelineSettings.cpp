// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetPipeline/AssetPipelineSettings.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Asset pipeline resource settings")
{
    AssetPipelineSettings settings;
    AssetPipelineDiagnostic diagnostic;

    SECTION("Defaults are valid")
    {
        CHECK(settings.IsValid(diagnostic));
    }

    SECTION("Invalid resource limits are rejected")
    {
        settings.DiskQuotaGigabytes = 0;
        CHECK_FALSE(settings.IsValid(diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);
        CHECK(String(GetAssetPipelineDiagnosticCodeName(diagnostic.Code)) == TEXT("ASSET_INVALID_SETTINGS_COMBINATION"));

        settings = AssetPipelineSettings();
        settings.MemoryLimitMegabytes = 127;
        CHECK_FALSE(settings.IsValid(diagnostic));

        settings = AssetPipelineSettings();
        settings.WorkerLimit = -1;
        CHECK_FALSE(settings.IsValid(diagnostic));
    }
}

TEST_CASE("Asset pipeline settings serialize")
{
    AssetPipelineSettings settings;
    settings.DiskQuotaGigabytes = 77;
    settings.WorkerLimit = 6;

    rapidjson_flax::StringBuffer buffer;
    CompactJsonWriter writer(buffer);
    writer.StartObject();
    settings.Serialize(writer, nullptr);
    writer.EndObject();

    rapidjson_flax::Document document;
    document.Parse(buffer.GetString(), buffer.GetSize());
    REQUIRE_FALSE(document.HasParseError());

    AssetPipelineSettings loaded;
    loaded.Deserialize(document, nullptr);
    CHECK(loaded.DiskQuotaGigabytes == 77);
    CHECK(loaded.WorkerLimit == 6);
}
