// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    AssetMeta MakeMeta()
    {
        AssetMeta meta;
        meta.ID = Guid::New();
        meta.AssetType = TEXT("FlaxEngine.Texture");
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = TEXT("Flax.Texture");
        meta.Processor.SettingsVersion = 2;
        meta.Processor.SettingsJson = "{\"srgb\":false,\"maxSize\":4096}";
        meta.Labels.Add(TEXT("stone"));
        meta.Labels.Add(TEXT("environment"));
        return meta;
    }

}

TEST_CASE("Asset meta parses canonicalizes and preserves unknown fields")
{
    const StringAnsi json =
        "{"
        "\"newRoot\":{\"plugin\":true},"
        "\"labels\":[\"stone\",\"environment\"],"
        "\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"FlaxEngine.Model\"},\"mesh:/Root/Body\":{\"guid\":\"53ac120a49ed4beebf9c810cc48f2ea7\",\"fileId\":9007199254740993,\"collisionSalt\":0,\"type\":\"FlaxEngine.Model\",\"name\":\"Body\",\"removed\":false,\"pluginSub\":7}},"
        "\"importer\":{\"settings\":{\"z\":1,\"a\":2},\"version\":2,\"id\":\"Flax.Model\",\"pluginProcessor\":\"kept\"},"
        "\"folderAsset\":false,"
        "\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\","
        "\"userData\":{\"owner\":\"test\"},"
        "\"fileFormatVersion\":2} ";
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::Parse(json, TEXT("Robot.gltf.meta"), meta, diagnostic));
    CHECK(meta.ID.ToString(Guid::FormatType::N) == TEXT("36f15f0c4b354af88ba2f72f6cb82e22"));
    CHECK(meta.SubAssets.Count() == 1);
    CHECK(meta.SubAssets[TEXT("mesh:/Root/Body")].ID.ToString(Guid::FormatType::N) == TEXT("53ac120a49ed4beebf9c810cc48f2ea7"));
    CHECK(meta.SubAssets[TEXT("mesh:/Root/Body")].LocalId == 9007199254740993ll);
    CHECK(meta.SubAssets[TEXT("mesh:/Root/Body")].CollisionSalt == 0);
    CHECK(meta.UnknownFields.ContainsKey("newRoot"));
    CHECK(meta.Processor.UnknownFields.ContainsKey("pluginProcessor"));
    CHECK(meta.SubAssets[TEXT("mesh:/Root/Body")].UnknownFields.ContainsKey("pluginSub"));
    CHECK(meta.UserDataJson.Contains("\"owner\": \"test\""));

    StringAnsi first;
    StringAnsi second;
    REQUIRE_FALSE(meta.ToJson(first, diagnostic));
    AssetMeta reparsed;
    REQUIRE_FALSE(AssetMeta::Parse(first, TEXT("Robot.gltf.meta"), reparsed, diagnostic));
    REQUIRE_FALSE(reparsed.ToJson(second, diagnostic));
    CHECK(second == first);
    CHECK(first.Contains("\"newRoot\""));
    CHECK(first.Find("\"id\"") < first.Find("\"version\""));
    CHECK(first.Find("\"version\"") < first.Find("\"settings\""));
    const int32 subAssetGuid = first.Find("\"guid\": \"53ac120a49ed4beebf9c810cc48f2ea7\"");
    const int32 subAssetFileId = first.Find("\"fileId\": 9007199254740993");
    const int32 subAssetType = first.FindLast("\"type\": \"FlaxEngine.Model\"");
    REQUIRE(subAssetGuid != -1);
    REQUIRE(subAssetFileId != -1);
    REQUIRE(subAssetType != -1);
    CHECK(subAssetGuid < subAssetFileId);
    CHECK(subAssetFileId < subAssetType);
    CHECK(first.Contains("\"pluginProcessor\""));
    CHECK(first.Contains("\"pluginSub\""));
    CHECK(first.StartsWith("{\n  \"fileFormatVersion\":"));
    CHECK(first.EndsWith("\n"));
}

