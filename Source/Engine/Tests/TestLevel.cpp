// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/ObjectsRemovalService.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/StringView.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/LargeWorlds.h"
#include "Engine/Level/Level.h"
#include "Engine/Level/Tags.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Serialization/Serialization.h"
#if USE_EDITOR
#include "Engine/Content/Content.h"
#include "Engine/Content/Cache/AssetsCache.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Level/Actors/EmptyActor.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Level/Scene/SceneAsset.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/JsonWriters.h"
#include "FlaxEngine.Gen.h"
#endif
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    Guid ParseGuid(const char* text)
    {
        Guid id;
        REQUIRE(!Guid::Parse(StringAnsiView(text), id));
        return id;
    }

    String GetTestScenePath(const Char* name)
    {
        return Globals::ProjectContentFolder / (String(TEXT("__ExternalActorsTest_")) + name + DEFAULT_SCENE_EXTENSION_DOT);
    }

    String GetSceneActorsFolder(const String& scenePath);

    String GetExternalActorsFolder(const String& scenePath)
    {
        return GetSceneActorsFolder(scenePath) / TEXT("ExternalActors");
    }

    String GetSceneActorsFolder(const String& scenePath)
    {
        String relativePath = FileSystem::ConvertAbsolutePathToRelative(Globals::ProjectContentFolder, scenePath);
        FileSystem::NormalizePath(relativePath);
        const String directory = String(StringUtils::GetDirectoryName(relativePath));
        const String filename = String(StringUtils::GetFileNameWithoutExtension(relativePath));
        return directory.HasChars()
               ? Globals::ProjectFolder / TEXT("SceneActors") / directory / filename
               : Globals::ProjectFolder / TEXT("SceneActors") / filename;
    }

    String GetExternalActorPath(const String& scenePath, const Guid& actorId)
    {
        const String actorIdText = actorId.ToString(Guid::FormatType::N);
        return GetExternalActorsFolder(scenePath) / actorIdText.Substring(0, 2) / actorIdText + TEXT(".actor");
    }

    void CleanupTestSceneFiles(const String& scenePath)
    {
        Content::GetRegistry()->DeleteAsset(scenePath, nullptr);
        FileSystem::DeleteFile(scenePath);
        FileSystem::DeleteDirectory(GetSceneActorsFolder(scenePath));
    }

    void EnsureDirectory(const StringView& directory)
    {
        if (directory.HasChars() && !FileSystem::DirectoryExists(directory))
            REQUIRE(!FileSystem::CreateDirectory(directory));
    }

    void WriteTestSceneAsset(const String& scenePath, const Guid& sceneId, bool externalActors)
    {
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(sceneId);
        writer.JKEY("TypeName");
        writer.String("FlaxEngine.SceneAsset", ARRAY_COUNT("FlaxEngine.SceneAsset") - 1);
        writer.JKEY("EngineBuild");
        writer.Int(FLAXENGINE_VERSION_BUILD);
        if (externalActors)
        {
            writer.JKEY("ExternalActors");
            writer.Bool(true);
        }
        writer.JKEY("Data");
        writer.StartArray();
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(sceneId);
        writer.JKEY("TypeName");
        writer.String("FlaxEngine.Scene", ARRAY_COUNT("FlaxEngine.Scene") - 1);
        if (externalActors)
        {
            writer.JKEY("UseExternalActors");
            writer.Bool(true);
        }
        writer.EndObject();
        writer.EndArray(1);
        writer.EndObject();

        EnsureDirectory(StringUtils::GetDirectoryName(scenePath));
        REQUIRE(!File::WriteAllBytes(scenePath, buffer.GetString(), static_cast<int32>(buffer.GetSize())));
        Content::GetRegistry()->RegisterAsset(sceneId, SceneAsset::TypeName, scenePath);
    }

    void ReadFileBytes(const String& path, BytesContainer& data)
    {
        data.Release();
        REQUIRE(!File::ReadAllBytes(path, data));
    }

    bool AreBytesEqual(const BytesContainer& a, const BytesContainer& b)
    {
        return a.Length() == b.Length() && (a.Length() == 0 || Platform::MemoryCompare(a.Get(), b.Get(), a.Length()) == 0);
    }

    void WriteExternalActorFile(const String& scenePath, const Guid& actorId, const Guid& parentId, const char* name, int64 orderInParent, int32 engineBuild = FLAXENGINE_VERSION_BUILD)
    {
        const String actorPath = GetExternalActorPath(scenePath, actorId);
        EnsureDirectory(StringUtils::GetDirectoryName(actorPath));

        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(actorId);
        writer.JKEY("TypeName");
        writer.String("FlaxEngine.SceneActor", ARRAY_COUNT("FlaxEngine.SceneActor") - 1);
        writer.JKEY("EngineBuild");
        writer.Int(engineBuild);
        writer.JKEY("Data");
        writer.StartArray();
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(actorId);
        writer.JKEY("TypeName");
        writer.String("FlaxEngine.EmptyActor", ARRAY_COUNT("FlaxEngine.EmptyActor") - 1);
        writer.JKEY("ParentID");
        writer.Guid(parentId);
        writer.JKEY("OrderInParent");
        writer.Int64(orderInParent);
        writer.JKEY("Name");
        writer.String(name, StringUtils::Length(name));
        writer.EndObject();
        writer.EndArray(1);
        writer.EndObject();

        REQUIRE(!File::WriteAllBytes(actorPath, buffer.GetString(), static_cast<int32>(buffer.GetSize())));
    }

    void ParseJson(rapidjson_flax::Document& document, const rapidjson_flax::StringBuffer& buffer)
    {
        document.Parse(buffer.GetString(), buffer.GetSize());
        REQUIRE(!document.HasParseError());
    }

    void ParseJsonFile(rapidjson_flax::Document& document, const String& path)
    {
        BytesContainer data;
        REQUIRE(!File::ReadAllBytes(path, data));
        document.Parse(data.Get<char>(), data.Length());
        REQUIRE(!document.HasParseError());
    }

    const rapidjson_flax::Value& GetDataArray(const rapidjson_flax::Document& document)
    {
        const auto data = document.FindMember("Data");
        REQUIRE(data != document.MemberEnd());
        REQUIRE(data->value.IsArray());
        return data->value;
    }

    bool ContainsObject(const rapidjson_flax::Value& data, const Guid& id)
    {
        for (rapidjson::SizeType i = 0; i < data.Size(); i++)
        {
            if (JsonTools::GetGuid(data[i], "ID") == id)
                return true;
        }
        return false;
    }
}

