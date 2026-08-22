// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetPipeline/AssetPipelineSettings.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Asset pipeline rollout settings")
{
    AssetPipelineSettings settings;
    AssetPipelineDiagnostic diagnostic;

    SECTION("Legacy defaults are valid and disabled")
    {
        CHECK(settings.IsValid(diagnostic));
        CHECK_FALSE(settings.UseNewAssetDatabase);
        CHECK_FALSE(settings.UseLibraryArtifacts);
        CHECK_FALSE(settings.UseTextGraphAssets);
        CHECK_FALSE(settings.StrictAssetMetadata);
        CHECK_FALSE(settings.AutoCreateMetaInEditor);
        CHECK_FALSE(settings.AllowLastGoodArtifacts);
        CHECK_FALSE(settings.LockConvertedTypeAuthoring);
    }

    SECTION("Dependent flags reject unsupported combinations")
    {
        settings.UseLibraryArtifacts = true;
        CHECK_FALSE(settings.IsValid(diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);
        CHECK(String(GetAssetPipelineDiagnosticCodeName(diagnostic.Code)) == TEXT("ASSET_INVALID_SETTINGS_COMBINATION"));

        settings = AssetPipelineSettings();
        settings.UseTextGraphAssets = true;
        CHECK_FALSE(settings.IsValid(diagnostic));

        settings = AssetPipelineSettings();
        settings.StrictAssetMetadata = true;
        CHECK_FALSE(settings.IsValid(diagnostic));

        settings = AssetPipelineSettings();
        settings.AutoCreateMetaInEditor = true;
        CHECK_FALSE(settings.IsValid(diagnostic));

        settings = AssetPipelineSettings();
        settings.AllowLastGoodArtifacts = true;
        CHECK_FALSE(settings.IsValid(diagnostic));

        settings = AssetPipelineSettings();
        settings.LockConvertedTypeAuthoring = true;
        CHECK_FALSE(settings.IsValid(diagnostic));
    }

    SECTION("Fully enabled rollout is valid")
    {
        settings.UseNewAssetDatabase = true;
        settings.UseLibraryArtifacts = true;
        settings.UseTextGraphAssets = true;
        settings.StrictAssetMetadata = true;
        settings.AutoCreateMetaInEditor = true;
        settings.AllowLastGoodArtifacts = true;
        settings.LockConvertedTypeAuthoring = true;
        CHECK(settings.IsValid(diagnostic));
    }
}

TEST_CASE("Asset pipeline settings serialize")
{
    AssetPipelineSettings settings;
    settings.UseNewAssetDatabase = true;
    settings.UseLibraryArtifacts = true;
    settings.AllowLastGoodArtifacts = true;
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
    CHECK(loaded.UseNewAssetDatabase);
    CHECK(loaded.UseLibraryArtifacts);
    CHECK(loaded.AllowLastGoodArtifacts);
    CHECK(loaded.DiskQuotaGigabytes == 77);
    CHECK(loaded.WorkerLimit == 6);
}