TEST_CASE("Asset meta rejects invalid importer and duplicate local file IDs")
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    CHECK(AssetMeta::Parse("{\"fileFormatVersion\":2,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"bad id\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"}},\"labels\":[]}", TEXT("bad.meta"), meta, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(AssetMeta::Parse("{\"fileFormatVersion\":2,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"},\"mesh:A\":{\"guid\":\"53ac120a49ed4beebf9c810cc48f2ea7\",\"fileId\":7,\"collisionSalt\":0,\"type\":\"T\"},\"mesh:B\":{\"guid\":\"737e46e96fdb4a08a310cd19515ef09b\",\"fileId\":7,\"collisionSalt\":1,\"type\":\"T\"}},\"labels\":[]}", TEXT("duplicate.meta"), meta, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
}

TEST_CASE("Folder metadata has no imported object table")
{
    AssetMeta folder;
    folder.ID = Guid::New();
    folder.FolderAsset = true;
    folder.SourceKind = AssetSourceKind::Folder;
    folder.Processor.ID = TEXT("Flax.Folder");
    folder.Processor.SettingsVersion = 1;
    folder.Processor.SettingsJson = "{}";
    StringAnsi json;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(folder.ToJson(json, diagnostic));
    CHECK_FALSE(json.Contains("\"objectIds\""));
    AssetMeta reparsed;
    REQUIRE_FALSE(AssetMeta::Parse(json, TEXT("Folder.meta"), reparsed, diagnostic));
    CHECK(reparsed.FolderAsset);
    CHECK(reparsed.SubAssets.Count() == 0);
}

TEST_CASE("Asset meta rejects duplicate object GUIDs and reconciliation identifiers")
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    const char* duplicateGuid = "{\"fileFormatVersion\":2,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"},\"mesh:A\":{\"guid\":\"53ac120a49ed4beebf9c810cc48f2ea7\",\"fileId\":7,\"collisionSalt\":0,\"type\":\"T\"},\"mesh:B\":{\"guid\":\"53ac120a49ed4beebf9c810cc48f2ea7\",\"fileId\":8,\"collisionSalt\":0,\"type\":\"T\"}},\"labels\":[]}";
    CHECK(AssetMeta::Parse(duplicateGuid, TEXT("duplicate-guid.meta"), meta, diagnostic));

    const char* duplicateAlias = "{\"fileFormatVersion\":2,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"},\"mesh:A\":{\"guid\":\"53ac120a49ed4beebf9c810cc48f2ea7\",\"fileId\":7,\"collisionSalt\":0,\"type\":\"T\",\"previousIdentifiers\":[\"mesh:B\"]},\"mesh:B\":{\"guid\":\"737e46e96fdb4a08a310cd19515ef09b\",\"fileId\":8,\"collisionSalt\":0,\"type\":\"T\"}},\"labels\":[]}";
    CHECK(AssetMeta::Parse(duplicateAlias, TEXT("duplicate-alias.meta"), meta, diagnostic));
}

TEST_CASE("Asset meta atomic write preserves old complete sidecar on failures")
{
    const String root = Globals::ProjectLibraryFolder / TEXT("Tests/AssetMetaIO");
    const String path = root / TEXT("Stone.png.meta");
    FileSystem::DeleteDirectory(root, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetMeta original = MakeMeta();
    AssetPipelineDiagnostic diagnostic;
    uint32 writeHash = 0;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(path, original, diagnostic, &writeHash));
    CHECK(writeHash != 0);
    BytesContainer originalBytes;
    REQUIRE_FALSE(File::ReadAllBytes(path, originalBytes));

    AssetMeta changed = original;
    changed.ID = Guid::New();
    const AssetMetaWriteFailurePoint failures[] =
    {
        AssetMetaWriteFailurePoint::BeforeWrite,
        AssetMetaWriteFailurePoint::AfterWrite,
        AssetMetaWriteFailurePoint::AfterValidate,
        AssetMetaWriteFailurePoint::BeforeReplace,
    };
    for (AssetMetaWriteFailurePoint failure : failures)
    {
        CHECK(AssetMeta::SaveAtomic(path, changed, diagnostic, nullptr, failure));
        BytesContainer current;
        REQUIRE_FALSE(File::ReadAllBytes(path, current));
        REQUIRE(current.Length() == originalBytes.Length());
        CHECK(Platform::MemoryCompare(current.Get(), originalBytes.Get(), current.Length()) == 0);
    }

    REQUIRE_FALSE(AssetMeta::SaveAtomic(path, changed, diagnostic));
    AssetMeta loaded;
    REQUIRE_FALSE(AssetMeta::Load(path, loaded, diagnostic));
    CHECK(loaded.ID == changed.ID);

    FileSystem::SetReadOnly(path, true);
    SCOPE_EXIT { FileSystem::SetReadOnly(path, false); };
    CHECK(AssetMeta::SaveAtomic(path, original, diagnostic));
    REQUIRE_FALSE(AssetMeta::Load(path, loaded, diagnostic));
    CHECK(loaded.ID == changed.ID);
}