#endif

TEST_CASE("Serialization")
{
    SECTION("Double vector deserialization preserves precision")
    {
        const double expectedY = 67.1239548087392;
        const double expectedZ = 8.602915590833845;
        rapidjson_flax::Document document;
        document.Parse("{\"X\":0.0,\"Y\":67.1239548087392,\"Z\":8.602915590833845}");
        REQUIRE(!document.HasParseError());

        Double3 value;
        Serialization::Deserialize(document, value, nullptr);

        CHECK(value.Y > expectedY - 1e-12);
        CHECK(value.Y < expectedY + 1e-12);
        CHECK(value.Z > expectedZ - 1e-12);
        CHECK(value.Z < expectedZ + 1e-12);
    }

    SECTION("Malformed variant type name reports a stream error")
    {
        MemoryWriteStream output;
        output.WriteByte((byte)VariantType::Object);
        output.WriteInt32(MAX_int32);
        output.WriteInt32(STREAM_MAX_STRING_LENGTH);
        MemoryReadStream input(output.GetHandle(), output.GetPosition());
        VariantType type;

        input.Read(type);

        CHECK(input.HasError());
    }
}

TEST_CASE("LargeWorlds")
{
    SECTION("UpdateOrigin")
    {
        LargeWorlds::Enable = true;
        Vector3 origin = Vector3::Zero;
        LargeWorlds::UpdateOrigin(origin, Vector3::Zero);
        CHECK(origin == Vector3::Zero);
        LargeWorlds::UpdateOrigin(origin, Vector3(LargeWorlds::ChunkSize * 0.5, LargeWorlds::ChunkSize * 1.0001, LargeWorlds::ChunkSize * 1.5));
        CHECK(origin == Vector3(0, 0, LargeWorlds::ChunkSize * 1));
    }
}

TEST_CASE("Tags")
{
    SECTION("Tag")
    {
        auto prevTags = Tags::List;

        Tags::List = Array<String>({ TEXT("A"), TEXT("A.1"), TEXT("B"), TEXT("B.1"), });

        auto a = Tags::Get(TEXT("A"));
        auto a1 = Tags::Get(TEXT("A.1"));
        auto b = Tags::Get(TEXT("B"));
        auto b1 = Tags::Get(TEXT("B.1"));
        auto c = Tags::Get(TEXT("C"));
        CHECK(a.Index == 1);
        CHECK(a1.Index == 2);
        CHECK(b.Index == 3);
        CHECK(b1.Index == 4);
        CHECK(c.Index == 5);

        Tags::List = prevTags;
    }

    SECTION("Tags")
    {
        auto prevTags = Tags::List;

        Tags::List = Array<String>({ TEXT("A"), TEXT("A.1"), TEXT("B"), TEXT("B.1"), });

        auto a = Tags::Get(TEXT("A"));
        auto a1 = Tags::Get(TEXT("A.1"));
        auto b = Tags::Get(TEXT("B"));
        auto b1 = Tags::Get(TEXT("B.1"));
        auto c = Tags::Get(TEXT("C"));

        Array<Tag> list = { a1, b1 };

        CHECK(Tags::HasTag(list, Tag()) == false);
        CHECK(Tags::HasTag(list, a1) == true);
        CHECK(Tags::HasTag(list, a) == true);
        CHECK(Tags::HasTag(list, c) == false);

        CHECK(Tags::HasTagExact(list, a1) == true);
        CHECK(Tags::HasTagExact(list, a) == false);
        CHECK(Tags::HasTagExact(list, c) == false);

        Tags::List = prevTags;
    }
}