TEST_CASE("Asset meta strictly rejects unsupported and non-canonical identity formats")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetMetaLegacy-") + Guid::New().ToString(Guid::FormatType::N));
    const String path = root / TEXT("old.meta");
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    const StringAnsi oldJson = "{\"fileFormatVersion\":1,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"}},\"labels\":[]}";
    REQUIRE_FALSE(File::WriteAllBytes(path, oldJson.Get(), oldJson.Length()));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    BytesContainer before;
    REQUIRE_FALSE(File::ReadAllBytes(path, before));
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    CHECK(AssetMeta::Load(path, meta, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
    CHECK(diagnostic.Message.Contains(TEXT("offline migrator")));
    BytesContainer after;
    REQUIRE_FALSE(File::ReadAllBytes(path, after));
    REQUIRE(after.Length() == before.Length());
    CHECK(Platform::MemoryCompare(after.Get(), before.Get(), before.Length()) == 0);

    CHECK(AssetMeta::Parse("{\"fileFormatVersion\":2,\"guid\":\"36F15F0C-4B35-4AF8-8BA2-F72F6CB82E22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"}},\"labels\":[]}", path, meta, diagnostic));
    CHECK(AssetMeta::Parse("{\"fileFormatVersion\":2,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"},\"mesh:A\":{\"fileId\":7,\"collisionSalt\":0,\"type\":\"T\"}},\"labels\":[]}", path, meta, diagnostic));
    CHECK(AssetMeta::Parse("{\"fileFormatVersion\":2,\"guid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"folderAsset\":false,\"importer\":{\"id\":\"Flax.T\",\"version\":1,\"settings\":{}},\"objectIds\":{\"main\":{\"fileId\":1,\"type\":\"T\"}},\"labels\":[]}", path, meta, diagnostic));
}

TEST_CASE("Asset metadata clone regenerates every GUID and preserves reconciliation metadata")
{
    AssetMeta source = MakeMeta();
    SubAssetMeta live;
    live.ID = Guid::New();
    live.LocalId = 1234567890123456;
    live.CollisionSalt = 3;
    live.TypeName = TEXT("FlaxEngine.Model");
    live.DisplayName = TEXT("Live");
    SubAssetMeta tombstone = live;
    tombstone.ID = Guid::New();
    tombstone.LocalId = 2234567890123456;
    tombstone.DisplayName = TEXT("Removed");
    tombstone.Removed = true;
    source.SubAssets.Add(TEXT("mesh:/Live"), live);
    source.SubAssets.Add(TEXT("mesh:/Removed"), tombstone);
    const AssetMeta clone = source.CloneWithNewIdentities();
    CHECK(clone.ID != source.ID);
    CHECK(clone.SubAssets[TEXT("mesh:/Live")].ID != live.ID);
    CHECK(clone.SubAssets[TEXT("mesh:/Removed")].ID != tombstone.ID);
    CHECK(clone.SubAssets[TEXT("mesh:/Live")].ID != clone.SubAssets[TEXT("mesh:/Removed")].ID);
    CHECK(clone.SubAssets[TEXT("mesh:/Live")].LocalId == live.LocalId);
    CHECK(clone.SubAssets[TEXT("mesh:/Live")].CollisionSalt == live.CollisionSalt);
    CHECK(clone.SubAssets[TEXT("mesh:/Removed")].LocalId == tombstone.LocalId);
    CHECK(clone.SubAssets[TEXT("mesh:/Removed")].Removed);
    CHECK(clone.SubAssets[TEXT("mesh:/Live")].LocalId != clone.SubAssets[TEXT("mesh:/Removed")].LocalId);
}

#endif