#if USE_EDITOR

TEST_CASE("ExternalActorsSceneStorage")
{
    SECTION("Save splits actors and recomposes scene data")
    {
        const Guid sceneId = ParseGuid("11111111111111111111111111111111");
        const Guid parentId = ParseGuid("11111111111111111111111111111112");
        const Guid childId = ParseGuid("11111111111111111111111111111113");
        const Guid siblingId = ParseGuid("11111111111111111111111111111114");
        const Guid staleId = ParseGuid("11111111111111111111111111111115");
        const String scenePath = GetTestScenePath(TEXT("Save"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, staleId, sceneId, "Stale", 4096);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        scene->UseExternalActors = true;

        EmptyActor* parent = EmptyActor::Spawn(ScriptingObject::SpawnParams(parentId, EmptyActor::TypeInitializer));
        REQUIRE(parent);
        parent->SetName(TEXT("Parent"));
        parent->SetParent(scene);

        EmptyActor* child = EmptyActor::Spawn(ScriptingObject::SpawnParams(childId, EmptyActor::TypeInitializer));
        REQUIRE(child);
        child->SetName(TEXT("Child"));
        child->SetParent(parent);

        EmptyActor* sibling = EmptyActor::Spawn(ScriptingObject::SpawnParams(siblingId, EmptyActor::TypeInitializer));
        REQUIRE(sibling);
        sibling->SetName(TEXT("Sibling"));
        sibling->SetParent(scene);
        sibling->SetOrderInParent(0);

        REQUIRE(!Level::SaveScene(scene));

        Array<String> actorFiles;
        REQUIRE(!FileSystem::DirectoryGetFiles(actorFiles, GetExternalActorsFolder(scenePath), TEXT("*.actor"), DirectorySearchOption::AllDirectories));
        CHECK(actorFiles.Count() == 3);
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, parentId)));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, childId)));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, siblingId)));
        CHECK(!FileSystem::FileExists(GetExternalActorPath(scenePath, staleId)));

        BytesContainer sceneFileData;
        REQUIRE(!File::ReadAllBytes(scenePath, sceneFileData));
        rapidjson_flax::Document sceneDocument;
        sceneDocument.Parse(sceneFileData.Get<char>(), sceneFileData.Length());
        REQUIRE(!sceneDocument.HasParseError());
        REQUIRE(JsonTools::GetBool(sceneDocument, "ExternalActors", false));
        const rapidjson_flax::Value& savedData = GetDataArray(sceneDocument);
        REQUIRE(savedData.Size() == 1);

        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        rapidjson_flax::StringBuffer unifiedBuffer;
        Array<String> externalActorFiles;
        REQUIRE(!Level::SaveSceneAssetToBytes(sceneAsset, unifiedBuffer, &externalActorFiles, false));
        Content::UnloadAsset(sceneAsset);

        rapidjson_flax::Document unifiedDocument;
        ParseJson(unifiedDocument, unifiedBuffer);
        const rapidjson_flax::Value& unifiedData = GetDataArray(unifiedDocument);
        REQUIRE(unifiedData.Size() == 4);
        CHECK(ContainsObject(unifiedData, sceneId));
        CHECK(ContainsObject(unifiedData, parentId));
        CHECK(ContainsObject(unifiedData, childId));
        CHECK(ContainsObject(unifiedData, siblingId));
        CHECK(externalActorFiles.Count() == 4);

        Array<Guid> rootChildIds;
        for (rapidjson::SizeType i = 0; i < unifiedData.Size(); i++)
        {
            if (JsonTools::GetGuid(unifiedData[i], "ParentID") == sceneId)
                rootChildIds.Add(JsonTools::GetGuid(unifiedData[i], "ID"));
        }
        REQUIRE(rootChildIds.Count() == 2);
        CHECK(rootChildIds[0] == siblingId);
        CHECK(rootChildIds[1] == parentId);
    }

    SECTION("Scene actors path includes content relative folder")
    {
        const Guid sceneIdA = ParseGuid("22222222222222222222222222222211");
        const Guid sceneIdB = ParseGuid("22222222222222222222222222222212");
        const Guid actorIdA = ParseGuid("22222222222222222222222222222213");
        const Guid actorIdB = ParseGuid("22222222222222222222222222222214");
        const String scenePathA = Globals::ProjectContentFolder / TEXT("__ExternalActorsTest_A") / TEXT("Main.scene");
        const String scenePathB = Globals::ProjectContentFolder / TEXT("__ExternalActorsTest_B") / TEXT("Main.scene");
        CleanupTestSceneFiles(scenePathA);
        CleanupTestSceneFiles(scenePathB);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePathA);
            CleanupTestSceneFiles(scenePathB);
            FileSystem::DeleteDirectory(Globals::ProjectContentFolder / TEXT("__ExternalActorsTest_A"));
            FileSystem::DeleteDirectory(Globals::ProjectContentFolder / TEXT("__ExternalActorsTest_B"));
        };

        WriteTestSceneAsset(scenePathA, sceneIdA, true);
        WriteTestSceneAsset(scenePathB, sceneIdB, true);
        WriteExternalActorFile(scenePathA, actorIdA, sceneIdA, "Actor A", 1024);
        WriteExternalActorFile(scenePathB, actorIdB, sceneIdB, "Actor B", 1024);

        CHECK(GetSceneActorsFolder(scenePathA) != GetSceneActorsFolder(scenePathB));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePathA, actorIdA)));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePathB, actorIdB)));
    }

    SECTION("Generated scene data path remains in content")
    {
        const Guid sceneId = ParseGuid("55555555555555555555555555555551");
        const String scenePath = GetTestScenePath(TEXT("GeneratedData"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };

        CHECK(scene->GetDataFolderPath() == Globals::ProjectContentFolder / TEXT("SceneData") / String(StringUtils::GetFileNameWithoutExtension(scenePath)));
    }

    SECTION("Scene byte snapshots include external actors without touching actor files")
    {
        const Guid sceneId = ParseGuid("66666666666666666666666666666661");
        const Guid actorId = ParseGuid("66666666666666666666666666666662");
        const Guid childId = ParseGuid("66666666666666666666666666666663");
        const Guid staleId = ParseGuid("66666666666666666666666666666664");
        const String scenePath = GetTestScenePath(TEXT("ByteSnapshot"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, staleId, sceneId, "Stale", 4096);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        scene->UseExternalActors = true;

        EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Actor"));
        actor->SetParent(scene);

        EmptyActor* child = EmptyActor::Spawn(ScriptingObject::SpawnParams(childId, EmptyActor::TypeInitializer));
        REQUIRE(child);
        child->SetName(TEXT("Child"));
        child->SetParent(actor);

        rapidjson_flax::StringBuffer snapshotBuffer;
        REQUIRE(!Level::SaveSceneToBytes(scene, snapshotBuffer, false));

        rapidjson_flax::Document snapshotDocument;
        ParseJson(snapshotDocument, snapshotBuffer);
        const rapidjson_flax::Value& snapshotData = GetDataArray(snapshotDocument);
        REQUIRE(snapshotData.Size() == 3);
        CHECK(ContainsObject(snapshotData, sceneId));
        CHECK(ContainsObject(snapshotData, actorId));
        CHECK(ContainsObject(snapshotData, childId));
        CHECK(!JsonTools::GetBool(snapshotDocument, "ExternalActors", false));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, staleId)));
        CHECK(!FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
        CHECK(!FileSystem::FileExists(GetExternalActorPath(scenePath, childId)));
    }

    SECTION("Save preserves unchanged external actor files")
    {
        const Guid sceneId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1");
        const Guid actorId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2");
        const String scenePath = GetTestScenePath(TEXT("UnchangedSave"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        scene->UseExternalActors = true;

        EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Actor"));
        actor->SetParent(scene);

        REQUIRE(!Level::SaveScene(scene));
        const String actorPath = GetExternalActorPath(scenePath, actorId);
        rapidjson_flax::Document actorDocument;
        ParseJsonFile(actorDocument, actorPath);
        const rapidjson_flax::Value& actorData = GetDataArray(actorDocument);

        rapidjson_flax::StringBuffer staleWrapperBuffer;
        PrettyJsonWriter staleWriter(staleWrapperBuffer);
        staleWriter.StartObject();
        staleWriter.JKEY("ID");
        staleWriter.Guid(actorId);
        staleWriter.JKEY("TypeName");
        staleWriter.String("FlaxEngine.SceneActor", ARRAY_COUNT("FlaxEngine.SceneActor") - 1);
        staleWriter.JKEY("EngineBuild");
        staleWriter.Int(FLAXENGINE_VERSION_BUILD + 1);
        staleWriter.JKEY("Data");
        actorData.Accept(staleWriter.GetWriter());
        staleWriter.EndObject();
        REQUIRE(!File::WriteAllBytes(actorPath, staleWrapperBuffer.GetString(), static_cast<int32>(staleWrapperBuffer.GetSize())));

        BytesContainer beforeSave;
        BytesContainer afterSave;
        ReadFileBytes(actorPath, beforeSave);
        REQUIRE(!Level::SaveScene(scene));
        ReadFileBytes(actorPath, afterSave);

        CHECK(AreBytesEqual(beforeSave, afterSave));
    }

    SECTION("Convert external actors scene to internal actors")
    {
        const Guid sceneId = ParseGuid("77777777777777777777777777777771");
        const Guid actorId = ParseGuid("77777777777777777777777777777772");
        const Guid childId = ParseGuid("77777777777777777777777777777773");
        const Guid staleId = ParseGuid("77777777777777777777777777777774");
        const String scenePath = GetTestScenePath(TEXT("Internalize"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, staleId, sceneId, "Stale", 4096);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        scene->UseExternalActors = true;

        EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Actor"));
        actor->SetParent(scene);

        EmptyActor* child = EmptyActor::Spawn(ScriptingObject::SpawnParams(childId, EmptyActor::TypeInitializer));
        REQUIRE(child);
        child->SetName(TEXT("Child"));
        child->SetParent(actor);

        REQUIRE(!Level::ConvertSceneToInternalActors(scene));
        CHECK(!scene->UseExternalActors);
        CHECK(!FileSystem::DirectoryExists(GetSceneActorsFolder(scenePath)));

        rapidjson_flax::Document sceneDocument;
        ParseJsonFile(sceneDocument, scenePath);
        CHECK(!JsonTools::GetBool(sceneDocument, "ExternalActors", false));
        const rapidjson_flax::Value& savedData = GetDataArray(sceneDocument);
        REQUIRE(savedData.Size() == 3);
        CHECK(!JsonTools::GetBool(savedData[0], "UseExternalActors", false));
        CHECK(ContainsObject(savedData, sceneId));
        CHECK(ContainsObject(savedData, actorId));
        CHECK(ContainsObject(savedData, childId));
        CHECK(!ContainsObject(savedData, staleId));
    }

    SECTION("Recompose writes parents before children")
    {
        const Guid sceneId = ParseGuid("fffffffffffffffffffffffffffffff1");
        const Guid parentId = ParseGuid("11111111111111111111111111111121");
        const Guid childId = ParseGuid("22222222222222222222222222222221");
        const String scenePath = GetTestScenePath(TEXT("ParentFirst"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, childId, parentId, "Child", 1024);
        WriteExternalActorFile(scenePath, parentId, sceneId, "Parent", 1024);

        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        rapidjson_flax::StringBuffer unifiedBuffer;
        REQUIRE(!Level::SaveSceneAssetToBytes(sceneAsset, unifiedBuffer, nullptr, false));
        Content::UnloadAsset(sceneAsset);

        rapidjson_flax::Document unifiedDocument;
        ParseJson(unifiedDocument, unifiedBuffer);
        const rapidjson_flax::Value& unifiedData = GetDataArray(unifiedDocument);
        REQUIRE(unifiedData.Size() == 3);
        CHECK(JsonTools::GetGuid(unifiedData[0], "ID") == sceneId);
        CHECK(JsonTools::GetGuid(unifiedData[1], "ID") == parentId);
        CHECK(JsonTools::GetGuid(unifiedData[2], "ID") == childId);
    }

    SECTION("Clone external actors scene copies and remaps actor files")
    {
        const Guid sceneId = ParseGuid("33333333333333333333333333333331");
        const Guid parentId = ParseGuid("33333333333333333333333333333332");
        const Guid childId = ParseGuid("33333333333333333333333333333333");
        const Guid cloneSceneId = ParseGuid("33333333333333333333333333333334");
        const String scenePath = GetTestScenePath(TEXT("CloneSource"));
        const String clonePath = GetTestScenePath(TEXT("CloneTarget"));
        CleanupTestSceneFiles(scenePath);
        CleanupTestSceneFiles(clonePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
            CleanupTestSceneFiles(clonePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, parentId, sceneId, "Parent", 1024);
        WriteExternalActorFile(scenePath, childId, parentId, "Child", 1024);

        REQUIRE(!Content::CloneAssetFile(clonePath, scenePath, cloneSceneId));

        rapidjson_flax::Document cloneSceneDocument;
        ParseJsonFile(cloneSceneDocument, clonePath);
        const rapidjson_flax::Value& cloneSceneData = GetDataArray(cloneSceneDocument);
        REQUIRE(cloneSceneData.Size() == 1);
        CHECK(JsonTools::GetGuid(cloneSceneDocument, "ID") == cloneSceneId);
        CHECK(JsonTools::GetGuid(cloneSceneData[0], "ID") == cloneSceneId);

        Array<String> cloneActorFiles;
        REQUIRE(!FileSystem::DirectoryGetFiles(cloneActorFiles, GetExternalActorsFolder(clonePath), TEXT("*.actor"), DirectorySearchOption::AllDirectories));
        REQUIRE(cloneActorFiles.Count() == 2);

        Array<Guid> cloneActorIds;
        Array<Guid> cloneParentIds;
        for (const String& file : cloneActorFiles)
        {
            rapidjson_flax::Document actorDocument;
            ParseJsonFile(actorDocument, file);
            const rapidjson_flax::Value& actorData = GetDataArray(actorDocument);
            REQUIRE(actorData.Size() == 1);
            cloneActorIds.Add(JsonTools::GetGuid(actorData[0], "ID"));
            cloneParentIds.Add(JsonTools::GetGuid(actorData[0], "ParentID"));
        }

        CHECK(!cloneActorIds.Contains(parentId));
        CHECK(!cloneActorIds.Contains(childId));
        REQUIRE(cloneParentIds.Contains(cloneSceneId));
        Guid cloneRootActorId = Guid::Empty;
        for (int32 i = 0; i < cloneParentIds.Count(); i++)
        {
            if (cloneParentIds[i] == cloneSceneId)
            {
                cloneRootActorId = cloneActorIds[i];
                break;
            }
        }
        REQUIRE(cloneRootActorId.IsValid());
        CHECK(cloneParentIds.Contains(cloneRootActorId));
    }

    SECTION("Clone external actors scene rejects empty destination actors folder")
    {
        const Guid sceneId = ParseGuid("99999999999999999999999999999991");
        const Guid actorId = ParseGuid("99999999999999999999999999999992");
        const Guid cloneSceneId = ParseGuid("99999999999999999999999999999993");
        const String scenePath = GetTestScenePath(TEXT("CloneEmptyDestinationSource"));
        const String clonePath = GetTestScenePath(TEXT("CloneEmptyDestinationTarget"));
        CleanupTestSceneFiles(scenePath);
        CleanupTestSceneFiles(clonePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
            CleanupTestSceneFiles(clonePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);
        EnsureDirectory(GetExternalActorsFolder(clonePath));

        REQUIRE(Content::CloneAssetFile(clonePath, scenePath, cloneSceneId));

        CHECK(!FileSystem::FileExists(clonePath));
        CHECK(FileSystem::DirectoryExists(GetExternalActorsFolder(clonePath)));
    }

    SECTION("Clone malformed binary fails without partial output")
    {
        const String sourcePath = Globals::ProjectContentFolder / TEXT("__MalformedCloneSource.flax");
        const String clonePath = Globals::ProjectContentFolder / TEXT("__MalformedCloneTarget.flax");
        FileSystem::DeleteFile(sourcePath);
        FileSystem::DeleteFile(clonePath);
        SCOPE_EXIT
        {
            FileSystem::DeleteFile(sourcePath);
            FileSystem::DeleteFile(clonePath);
        };
        const byte malformed[] = { 'F', 'L', 'A', 'X', 0, 0xff, 0x13 };
        REQUIRE(!File::WriteAllBytes(sourcePath, malformed, ARRAY_COUNT(malformed)));

        CHECK(Content::CloneAssetFile(clonePath, sourcePath, Guid::New()));
        CHECK(FileSystem::FileExists(sourcePath));
        CHECK(!FileSystem::FileExists(clonePath));
    }

    SECTION("Clone preserves an existing destination")
    {
        const Guid sceneId = ParseGuid("99999999999999999999999999999981");
        const String sourcePath = GetTestScenePath(TEXT("CloneCollisionSource"));
        const String clonePath = GetTestScenePath(TEXT("CloneCollisionTarget"));
        CleanupTestSceneFiles(sourcePath);
        CleanupTestSceneFiles(clonePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(sourcePath);
            CleanupTestSceneFiles(clonePath);
        };
        WriteTestSceneAsset(sourcePath, sceneId, false);
        const byte destinationBytes[] = { 7, 8, 9, 10 };
        REQUIRE(!File::WriteAllBytes(clonePath, destinationBytes, ARRAY_COUNT(destinationBytes)));

        CHECK(Content::CloneAssetFile(clonePath, sourcePath, Guid::New()));
        BytesContainer preservedBytes;
        REQUIRE(!File::ReadAllBytes(clonePath, preservedBytes));
        REQUIRE(preservedBytes.Length() == ARRAY_COUNT(destinationBytes));
        CHECK(Platform::MemoryCompare(preservedBytes.Get(), destinationBytes, ARRAY_COUNT(destinationBytes)) == 0);
    }

    SECTION("Replace restores an existing destination when cloning fails")
    {
        const String sourcePath = Globals::ProjectContentFolder / TEXT("__MalformedReplaceSource.flax");
        const String destinationPath = Globals::ProjectContentFolder / TEXT("__MalformedReplaceTarget.flax");
        FileSystem::DeleteFile(sourcePath);
        FileSystem::DeleteFile(destinationPath);
        SCOPE_EXIT
        {
            FileSystem::DeleteFile(sourcePath);
            FileSystem::DeleteFile(destinationPath);
        };
        const byte malformed[] = { 'F', 'L', 'A', 'X', 0, 0xff, 0x13 };
        const byte destinationBytes[] = { 7, 8, 9, 10 };
        REQUIRE(!File::WriteAllBytes(sourcePath, malformed, ARRAY_COUNT(malformed)));
        REQUIRE(!File::WriteAllBytes(destinationPath, destinationBytes, ARRAY_COUNT(destinationBytes)));

        CHECK(Content::CloneAssetFile(destinationPath, sourcePath, Guid::New(), true));
        BytesContainer preservedBytes;
        REQUIRE(!File::ReadAllBytes(destinationPath, preservedBytes));
        REQUIRE(preservedBytes.Length() == ARRAY_COUNT(destinationBytes));
        CHECK(Platform::MemoryCompare(preservedBytes.Get(), destinationBytes, ARRAY_COUNT(destinationBytes)) == 0);
    }

    SECTION("Replace commits a validated staged binary asset and refreshes cached storage")
    {
        const String sourcePath = Globals::EngineContentFolder / TEXT("Engine/DefaultMaterial.flax");
        const String destinationPath = Globals::ProjectContentFolder / TEXT("__ReplaceCachedStorage.flax");
        const Guid initialId = Guid::New();
        const Guid replacementId = Guid::New();
        FileSystem::DeleteFile(destinationPath);
        SCOPE_EXIT
        {
            FileSystem::DeleteFile(destinationPath);
        };

        REQUIRE(!Content::CloneAssetFile(destinationPath, sourcePath, initialId));
        auto cachedStorage = ContentStorageManager::GetStorage(destinationPath);
        REQUIRE(cachedStorage);
        REQUIRE(cachedStorage->HasAsset(initialId));

        REQUIRE(!Content::CloneAssetFile(destinationPath, sourcePath, replacementId, true));
        CHECK(cachedStorage->HasAsset(replacementId));
        CHECK(!cachedStorage->HasAsset(initialId));

        AssetInitData replacedData;
        REQUIRE(!cachedStorage->LoadAssetHeader(replacementId, replacedData));
        CHECK(replacedData.Header.ID == replacementId);
        CHECK(replacedData.Header.TypeName == TEXT("FlaxEngine.Material"));
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            if (replacedData.Header.Chunks[i])
                REQUIRE(!cachedStorage->LoadAssetChunk(replacedData.Header.Chunks[i]));
        }
    }

    SECTION("Rename external actors scene moves scene actors folder")
    {
        const Guid sceneId = ParseGuid("44444444444444444444444444444441");
        const Guid actorId = ParseGuid("44444444444444444444444444444442");
        const String scenePath = GetTestScenePath(TEXT("RenameSource"));
        const String renamedPath = GetTestScenePath(TEXT("RenameTarget"));
        CleanupTestSceneFiles(scenePath);
        CleanupTestSceneFiles(renamedPath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
            CleanupTestSceneFiles(renamedPath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);
        const String oldSceneActorsFolder = GetSceneActorsFolder(scenePath);
        const String newSceneActorsFolder = GetSceneActorsFolder(renamedPath);
        CHECK(!FileSystem::FileExists(newSceneActorsFolder));
        CHECK(!FileSystem::DirectoryExists(newSceneActorsFolder));

        REQUIRE(!Content::RenameAsset(scenePath, renamedPath));

        CHECK(!FileSystem::DirectoryExists(oldSceneActorsFolder));
        CHECK(FileSystem::DirectoryExists(newSceneActorsFolder));
        CHECK(FileSystem::FileExists(GetExternalActorPath(renamedPath, actorId)));
    }

    SECTION("Rename Json asset replaces empty destination file")
    {
        const Guid sceneId = ParseGuid("45454545454545454545454545454531");
        const String scenePath = GetTestScenePath(TEXT("RenameJsonEmptyDestinationSource"));
        const String renamedPath = GetTestScenePath(TEXT("RenameJsonEmptyDestinationTarget"));
        CleanupTestSceneFiles(scenePath);
        CleanupTestSceneFiles(renamedPath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
            CleanupTestSceneFiles(renamedPath);
        };
        WriteTestSceneAsset(scenePath, sceneId, false);
        REQUIRE(!File::WriteAllBytes(renamedPath, nullptr, 0));
        REQUIRE(FileSystem::GetFileSize(scenePath) > 0);
        REQUIRE(FileSystem::GetFileSize(renamedPath) == 0);

        REQUIRE(!Content::RenameAsset(scenePath, renamedPath));

        CHECK(!FileSystem::FileExists(scenePath));
        CHECK(FileSystem::GetFileSize(renamedPath) > 0);
        rapidjson_flax::Document renamedDocument;
        ParseJsonFile(renamedDocument, renamedPath);
        CHECK(JsonTools::GetGuid(renamedDocument, "ID") == sceneId);
    }

    SECTION("Rename content folder is atomic and updates loaded assets")
    {
        const Guid sceneId = ParseGuid("45454545454545454545454545454541");
        const Guid actorId = ParseGuid("45454545454545454545454545454542");
        const String sourceFolder = Globals::ProjectContentFolder / TEXT("__ContentFolderMoveSource");
        const String destinationFolder = Globals::ProjectContentFolder / TEXT("__ContentFolderMoveTarget");
        const String sourceScenePath = sourceFolder / TEXT("Nested/FolderMove.scene");
        const String destinationScenePath = destinationFolder / TEXT("Nested/FolderMove.scene");
        FileSystem::DeleteDirectory(sourceFolder);
        FileSystem::DeleteDirectory(destinationFolder);
        CleanupTestSceneFiles(sourceScenePath);
        CleanupTestSceneFiles(destinationScenePath);
        SceneAsset* sceneAsset = nullptr;
        SCOPE_EXIT
        {
            if (sceneAsset)
                Content::UnloadAsset(sceneAsset);
            CleanupTestSceneFiles(sourceScenePath);
            CleanupTestSceneFiles(destinationScenePath);
            FileSystem::DeleteDirectory(sourceFolder);
            FileSystem::DeleteDirectory(destinationFolder);
        };

        WriteTestSceneAsset(sourceScenePath, sceneId, true);
        WriteExternalActorFile(sourceScenePath, actorId, sceneId, "Actor", 1024);
        sceneAsset = Content::Load<SceneAsset>(sourceScenePath);
        REQUIRE(sceneAsset);
        REQUIRE(!sceneAsset->WaitForLoaded());

        REQUIRE(!Content::RenameAssetFolder(sourceFolder, destinationFolder));

        CHECK(!FileSystem::DirectoryExists(sourceFolder));
        CHECK(FileSystem::DirectoryExists(destinationFolder));
        CHECK(FileSystem::FileExists(destinationScenePath));
        CHECK(!FileSystem::DirectoryExists(GetSceneActorsFolder(sourceScenePath)));
        CHECK(FileSystem::FileExists(GetExternalActorPath(destinationScenePath, actorId)));
        CHECK(sceneAsset->GetPath() == destinationScenePath);
        AssetInfo info;
        REQUIRE(Content::GetAssetInfo(sceneId, info));
        CHECK(info.Path == destinationScenePath);
    }

    SECTION("Rename duplicated external actors scene moves cloned actor folder")
    {
        const Guid sceneId = ParseGuid("88888888888888888888888888888881");
        const Guid actorId = ParseGuid("88888888888888888888888888888882");
        const Guid cloneSceneId = ParseGuid("88888888888888888888888888888883");
        const String scenePath = GetTestScenePath(TEXT("DuplicateRenameSource"));
        const String clonePath = GetTestScenePath(TEXT("DuplicateRenameSource 0"));
        const String renamedPath = GetTestScenePath(TEXT("CopyOfDuplicateRenameSource"));
        CleanupTestSceneFiles(scenePath);
        CleanupTestSceneFiles(clonePath);
        CleanupTestSceneFiles(renamedPath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
            CleanupTestSceneFiles(clonePath);
            CleanupTestSceneFiles(renamedPath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);

        REQUIRE(!Content::CloneAssetFile(clonePath, scenePath, cloneSceneId));
        CHECK(FileSystem::DirectoryExists(GetSceneActorsFolder(clonePath)));
        CHECK(!FileSystem::DirectoryExists(GetSceneActorsFolder(renamedPath)));

        REQUIRE(!Content::RenameAsset(clonePath, renamedPath));

        CHECK(!FileSystem::DirectoryExists(GetSceneActorsFolder(clonePath)));
        CHECK(FileSystem::DirectoryExists(GetSceneActorsFolder(renamedPath)));

        Array<String> renamedActorFiles;
        REQUIRE(!FileSystem::DirectoryGetFiles(renamedActorFiles, GetExternalActorsFolder(renamedPath), TEXT("*.actor"), DirectorySearchOption::AllDirectories));
        CHECK(renamedActorFiles.Count() == 1);
    }

    SECTION("Delete external actors scene removes scene actors folder")
    {
        const Guid sceneId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa11");
        const Guid actorId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa12");
        const String scenePath = GetTestScenePath(TEXT("Delete"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);

        CHECK(FileSystem::DirectoryExists(GetSceneActorsFolder(scenePath)));
        REQUIRE(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));

        Content::DeleteAsset(scenePath);

        CHECK(!FileSystem::DirectoryExists(GetSceneActorsFolder(scenePath)));
    }

    SECTION("Delete loaded external actors scene removes scene actors folder")
    {
        const Guid sceneId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa21");
        const Guid actorId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa22");
        const String scenePath = GetTestScenePath(TEXT("DeleteLoaded"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);

        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        CHECK(FileSystem::DirectoryExists(GetSceneActorsFolder(scenePath)));
        REQUIRE(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));

        Content::DeleteAsset(sceneAsset);
        ObjectsRemovalService::Flush();

        CHECK(!FileSystem::DirectoryExists(GetSceneActorsFolder(scenePath)));
    }

    SECTION("Recompose ignores actors with missing parent chains")
    {
        const Guid sceneId = ParseGuid("22222222222222222222222222222221");
        const Guid validId = ParseGuid("22222222222222222222222222222222");
        const Guid missingParentId = ParseGuid("22222222222222222222222222222223");
        const Guid invalidId = ParseGuid("22222222222222222222222222222224");
        const Guid invalidChildId = ParseGuid("22222222222222222222222222222225");
        const String scenePath = GetTestScenePath(TEXT("InvalidParents"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, validId, sceneId, "Valid", 1024);
        WriteExternalActorFile(scenePath, invalidId, missingParentId, "Invalid", 2048);
        WriteExternalActorFile(scenePath, invalidChildId, invalidId, "Invalid Child", 1024);

        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        rapidjson_flax::StringBuffer unifiedBuffer;
        REQUIRE(!Level::SaveSceneAssetToBytes(sceneAsset, unifiedBuffer, nullptr, false));
        Content::UnloadAsset(sceneAsset);

        rapidjson_flax::Document unifiedDocument;
        ParseJson(unifiedDocument, unifiedBuffer);
        const rapidjson_flax::Value& unifiedData = GetDataArray(unifiedDocument);
        REQUIRE(unifiedData.Size() == 2);
        CHECK(ContainsObject(unifiedData, sceneId));
        CHECK(ContainsObject(unifiedData, validId));
        CHECK(!ContainsObject(unifiedData, invalidId));
        CHECK(!ContainsObject(unifiedData, invalidChildId));
    }

}

#endif
