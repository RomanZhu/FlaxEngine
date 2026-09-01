// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Log.h"
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
#include "Engine/Scripting/Scripting.h"
#if USE_EDITOR
#include "Engine/Content/Content.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/AssetObjectRegistry.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Level/Actors/EmptyActor.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Level/Scene/SceneAsset.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Level/Scripts/ModelPrefab.h"
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

    Guid GetSceneGuid(const String& scenePath)
    {
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (!AssetMeta::Load(scenePath + TEXT(".meta"), meta, diagnostic))
            return meta.ID;
        BytesContainer bytes;
        if (File::ReadAllBytes(scenePath, bytes))
            return Guid::Empty;
        rapidjson_flax::Document document;
        document.Parse(bytes.Get<char>(), bytes.Length());
        return document.HasParseError() ? Guid::Empty : JsonTools::GetGuid(document, "ID");
    }

    String GetExternalActorsFolder(const String& scenePath)
    {
        return SceneFragmentStore::GetScenePath(GetSceneGuid(scenePath));
    }

    String GetSceneFragmentsFolder(const String& scenePath)
    {
        return GetExternalActorsFolder(scenePath);
    }

    String GetExternalActorPath(const String& scenePath, const Guid& actorId)
    {
        return GetExternalActorsFolder(scenePath) / SceneFragmentStore::GetRelativeFragmentPath(SceneObject::MakeLocalFileId(actorId));
    }

    void CleanupTestSceneFiles(const String& scenePath)
    {
        Content::GetObjectRegistry()->RemoveTransientPackage(scenePath, nullptr);
        const Guid sceneGuid = GetSceneGuid(scenePath);
        if (sceneGuid.IsValid())
            FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuid));
        FileSystem::DeleteFile(scenePath);
        FileSystem::DeleteFile(scenePath + TEXT(".meta"));
        Array<String> refresh;
        refresh.Add(scenePath);
        AssetPipelineService::RefreshSources(refresh);
    }

    void EnsureDirectory(const StringView& directory)
    {
        if (directory.HasChars() && !FileSystem::DirectoryExists(directory))
            REQUIRE(!FileSystem::CreateDirectory(directory));
    }

    void RefreshTestScene(const String& scenePath)
    {
        Array<String> refresh;
        refresh.Add(scenePath);
        AssetPipelineDiagnostic diagnostic;
        const bool failed = AssetPipelineService::RefreshSources(refresh, true, diagnostic);
        if (failed)
            LOG(Error, "Test scene refresh failed: {0}", diagnostic.Message);
        REQUIRE(!failed);
    }

    void WriteTestSceneAsset(const String& scenePath, const Guid& sceneId, bool externalActors)
    {
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("sceneVersion");
        writer.Int(4);
        if (externalActors)
        {
            writer.JKEY("externalActors");
            writer.Bool(true);
        }
        writer.JKEY("objects");
        writer.StartArray();
        writer.StartObject();
        writer.JKEY("fileId");
        writer.Int64(1);
        writer.JKEY("type");
        writer.String("FlaxEngine.Scene", ARRAY_COUNT("FlaxEngine.Scene") - 1);
        if (externalActors)
        {
            writer.JKEY("useExternalActors");
            writer.Bool(true);
        }
        writer.EndObject();
        writer.EndArray(1);
        writer.EndObject();

        EnsureDirectory(StringUtils::GetDirectoryName(scenePath));
        REQUIRE(!File::WriteAllBytes(scenePath, buffer.GetString(), static_cast<int32>(buffer.GetSize())));
        AssetMeta meta;
        meta.ID = sceneId;
        meta.AssetType = SceneAsset::TypeName;
        meta.SourceKind = AssetSourceKind::TextDocument;
        meta.Processor.ID = TEXT("Flax.JsonDocument");
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        AssetPipelineDiagnostic diagnostic;
        REQUIRE(!AssetMeta::SaveAtomic(scenePath + TEXT(".meta"), meta, diagnostic));
        if (externalActors)
        {
            Array<SceneFragmentWrite> emptyFragments;
            String error;
            REQUIRE(!SceneFragmentStore::Save(sceneId, emptyFragments, error));
        }
        RefreshTestScene(scenePath);
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
        (void)engineBuild;
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartArray();
        writer.StartObject();
        const int64 actorLocalId = SceneObject::MakeLocalFileId(actorId);
        const Guid sceneGuid = GetSceneGuid(scenePath);
        writer.JKEY("fileId");
        writer.Int64(actorLocalId);
        writer.JKEY("type");
        writer.String("FlaxEngine.EmptyActor", ARRAY_COUNT("FlaxEngine.EmptyActor") - 1);
        writer.JKEY("parentFileId");
        writer.Int64(parentId == sceneGuid ? 1 : SceneObject::MakeLocalFileId(parentId));
        writer.JKEY("orderInParent");
        writer.Int64(orderInParent);
        writer.JKEY("name");
        writer.String(name, StringUtils::Length(name));
        writer.EndObject();
        writer.EndArray(1);
        Array<SceneFragmentWrite> writes;
        SceneFragmentIndex existingIndex;
        Array<Array<byte>> existingFragments;
        String error;
        if (!SceneFragmentStore::Load(sceneGuid, existingIndex, existingFragments, error))
        {
            for (int32 i = 0; i < existingIndex.Fragments.Count(); i++)
            {
                rapidjson_flax::Document existing;
                existing.Parse(reinterpret_cast<const char*>(existingFragments[i].Get()), existingFragments[i].Count());
                const auto payload = existing.FindMember("payload");
                const auto contained = existing.FindMember("containedLocalIds");
                REQUIRE(payload != existing.MemberEnd());
                REQUIRE(contained != existing.MemberEnd());
                rapidjson_flax::StringBuffer payloadBytes;
                PrettyJsonWriter payloadWriter(payloadBytes);
                payload->value.Accept(payloadWriter.GetWriter());
                SceneFragmentWrite write;
                write.RootActorLocalId = existingIndex.Fragments[i].RootActorLocalId;
                write.SerializerVersion = existingIndex.Fragments[i].SerializerVersion;
                for (const rapidjson_flax::Value& id : contained->value.GetArray())
                    write.ContainedLocalIds.Add(id.GetInt64());
                write.Payload.Set(reinterpret_cast<const byte*>(payloadBytes.GetString()), static_cast<int32>(payloadBytes.GetSize()));
                writes.Add(MoveTemp(write));
            }
        }
        SceneFragmentWrite write;
        write.RootActorLocalId = actorLocalId;
        write.ContainedLocalIds.Add(actorLocalId);
        write.Payload.Set(reinterpret_cast<const byte*>(buffer.GetString()), static_cast<int32>(buffer.GetSize()));
        writes.Add(MoveTemp(write));
        REQUIRE(!SceneFragmentStore::Save(sceneGuid, writes, error));
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
        if (data != document.MemberEnd())
        {
            REQUIRE(data->value.IsArray());
            return data->value;
        }
        const auto payload = document.FindMember("payload");
        if (payload != document.MemberEnd())
        {
            REQUIRE(payload->value.IsArray());
            return payload->value;
        }
        const auto objects = document.FindMember("objects");
        REQUIRE(objects != document.MemberEnd());
        REQUIRE(objects->value.IsArray());
        return objects->value;
    }

    rapidjson_flax::Value& GetDataArray(rapidjson_flax::Document& document)
    {
        return const_cast<rapidjson_flax::Value&>(GetDataArray(static_cast<const rapidjson_flax::Document&>(document)));
    }

    bool ContainsObject(const rapidjson_flax::Value& data, const Guid& id)
    {
        for (rapidjson::SizeType i = 0; i < data.Size(); i++)
        {
            if (JsonTools::GetGuid(data[i], "ID") == id)
                return true;
            auto fileId = data[i].FindMember("fileId");
            if (fileId == data[i].MemberEnd())
                fileId = data[i].FindMember("FileId");
            if (fileId != data[i].MemberEnd() && fileId->value.IsInt64() &&
                fileId->value.GetInt64() == SceneObject::MakeLocalFileId(id))
                return true;
        }
        return false;
    }

    int64 GetObjectLocalId(const rapidjson_flax::Value& object)
    {
        auto fileId = object.FindMember("fileId");
        if (fileId == object.MemberEnd())
            fileId = object.FindMember("FileId");
        if (fileId != object.MemberEnd() && fileId->value.IsInt64())
            return fileId->value.GetInt64();
        const Guid id = JsonTools::GetGuid(object, "ID");
        return id.IsValid() ? SceneObject::MakeLocalFileId(id) : 0;
    }

    int64 GetParentLocalId(const rapidjson_flax::Value& object)
    {
        auto parent = object.FindMember("parentFileId");
        if (parent == object.MemberEnd())
            parent = object.FindMember("ParentFileId");
        if (parent != object.MemberEnd() && parent->value.IsInt64())
            return parent->value.GetInt64();
        const Guid id = JsonTools::GetGuid(object, "ParentID");
        return id.IsValid() ? SceneObject::MakeLocalFileId(id) : 0;
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
    SECTION("Placed model identity survives fragment save and reopen")
    {
        const Guid sceneId = ParseGuid("10111111111111111111111111111111");
        const Guid actorId = ParseGuid("10111111111111111111111111111112");
        AssetReference<Model> model = Content::LoadAsync<Model>(Globals::EngineContentFolder / TEXT("Editor/Primitives/Cube.flax"));
        REQUIRE(model);
        REQUIRE(!model->WaitForLoaded());
        const Guid modelObject = model.GetID();
        REQUIRE(modelObject.IsValid());
        const String scenePath = GetTestScenePath(TEXT("PlacedModelPersistence"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        scene->UseExternalActors = true;
        StaticModel* actor = StaticModel::Spawn(ScriptingObject::SpawnParams(actorId, StaticModel::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Placed Model"));
        actor->Model = model.Get();
        REQUIRE(actor->Model.GetID() == modelObject);
        actor->SetParent(scene);

        REQUIRE(!Level::SaveScene(scene));
        rapidjson_flax::Document fragmentDocument;
        ParseJsonFile(fragmentDocument, GetExternalActorPath(scenePath, actorId));
        const rapidjson_flax::Value& fragmentObjects = GetDataArray(fragmentDocument);
        REQUIRE(fragmentObjects.Size() == 1);
        REQUIRE(fragmentObjects[0].HasMember("Model"));
        const rapidjson_flax::Value& fragmentModel = fragmentObjects[0]["Model"];
        REQUIRE(fragmentModel.IsString());
        Guid serializedModel;
        REQUIRE_FALSE(Guid::Parse(StringAnsiView(fragmentModel.GetString(), fragmentModel.GetStringLength()), serializedModel));
        CHECK(serializedModel == modelObject);

        RefreshTestScene(scenePath);
        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        rapidjson_flax::StringBuffer reopenedBytes;
        REQUIRE(!Level::SaveSceneAssetToBytes(sceneAsset, reopenedBytes, nullptr, false));
        Content::UnloadAsset(sceneAsset);

        scene->DeleteObject();
        ObjectsRemovalService::Flush();
        scene = nullptr;
        BytesContainer bytes(reinterpret_cast<const byte*>(reopenedBytes.GetString()), static_cast<int32>(reopenedBytes.GetSize()));
        Scene* reopened = Level::LoadSceneFromBytes(bytes);
        REQUIRE(reopened);
        SCOPE_EXIT
        {
            Level::UnloadScene(reopened);
        };
        StaticModel* reopenedActor = reopened->FindActor<StaticModel>(TEXT("Placed Model"));
        REQUIRE(reopenedActor);
        CHECK(reopenedActor->GetParent() == reopened);
        CHECK(reopenedActor->Model.GetID() == modelObject);
    }

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

        Array<String> lifecycleWarningsAndErrors;
        Delegate<LogType, const StringView&>::FunctionType logHandler = [&lifecycleWarningsAndErrors](LogType type, const StringView& message)
        {
            if (type == LogType::Warning || type == LogType::Error || type == LogType::Fatal)
                lifecycleWarningsAndErrors.Add(String(message));
        };
        Log::Logger::OnMessage.Bind(logHandler);
        SCOPE_EXIT { Log::Logger::OnMessage.Unbind(logHandler); };

        REQUIRE(!Level::SaveScene(scene));
        RefreshTestScene(scenePath);

        Array<String> actorFiles;
        REQUIRE(!FileSystem::DirectoryGetFiles(actorFiles, GetExternalActorsFolder(scenePath), TEXT("*.sceneactor"), DirectorySearchOption::AllDirectories));
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
        REQUIRE(JsonTools::GetBool(sceneDocument, "externalActors", false));
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
        CHECK_FALSE(unifiedDocument.HasMember("ExternalActors"));
        CHECK_FALSE(unifiedDocument.HasMember("externalActors"));
        const rapidjson_flax::Value& unifiedData = GetDataArray(unifiedDocument);
        REQUIRE(unifiedData.Size() == 4);
        CHECK(GetObjectLocalId(unifiedData[0]) == 1);
        CHECK(ContainsObject(unifiedData, siblingId));
        CHECK(ContainsObject(unifiedData, parentId));
        CHECK(ContainsObject(unifiedData, childId));
        CHECK(externalActorFiles.IsEmpty());
        CHECK(lifecycleWarningsAndErrors.IsEmpty());

        Array<int64> rootChildIds;
        for (rapidjson::SizeType i = 0; i < unifiedData.Size(); i++)
        {
            if (GetParentLocalId(unifiedData[i]) == 1)
                rootChildIds.Add(GetObjectLocalId(unifiedData[i]));
        }
        REQUIRE(rootChildIds.Count() == 2);
        CHECK(rootChildIds[0] == SceneObject::MakeLocalFileId(siblingId));
        CHECK(rootChildIds[1] == SceneObject::MakeLocalFileId(parentId));
    }

    SECTION("Reorder changes only the moved external actor key")
    {
        const Guid sceneId = ParseGuid("12111111111111111111111111111111");
        const Guid parentId = ParseGuid("12111111111111111111111111111112");
        const Guid childAId = ParseGuid("12111111111111111111111111111113");
        const Guid childBId = ParseGuid("12111111111111111111111111111114");
        const Guid childCId = ParseGuid("12111111111111111111111111111115");
        const Guid childDId = ParseGuid("12111111111111111111111111111116");
        const Guid scriptAId = ParseGuid("12111111111111111111111111111117");
        const Guid scriptBId = ParseGuid("12111111111111111111111111111118");
        const Guid scriptCId = ParseGuid("12111111111111111111111111111119");
        const Guid scriptDId = ParseGuid("1211111111111111111111111111111a");
        const String scenePath = GetTestScenePath(TEXT("SiblingOrderKey"));
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

        EmptyActor* parent = EmptyActor::Spawn(ScriptingObject::SpawnParams(parentId, EmptyActor::TypeInitializer));
        REQUIRE(parent);
        parent->SetParent(scene);
        EmptyActor* children[] =
        {
            EmptyActor::Spawn(ScriptingObject::SpawnParams(childAId, EmptyActor::TypeInitializer)),
            EmptyActor::Spawn(ScriptingObject::SpawnParams(childBId, EmptyActor::TypeInitializer)),
            EmptyActor::Spawn(ScriptingObject::SpawnParams(childCId, EmptyActor::TypeInitializer)),
            EmptyActor::Spawn(ScriptingObject::SpawnParams(childDId, EmptyActor::TypeInitializer)),
        };
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
        {
            REQUIRE(children[i]);
            children[i]->SetParent(parent);
            children[i]->SetExternalOrderInParent(1024 + i);
        }

        ModelPrefab* scripts[] =
        {
            ModelPrefab::Spawn(ScriptingObject::SpawnParams(scriptAId, ModelPrefab::TypeInitializer)),
            ModelPrefab::Spawn(ScriptingObject::SpawnParams(scriptBId, ModelPrefab::TypeInitializer)),
            ModelPrefab::Spawn(ScriptingObject::SpawnParams(scriptCId, ModelPrefab::TypeInitializer)),
            ModelPrefab::Spawn(ScriptingObject::SpawnParams(scriptDId, ModelPrefab::TypeInitializer)),
        };
        for (int32 i = 0; i < ARRAY_COUNT(scripts); i++)
        {
            REQUIRE(scripts[i]);
            scripts[i]->SetParent(parent);
            scripts[i]->SetExternalOrderInParent(1024 + i);
        }

        REQUIRE(!Level::SaveScene(scene));
        BytesContainer originalChildFiles[ARRAY_COUNT(children)];
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
            ReadFileBytes(GetExternalActorPath(scenePath, children[i]->GetID()), originalChildFiles[i]);

        children[3]->SetOrderInParent(1);
        scripts[3]->SetOrderInParent(1);

        Actor* expectedChildren[] = { children[0], children[3], children[1], children[2] };
        REQUIRE(parent->Children.Count() == ARRAY_COUNT(expectedChildren));
        for (int32 i = 0; i < ARRAY_COUNT(expectedChildren); i++)
            CHECK(parent->Children[i] == expectedChildren[i]);
        Script* expectedScripts[] = { scripts[0], scripts[3], scripts[1], scripts[2] };
        REQUIRE(parent->Scripts.Count() == ARRAY_COUNT(expectedScripts));
        for (int32 i = 0; i < ARRAY_COUNT(expectedScripts); i++)
            CHECK(parent->Scripts[i] == expectedScripts[i]);

        REQUIRE(!Level::SaveScene(scene));
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
        {
            BytesContainer currentFile;
            ReadFileBytes(GetExternalActorPath(scenePath, children[i]->GetID()), currentFile);
            CHECK(AreBytesEqual(originalChildFiles[i], currentFile) == (i != 3));

            rapidjson_flax::Document actorDocument;
            ParseJsonFile(actorDocument, GetExternalActorPath(scenePath, children[i]->GetID()));
            const rapidjson_flax::Value& actorData = GetDataArray(actorDocument);
            REQUIRE(actorData.Size() >= 1);
            if (i == 3)
            {
                CHECK(actorData[0].HasMember("siblingOrderKey"));
                CHECK(!actorData[0].HasMember("orderInParent"));
            }
            else
            {
                REQUIRE(actorData[0].HasMember("orderInParent"));
                CHECK(actorData[0]["orderInParent"].GetInt64() == 1024 + i);
                CHECK(!actorData[0].HasMember("siblingOrderKey"));
            }
        }

        rapidjson_flax::Document parentDocument;
        ParseJsonFile(parentDocument, GetExternalActorPath(scenePath, parentId));
        const rapidjson_flax::Value& parentData = GetDataArray(parentDocument);
        for (int32 i = 0; i < ARRAY_COUNT(scripts); i++)
        {
            const rapidjson_flax::Value* scriptData = nullptr;
            for (rapidjson::SizeType j = 1; j < parentData.Size(); j++)
            {
                if (GetObjectLocalId(parentData[j]) == scripts[i]->GetLocalFileId())
                {
                    scriptData = &parentData[j];
                    break;
                }
            }
            REQUIRE(scriptData);
            if (i == 3)
            {
                CHECK(scriptData->HasMember("siblingOrderKey"));
                CHECK(!scriptData->HasMember("orderInParent"));
            }
            else
            {
                REQUIRE(scriptData->HasMember("orderInParent"));
                CHECK((*scriptData)["orderInParent"].GetInt64() == 1024 + i);
                CHECK(!scriptData->HasMember("siblingOrderKey"));
            }
        }

        REQUIRE(!Level::ApplyExternalActorsSiblingKeys(scene));
        for (EmptyActor* child : children)
        {
            rapidjson_flax::Document actorDocument;
            ParseJsonFile(actorDocument, GetExternalActorPath(scenePath, child->GetID()));
            const rapidjson_flax::Value& actorData = GetDataArray(actorDocument);
            REQUIRE(actorData.Size() >= 1);
            CHECK(actorData[0].HasMember("siblingOrderKey"));
            CHECK(!actorData[0].HasMember("orderInParent"));
        }

        rapidjson_flax::Document migratedParentDocument;
        ParseJsonFile(migratedParentDocument, GetExternalActorPath(scenePath, parentId));
        const rapidjson_flax::Value& migratedParentData = GetDataArray(migratedParentDocument);
        for (rapidjson::SizeType i = 0; i < migratedParentData.Size(); i++)
        {
            if (migratedParentData[i].HasMember("parentFileId"))
            {
                CHECK(migratedParentData[i].HasMember("siblingOrderKey"));
                CHECK(!migratedParentData[i].HasMember("orderInParent"));
            }
        }
    }

    SECTION("Temporary prefab-style rotations preserve sibling keys")
    {
        const Guid sceneId = ParseGuid("13111111111111111111111111111111");
        const Guid parentId = ParseGuid("13111111111111111111111111111112");
        const Guid childIds[] =
        {
            ParseGuid("13111111111111111111111111111113"),
            ParseGuid("13111111111111111111111111111114"),
            ParseGuid("13111111111111111111111111111115"),
        };
        const String scenePath = GetTestScenePath(TEXT("TemporarySiblingRotation"));
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

        EmptyActor* parent = EmptyActor::Spawn(ScriptingObject::SpawnParams(parentId, EmptyActor::TypeInitializer));
        REQUIRE(parent);
        parent->SetParent(scene);
        EmptyActor* children[ARRAY_COUNT(childIds)];
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
        {
            children[i] = EmptyActor::Spawn(ScriptingObject::SpawnParams(childIds[i], EmptyActor::TypeInitializer));
            REQUIRE(children[i]);
            children[i]->SetParent(parent);
            children[i]->SetExternalOrderInParent(1024 + i * 1024);
        }

        REQUIRE(!Level::SaveScene(scene));
        BytesContainer originalFiles[ARRAY_COUNT(children)];
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
            ReadFileBytes(GetExternalActorPath(scenePath, children[i]->GetID()), originalFiles[i]);

        // Prefab synchronization uses source indices that can include removed children.
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
            children[i]->SetOrderInParent(10 + i);
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
            CHECK(parent->Children[i] == children[i]);

        REQUIRE(!Level::SaveScene(scene));
        for (int32 i = 0; i < ARRAY_COUNT(children); i++)
        {
            BytesContainer currentFile;
            ReadFileBytes(GetExternalActorPath(scenePath, children[i]->GetID()), currentFile);
            CHECK(AreBytesEqual(originalFiles[i], currentFile));
        }
    }

    SECTION("Scene fragment path is keyed by scene GUID")
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

        CHECK(GetSceneFragmentsFolder(scenePathA) != GetSceneFragmentsFolder(scenePathB));
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
        actor->SetExternalOrderInParent(512);

        EmptyActor* child = EmptyActor::Spawn(ScriptingObject::SpawnParams(childId, EmptyActor::TypeInitializer));
        REQUIRE(child);
        child->SetName(TEXT("Child"));
        child->SetParent(actor);
        child->SetExternalOrderInParent(1024);

        rapidjson_flax::StringBuffer snapshotBuffer;
        REQUIRE(!Level::SaveSceneToBytes(scene, snapshotBuffer, false));

        rapidjson_flax::Document snapshotDocument;
        ParseJson(snapshotDocument, snapshotBuffer);
        const rapidjson_flax::Value& snapshotData = GetDataArray(snapshotDocument);
        REQUIRE(snapshotData.Size() == 3);
        CHECK(GetObjectLocalId(snapshotData[0]) == 1);
        CHECK(ContainsObject(snapshotData, actorId));
        CHECK(ContainsObject(snapshotData, childId));
        const rapidjson_flax::Value* actorData = nullptr;
        const rapidjson_flax::Value* childData = nullptr;
        for (rapidjson::SizeType i = 0; i < snapshotData.Size(); i++)
        {
            const int64 localId = GetObjectLocalId(snapshotData[i]);
            if (localId == SceneObject::MakeLocalFileId(actorId))
                actorData = &snapshotData[i];
            else if (localId == SceneObject::MakeLocalFileId(childId))
                childData = &snapshotData[i];
        }
        REQUIRE(actorData);
        REQUIRE(childData);
        REQUIRE(actorData->HasMember("orderInParent"));
        REQUIRE(childData->HasMember("orderInParent"));
        CHECK((*actorData)["orderInParent"].GetInt64() == 512);
        CHECK((*childData)["orderInParent"].GetInt64() == 1024);
        CHECK(!JsonTools::GetBool(snapshotDocument, "externalActors", false));
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
        BytesContainer beforeSave;
        BytesContainer afterSave;
        ReadFileBytes(actorPath, beforeSave);
        REQUIRE(!Level::SaveScene(scene));
        ReadFileBytes(actorPath, afterSave);

        CHECK(AreBytesEqual(beforeSave, afterSave));
    }

    SECTION("Save rejects externally modified indexed fragments without mutation")
    {
        const Guid sceneId = ParseGuid("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1");
        const Guid actorId = ParseGuid("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2");
        const String scenePath = GetTestScenePath(TEXT("UlpSave"));
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
        actor->SetParent(scene);
        actor->SetLocalPosition(Vector3(1.0, 2.0, 3.0));

        REQUIRE(!Level::SaveScene(scene));
        const String actorPath = GetExternalActorPath(scenePath, actorId);
        BytesContainer originalBytes;
        ReadFileBytes(actorPath, originalBytes);
        rapidjson_flax::Document actorDocument;
        ParseJsonFile(actorDocument, actorPath);
        auto& actorData = GetDataArray(actorDocument)[0];
        REQUIRE(actorData.HasMember("Transform"));
        auto& translationX = actorData["Transform"]["Translation"]["X"];
        REQUIRE(translationX.IsDouble());
        translationX.SetDouble(1.0000000000000002);

        rapidjson_flax::StringBuffer oneUlpBuffer;
        PrettyJsonWriter oneUlpWriter(oneUlpBuffer);
        actorDocument.Accept(oneUlpWriter.GetWriter());
        REQUIRE(!File::WriteAllBytes(actorPath, oneUlpBuffer.GetString(), static_cast<int32>(oneUlpBuffer.GetSize())));

        BytesContainer externallyModifiedBytes;
        BytesContainer afterRejectedSave;
        ReadFileBytes(actorPath, externallyModifiedBytes);
        REQUIRE(Level::SaveScene(scene));
        ReadFileBytes(actorPath, afterRejectedSave);
        CHECK(AreBytesEqual(externallyModifiedBytes, afterRejectedSave));
        CHECK_FALSE(AreBytesEqual(originalBytes, afterRejectedSave));
    }

    SECTION("Convert internal actors scene without adjacent backup files")
    {
        const Guid sceneId = ParseGuid("76767676767676767676767676767671");
        const Guid actorId = ParseGuid("76767676767676767676767676767672");
        const String scenePath = GetTestScenePath(TEXT("Externalize"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, false);

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        CHECK_FALSE(scene->UseExternalActors);
        EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Actor"));
        actor->SetParent(scene);

        REQUIRE(!Level::ConvertSceneToExternalActors(scene));
        CHECK(scene->UseExternalActors);
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
        CHECK(actor->GetGlobalObjectId().SourceAsset.Value == sceneId);
        CHECK(actor->GetGlobalObjectId().LocalFileId == actor->GetLocalFileId());

        rapidjson_flax::Document externalizedDocument;
        ParseJsonFile(externalizedDocument, scenePath);
        CHECK(JsonTools::GetInt(externalizedDocument, "sceneVersion", 0) == 4);
        CHECK(JsonTools::GetBool(externalizedDocument, "externalActors", false));
        CHECK(GetDataArray(externalizedDocument).Size() == 1);

        Array<String> backups;
        const String backupPattern = String(StringUtils::GetFileName(scenePath)) + TEXT(".*.bak");
        const String sceneDirectory(StringUtils::GetDirectoryName(scenePath));
        REQUIRE_FALSE(FileSystem::DirectoryGetFiles(backups, sceneDirectory, *backupPattern,
            DirectorySearchOption::TopDirectoryOnly));
        CHECK(backups.IsEmpty());
    }

    SECTION("Failed external actor conversion restores in-memory and source state")
    {
        const Guid sceneId = ParseGuid("75757575757575757575757575757571");
        const Guid actorId = ParseGuid("75757575757575757575757575757572");
        const String scenePath = GetTestScenePath(TEXT("ExternalizeFailure"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, false);
        EnsureDirectory(SceneFragmentStore::GetScenePath(sceneId));

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
        REQUIRE(actor);
        actor->SetParent(scene);
        BytesContainer before;
        BytesContainer after;
        ReadFileBytes(scenePath, before);

        REQUIRE(Level::ConvertSceneToExternalActors(scene));
        CHECK_FALSE(scene->UseExternalActors);
        ReadFileBytes(scenePath, after);
        CHECK(AreBytesEqual(before, after));
        CHECK_FALSE(FileSystem::FileExists(SceneFragmentStore::GetIndexPath(sceneId)));
    }

    SECTION("Storage transition rejects an unsupported authored scene version")
    {
        const Guid sceneId = ParseGuid("74747474747474747474747474747471");
        const String scenePath = GetTestScenePath(TEXT("UnsupportedTransition"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, false);
        const char unsupported[] = R"({"sceneVersion":3,"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})";
        REQUIRE(!File::WriteAllBytes(scenePath, unsupported, ARRAY_COUNT(unsupported) - 1));

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        BytesContainer before;
        BytesContainer after;
        ReadFileBytes(scenePath, before);

        REQUIRE(Level::ConvertSceneToExternalActors(scene));
        CHECK_FALSE(scene->UseExternalActors);
        CHECK_FALSE(FileSystem::DirectoryExists(SceneFragmentStore::GetScenePath(sceneId)));
        ReadFileBytes(scenePath, after);
        CHECK(AreBytesEqual(before, after));
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
        CHECK(!FileSystem::DirectoryExists(GetSceneFragmentsFolder(scenePath)));

        rapidjson_flax::Document sceneDocument;
        ParseJsonFile(sceneDocument, scenePath);
        CHECK(!JsonTools::GetBool(sceneDocument, "externalActors", false));
        const rapidjson_flax::Value& savedData = GetDataArray(sceneDocument);
        REQUIRE(savedData.Size() == 3);
        CHECK(!JsonTools::GetBool(savedData[0], "useExternalActors", false));
        CHECK(GetObjectLocalId(savedData[0]) == 1);
        CHECK(ContainsObject(savedData, actorId));
        CHECK(ContainsObject(savedData, childId));
        CHECK(!ContainsObject(savedData, staleId));
    }

    SECTION("Failed internalization preserves the external source and fragment store")
    {
        const Guid sceneId = ParseGuid("73737373737373737373737373737371");
        const Guid actorId = ParseGuid("73737373737373737373737373737372");
        const String scenePath = GetTestScenePath(TEXT("InternalizeFailure"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);
        const String orphanPath = SceneFragmentStore::GetScenePath(sceneId) / TEXT("orphan.sceneactor");
        const char orphan[] = "{}";
        REQUIRE(!File::WriteAllBytes(orphanPath, orphan, ARRAY_COUNT(orphan) - 1));

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            scene->DeleteObject();
        };
        scene->UseExternalActors = true;
        EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
        REQUIRE(actor);
        actor->SetParent(scene);

        BytesContainer sourceBefore;
        BytesContainer indexBefore;
        BytesContainer fragmentBefore;
        ReadFileBytes(scenePath, sourceBefore);
        ReadFileBytes(SceneFragmentStore::GetIndexPath(sceneId), indexBefore);
        ReadFileBytes(GetExternalActorPath(scenePath, actorId), fragmentBefore);

        REQUIRE(Level::ConvertSceneToInternalActors(scene));
        CHECK(scene->UseExternalActors);
        CHECK(FileSystem::FileExists(orphanPath));
        BytesContainer after;
        ReadFileBytes(scenePath, after);
        CHECK(AreBytesEqual(sourceBefore, after));
        ReadFileBytes(SceneFragmentStore::GetIndexPath(sceneId), after);
        CHECK(AreBytesEqual(indexBefore, after));
        ReadFileBytes(GetExternalActorPath(scenePath, actorId), after);
        CHECK(AreBytesEqual(fragmentBefore, after));
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
        RefreshTestScene(scenePath);

        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        rapidjson_flax::StringBuffer unifiedBuffer;
        REQUIRE(!Level::SaveSceneAssetToBytes(sceneAsset, unifiedBuffer, nullptr, false));
        Content::UnloadAsset(sceneAsset);

        rapidjson_flax::Document unifiedDocument;
        ParseJson(unifiedDocument, unifiedBuffer);
        const rapidjson_flax::Value& unifiedData = GetDataArray(unifiedDocument);
        REQUIRE(unifiedData.Size() == 3);
        CHECK(GetObjectLocalId(unifiedData[0]) == 1);
        CHECK(GetObjectLocalId(unifiedData[1]) == SceneObject::MakeLocalFileId(parentId));
        CHECK(GetObjectLocalId(unifiedData[2]) == SceneObject::MakeLocalFileId(childId));
    }

    SECTION("Clone external actors scene copies and remaps actor files")
    {
        const Guid sceneId = ParseGuid("33333333333333333333333333333331");
        const Guid parentId = ParseGuid("33333333333333333333333333333332");
        const Guid childId = ParseGuid("33333333333333333333333333333333");
        Guid cloneSceneId;
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
        RefreshTestScene(scenePath);

        REQUIRE(!AssetOperationService::CopyAsset(scenePath, clonePath, cloneSceneId));
        REQUIRE(cloneSceneId.IsValid());
        CHECK(cloneSceneId != sceneId);

        SceneFragmentIndex sourceIndex;
        SceneFragmentIndex cloneIndex;
        Array<Array<byte>> sourceFragments;
        Array<Array<byte>> clonedFragments;
        String fragmentError;
        REQUIRE(!SceneFragmentStore::Load(sceneId, sourceIndex, sourceFragments, fragmentError));
        REQUIRE(!SceneFragmentStore::Load(cloneSceneId, cloneIndex, clonedFragments, fragmentError));
        CHECK(sourceIndex.OwnerSceneGuid == sceneId);
        CHECK(cloneIndex.OwnerSceneGuid == cloneSceneId);
        REQUIRE(sourceIndex.Fragments.Count() == 2);
        REQUIRE(cloneIndex.Fragments.Count() == 2);
        REQUIRE(sourceFragments.Count() == 2);
        REQUIRE(clonedFragments.Count() == 2);
        for (int32 i = 0; i < cloneIndex.Fragments.Count(); i++)
        {
            CHECK(cloneIndex.Fragments[i].RootActorLocalId == sourceIndex.Fragments[i].RootActorLocalId);
            rapidjson_flax::Document sourceFragment;
            sourceFragment.Parse(reinterpret_cast<const char*>(sourceFragments[i].Get()), sourceFragments[i].Count());
            REQUIRE_FALSE(sourceFragment.HasParseError());
            CHECK(JsonTools::GetGuid(sourceFragment, "ownerSceneGuid") == sceneId);
            rapidjson_flax::Document clonedFragment;
            clonedFragment.Parse(reinterpret_cast<const char*>(clonedFragments[i].Get()), clonedFragments[i].Count());
            REQUIRE_FALSE(clonedFragment.HasParseError());
            CHECK(JsonTools::GetGuid(clonedFragment, "ownerSceneGuid") == cloneSceneId);
        }

        rapidjson_flax::Document cloneSceneDocument;
        ParseJsonFile(cloneSceneDocument, clonePath);
        const rapidjson_flax::Value& cloneSceneData = GetDataArray(cloneSceneDocument);
        REQUIRE(cloneSceneData.Size() == 1);
        CHECK(GetSceneGuid(clonePath) == cloneSceneId);
        CHECK(GetObjectLocalId(cloneSceneData[0]) == 1);

        Array<String> cloneActorFiles;
        REQUIRE(!FileSystem::DirectoryGetFiles(cloneActorFiles, GetExternalActorsFolder(clonePath), TEXT("*.sceneactor"), DirectorySearchOption::AllDirectories));
        REQUIRE(cloneActorFiles.Count() == 2);

        Array<int64> cloneActorIds;
        Array<int64> cloneParentIds;
        for (const String& file : cloneActorFiles)
        {
            rapidjson_flax::Document actorDocument;
            ParseJsonFile(actorDocument, file);
            const rapidjson_flax::Value& actorData = GetDataArray(actorDocument);
            REQUIRE(actorData.Size() == 1);
            cloneActorIds.Add(GetObjectLocalId(actorData[0]));
            cloneParentIds.Add(GetParentLocalId(actorData[0]));
        }

        const int64 parentLocalId = SceneObject::MakeLocalFileId(parentId);
        CHECK(cloneActorIds.Contains(parentLocalId));
        CHECK(cloneActorIds.Contains(SceneObject::MakeLocalFileId(childId)));
        CHECK(cloneParentIds.Contains(1));
        CHECK(cloneParentIds.Contains(parentLocalId));
        const int32 clonedParentIndex = cloneActorIds.Find(parentLocalId);
        const int32 clonedChildIndex = cloneActorIds.Find(SceneObject::MakeLocalFileId(childId));
        REQUIRE(clonedParentIndex != -1);
        REQUIRE(clonedChildIndex != -1);
        CHECK(cloneParentIds[clonedParentIndex] == 1);
        CHECK(cloneParentIds[clonedChildIndex] == parentLocalId);
    }

}

TEST_CASE("LegacyExternalActorsClone")
{

    SECTION("Clone external actors scene rejects before creating destination data")
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

        REQUIRE(Content::CloneAssetFile(clonePath, scenePath, cloneSceneId));

        CHECK(!FileSystem::FileExists(clonePath));
        CHECK(!FileSystem::DirectoryExists(SceneFragmentStore::GetScenePath(cloneSceneId)));
        CHECK(FileSystem::FileExists(scenePath));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
    }

    SECTION("Clone external actors scene preserves an existing destination")
    {
        const Guid sceneId = ParseGuid("99999999999999999999999999999994");
        const Guid actorId = ParseGuid("99999999999999999999999999999995");
        const Guid destinationSceneId = ParseGuid("99999999999999999999999999999996");
        const Guid destinationActorId = ParseGuid("99999999999999999999999999999997");
        const String scenePath = GetTestScenePath(TEXT("CloneOverwriteSource"));
        const String destinationPath = GetTestScenePath(TEXT("CloneOverwriteTarget"));
        CleanupTestSceneFiles(scenePath);
        CleanupTestSceneFiles(destinationPath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
            CleanupTestSceneFiles(destinationPath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "SourceActor", 1024);
        WriteTestSceneAsset(destinationPath, destinationSceneId, true);
        WriteExternalActorFile(destinationPath, destinationActorId, destinationSceneId, "DestinationActor", 1024);
        BytesContainer destinationSceneBytes;
        BytesContainer destinationActorBytes;
        ReadFileBytes(destinationPath, destinationSceneBytes);
        ReadFileBytes(GetExternalActorPath(destinationPath, destinationActorId), destinationActorBytes);

        REQUIRE(Content::CloneAssetFile(destinationPath, scenePath, destinationSceneId, true));

        BytesContainer preservedSceneBytes;
        BytesContainer preservedActorBytes;
        ReadFileBytes(destinationPath, preservedSceneBytes);
        ReadFileBytes(GetExternalActorPath(destinationPath, destinationActorId), preservedActorBytes);
        CHECK(AreBytesEqual(destinationSceneBytes, preservedSceneBytes));
        CHECK(AreBytesEqual(destinationActorBytes, preservedActorBytes));
    }

}

TEST_CASE("LegacyContentAssetFileOperations")
{

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

}

TEST_CASE("ExternalActorsSceneStorage Lifecycle")
{

    SECTION("Create update duplicate and delete preserve persistent identities")
    {
        const Guid sceneId = ParseGuid("45454545454545454545454545454511");
        const Guid actorId = ParseGuid("45454545454545454545454545454512");
        const String scenePath = GetTestScenePath(TEXT("ActorLifecycle"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);

        AssetReference<Model> model = Content::LoadAsync<Model>(Globals::EngineContentFolder / TEXT("Editor/Primitives/Cube.flax"));
        REQUIRE(model);
        REQUIRE(!model->WaitForLoaded());
        const Guid modelId = model.GetID();
        REQUIRE(modelId.IsValid());

        Scene* scene = Scene::Spawn(ScriptingObject::SpawnParams(sceneId, Scene::TypeInitializer));
        REQUIRE(scene);
        SCOPE_EXIT
        {
            if (scene)
                scene->DeleteObject();
        };
        scene->UseExternalActors = true;

        StaticModel* actor = StaticModel::Spawn(ScriptingObject::SpawnParams(actorId, StaticModel::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Original"));
        actor->Model = model.Get();
        actor->SetParent(scene);
        const int64 actorLocalId = actor->GetLocalFileId();

        REQUIRE(!Level::SaveScene(scene));
        SceneFragmentIndex createdIndex;
        Array<Array<byte>> createdFragments;
        String error;
        REQUIRE(!SceneFragmentStore::Load(sceneId, createdIndex, createdFragments, error));
        REQUIRE(createdIndex.Fragments.Count() == 1);
        REQUIRE(createdFragments.Count() == 1);
        CHECK(createdIndex.Fragments[0].RootActorLocalId == actorLocalId);
        CHECK(actor->GetGlobalObjectId().SourceAsset.Value == sceneId);
        CHECK(actor->GetGlobalObjectId().LocalFileId == actorLocalId);

        actor->SetName(TEXT("Updated"));
        actor->SetPosition(Vector3(10.0f, 20.0f, 30.0f));
        REQUIRE(!Level::SaveScene(scene));
        SceneFragmentIndex updatedIndex;
        Array<Array<byte>> updatedFragments;
        REQUIRE(!SceneFragmentStore::Load(sceneId, updatedIndex, updatedFragments, error));
        REQUIRE(updatedFragments.Count() == 1);
        CHECK(updatedIndex.IndexRevision == createdIndex.IndexRevision + 1);
        CHECK(updatedIndex.Fragments[0].Content != createdIndex.Fragments[0].Content);
        CHECK(updatedIndex.Fragments[0].RootActorLocalId == actorLocalId);

        Actor* duplicateBase = actor->Clone();
        REQUIRE(duplicateBase);
        StaticModel* duplicate = dynamic_cast<StaticModel*>(duplicateBase);
        REQUIRE(duplicate);
        duplicate->SetName(TEXT("Duplicate"));
        duplicate->SetParent(scene);
        const Guid duplicateRuntimeId = duplicate->GetID();
        const int64 duplicateLocalId = duplicate->GetLocalFileId();
        CHECK(duplicateRuntimeId != actorId);
        CHECK(duplicateLocalId != actorLocalId);
        CHECK(duplicate->Model.GetID() == modelId);

        REQUIRE(!Level::SaveScene(scene));
        SceneFragmentIndex duplicatedIndex;
        Array<Array<byte>> duplicatedFragments;
        REQUIRE(!SceneFragmentStore::Load(sceneId, duplicatedIndex, duplicatedFragments, error));
        REQUIRE(duplicatedIndex.Fragments.Count() == 2);
        CHECK(duplicatedIndex.IndexRevision == updatedIndex.IndexRevision + 1);
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, duplicateRuntimeId)));

        BytesContainer duplicateBeforeDelete;
        ReadFileBytes(GetExternalActorPath(scenePath, duplicateRuntimeId), duplicateBeforeDelete);
        actor->DeleteObject();
        ObjectsRemovalService::Flush();
        actor = nullptr;
        REQUIRE(!Level::SaveScene(scene));

        SceneFragmentIndex deletedIndex;
        Array<Array<byte>> deletedFragments;
        REQUIRE(!SceneFragmentStore::Load(sceneId, deletedIndex, deletedFragments, error));
        REQUIRE(deletedIndex.Fragments.Count() == 1);
        CHECK(deletedIndex.IndexRevision == duplicatedIndex.IndexRevision + 1);
        CHECK(deletedIndex.Fragments[0].RootActorLocalId == duplicateLocalId);
        CHECK(!FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
        CHECK(FileSystem::FileExists(GetExternalActorPath(scenePath, duplicateRuntimeId)));
        BytesContainer duplicateAfterDelete;
        ReadFileBytes(GetExternalActorPath(scenePath, duplicateRuntimeId), duplicateAfterDelete);
        CHECK(AreBytesEqual(duplicateBeforeDelete, duplicateAfterDelete));

        RefreshTestScene(scenePath);
        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        rapidjson_flax::StringBuffer reopenedBytes;
        REQUIRE(!Level::SaveSceneAssetToBytes(sceneAsset, reopenedBytes, nullptr, false));
        Content::UnloadAsset(sceneAsset);

        scene->DeleteObject();
        ObjectsRemovalService::Flush();
        scene = nullptr;
        BytesContainer bytes(reinterpret_cast<const byte*>(reopenedBytes.GetString()), static_cast<int32>(reopenedBytes.GetSize()));
        Scene* reopened = Level::LoadSceneFromBytes(bytes);
        REQUIRE(reopened);
        SCOPE_EXIT
        {
            Level::UnloadScene(reopened);
        };
        StaticModel* reopenedDuplicate = reopened->FindActor<StaticModel>(TEXT("Duplicate"));
        REQUIRE(reopenedDuplicate);
        CHECK(reopenedDuplicate->GetLocalFileId() == duplicateLocalId);
        CHECK(reopenedDuplicate->GetGlobalObjectId().SourceAsset.Value == sceneId);
        CHECK(reopenedDuplicate->GetGlobalObjectId().LocalFileId == duplicateLocalId);
        CHECK(reopenedDuplicate->Model.GetID() == modelId);
        CHECK(reopened->FindActor(TEXT("Updated")) == nullptr);
    }

    SECTION("Cross-scene actor move publishes both fragment sets in one save")
    {
        const Guid sceneAId = ParseGuid("45454545454545454545454545454501");
        const Guid sceneBId = ParseGuid("45454545454545454545454545454502");
        const Guid actorId = ParseGuid("45454545454545454545454545454503");
        const Guid scriptId = ParseGuid("45454545454545454545454545454504");
        const String sceneAPath = GetTestScenePath(TEXT("MoveActorSource"));
        const String sceneBPath = GetTestScenePath(TEXT("MoveActorDestination"));
        CleanupTestSceneFiles(sceneAPath);
        CleanupTestSceneFiles(sceneBPath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(sceneAPath);
            CleanupTestSceneFiles(sceneBPath);
        };
        WriteTestSceneAsset(sceneAPath, sceneAId, true);
        WriteTestSceneAsset(sceneBPath, sceneBId, true);

        AssetReference<Model> model = Content::LoadAsync<Model>(Globals::EngineContentFolder / TEXT("Editor/Primitives/Cube.flax"));
        REQUIRE(model);
        REQUIRE(!model->WaitForLoaded());
        const Guid modelId = model.GetID();
        REQUIRE(modelId.IsValid());

        Scene* sceneA = Scene::Spawn(ScriptingObject::SpawnParams(sceneAId, Scene::TypeInitializer));
        Scene* sceneB = Scene::Spawn(ScriptingObject::SpawnParams(sceneBId, Scene::TypeInitializer));
        REQUIRE(sceneA);
        REQUIRE(sceneB);
        SCOPE_EXIT
        {
            sceneA->DeleteObject();
            sceneB->DeleteObject();
        };
        sceneA->UseExternalActors = true;
        sceneB->UseExternalActors = true;

        StaticModel* actor = StaticModel::Spawn(ScriptingObject::SpawnParams(actorId, StaticModel::TypeInitializer));
        REQUIRE(actor);
        actor->SetName(TEXT("Moved actor"));
        actor->Model = model.Get();
        actor->SetParent(sceneA);
        ModelPrefab* script = ModelPrefab::Spawn(ScriptingObject::SpawnParams(scriptId, ModelPrefab::TypeInitializer));
        REQUIRE(script);
        script->SetParent(actor);
        const int64 scriptLocalId = script->GetLocalFileId();
        CHECK(actor->GetGlobalObjectId().SourceAsset.Value == sceneAId);
        CHECK(script->GetGlobalObjectId().SourceAsset.Value == sceneAId);
        CHECK(actor->Model.GetID() == modelId);

        Array<Scene*> scenes;
        scenes.Add(sceneA);
        scenes.Add(sceneB);
        REQUIRE(!Level::SaveScenes(scenes));
        const String sourceFragment = GetExternalActorPath(sceneAPath, actorId);
        const String destinationFragment = GetExternalActorPath(sceneBPath, actorId);
        REQUIRE(FileSystem::FileExists(sourceFragment));
        REQUIRE_FALSE(FileSystem::FileExists(destinationFragment));

        const int64 localId = actor->GetLocalFileId();
        actor->SetParent(sceneB);
        REQUIRE(actor->GetParent() == sceneB);
        CHECK(actor->GetID() == actorId);
        CHECK(actor->GetLocalFileId() == localId);
        CHECK(actor->GetGlobalObjectId().SourceAsset.Value == sceneBId);
        CHECK(actor->GetGlobalObjectId().LocalFileId == localId);
        CHECK(script->GetGlobalObjectId().SourceAsset.Value == sceneBId);
        CHECK(script->GetGlobalObjectId().LocalFileId == scriptLocalId);
        CHECK(actor->Model.GetID() == modelId);
        REQUIRE(!Level::SaveScenes(scenes));

        CHECK_FALSE(FileSystem::FileExists(sourceFragment));
        CHECK(FileSystem::FileExists(destinationFragment));
        SceneFragmentIndex sourceIndex;
        SceneFragmentIndex destinationIndex;
        Array<Array<byte>> sourceFragments;
        Array<Array<byte>> destinationFragments;
        String error;
        REQUIRE(!SceneFragmentStore::Load(sceneAId, sourceIndex, sourceFragments, error));
        REQUIRE(!SceneFragmentStore::Load(sceneBId, destinationIndex, destinationFragments, error));
        CHECK(sourceIndex.Fragments.IsEmpty());
        CHECK(sourceFragments.IsEmpty());
        REQUIRE(destinationIndex.Fragments.Count() == 1);
        REQUIRE(destinationFragments.Count() == 1);
        CHECK(destinationIndex.Fragments[0].RootActorLocalId == localId);

        rapidjson_flax::Document movedFragment;
        movedFragment.Parse(reinterpret_cast<const char*>(destinationFragments[0].Get()), destinationFragments[0].Count());
        REQUIRE(!movedFragment.HasParseError());
        CHECK(JsonTools::GetGuid(movedFragment, "ownerSceneGuid") == sceneBId);
        const rapidjson_flax::Value& movedData = GetDataArray(movedFragment);
        REQUIRE(movedData.Size() == 2);
        REQUIRE(movedData[0].HasMember("Model"));
        REQUIRE(movedData[0]["Model"].IsString());
        Guid serializedModel;
        REQUIRE_FALSE(Guid::Parse(StringAnsiView(movedData[0]["Model"].GetString(), movedData[0]["Model"].GetStringLength()), serializedModel));
        CHECK(serializedModel == modelId);

        // ParentActorsAction undo performs the same hierarchy transition back to the original scene.
        actor->SetParent(sceneA);
        CHECK(actor->GetGlobalObjectId().SourceAsset.Value == sceneAId);
        CHECK(actor->GetGlobalObjectId().LocalFileId == localId);
        CHECK(script->GetGlobalObjectId().SourceAsset.Value == sceneAId);
        CHECK(script->GetGlobalObjectId().LocalFileId == scriptLocalId);
        CHECK(actor->Model.GetID() == modelId);
        REQUIRE(!Level::SaveScenes(scenes));
        CHECK(FileSystem::FileExists(sourceFragment));
        CHECK_FALSE(FileSystem::FileExists(destinationFragment));

        REQUIRE(!SceneFragmentStore::Load(sceneAId, sourceIndex, sourceFragments, error));
        REQUIRE(!SceneFragmentStore::Load(sceneBId, destinationIndex, destinationFragments, error));
        REQUIRE(sourceIndex.Fragments.Count() == 1);
        REQUIRE(sourceFragments.Count() == 1);
        CHECK(sourceIndex.Fragments[0].RootActorLocalId == localId);
        CHECK(destinationIndex.Fragments.IsEmpty());
        CHECK(destinationFragments.IsEmpty());
    }

    SECTION("Rename external actors scene preserves GUID-keyed fragments")
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
        RefreshTestScene(scenePath);
        const String fragmentsFolder = SceneFragmentStore::GetScenePath(sceneId);
        CHECK(FileSystem::DirectoryExists(fragmentsFolder));

        REQUIRE(!AssetOperationService::MoveAsset(scenePath, renamedPath));

        CHECK(FileSystem::DirectoryExists(fragmentsFolder));
        CHECK(FileSystem::FileExists(GetExternalActorPath(renamedPath, actorId)));
    }

}

TEST_CASE("ExternalActorsSceneStorage Operations")
{

    SECTION("Trash and restore preserve the exact GUID-keyed scene storage")
    {
        const Guid sceneId = ParseGuid("89898989898989898989898989898981");
        const Guid actorId = ParseGuid("89898989898989898989898989898982");
        const String scenePath = GetTestScenePath(TEXT("TrashRestore"));
        CleanupTestSceneFiles(scenePath);
        SCOPE_EXIT
        {
            CleanupTestSceneFiles(scenePath);
        };
        WriteTestSceneAsset(scenePath, sceneId, true);
        WriteExternalActorFile(scenePath, actorId, sceneId, "Actor", 1024);

        const String metaPath = scenePath + TEXT(".meta");
        const String fragmentsPath = SceneFragmentStore::GetScenePath(sceneId);
        const String indexPath = SceneFragmentStore::GetIndexPath(sceneId);
        const String fragmentPath = GetExternalActorPath(scenePath, actorId);
        BytesContainer sourceBefore;
        BytesContainer metaBefore;
        BytesContainer indexBefore;
        BytesContainer fragmentBefore;
        ReadFileBytes(scenePath, sourceBefore);
        ReadFileBytes(metaPath, metaBefore);
        ReadFileBytes(indexPath, indexBefore);
        ReadFileBytes(fragmentPath, fragmentBefore);

        AssetTrashEntryRequest request;
        request.SourcePath = scenePath;
        request.ExpectedAssetGuid = sceneId;
        Array<AssetTrashEntryRequest> requests;
        requests.Add(MoveTemp(request));
        AssetTrashBatch trash;
        REQUIRE_FALSE(AssetOperationService::TrashEntries(requests, trash));
        REQUIRE(trash.Entries.Count() == 1);
        REQUIRE(trash.Entries[0].Fragments.Count() == 1);
        CHECK(trash.Entries[0].AssetGuid == sceneId);
        CHECK(FileSystem::AreFilePathsEquivalent(trash.Entries[0].Fragments[0].OriginalPath, fragmentsPath));
        CHECK(FileSystem::DirectoryExists(trash.Entries[0].Fragments[0].TrashPath));
        CHECK_FALSE(FileSystem::FileExists(scenePath));
        CHECK_FALSE(FileSystem::FileExists(metaPath));
        CHECK_FALSE(FileSystem::DirectoryExists(fragmentsPath));
        CHECK_FALSE(FileSystem::DirectoryExists(scenePath + TEXT("-data")));

        REQUIRE_FALSE(AssetOperationService::RestoreEntries(trash));
        CHECK(FileSystem::FileExists(scenePath));
        CHECK(FileSystem::FileExists(metaPath));
        CHECK(FileSystem::DirectoryExists(fragmentsPath));
        CHECK_FALSE(FileSystem::DirectoryExists(trash.Entries[0].Fragments[0].TrashPath));

        BytesContainer after;
        ReadFileBytes(scenePath, after);
        CHECK(AreBytesEqual(sourceBefore, after));
        ReadFileBytes(metaPath, after);
        CHECK(AreBytesEqual(metaBefore, after));
        ReadFileBytes(indexPath, after);
        CHECK(AreBytesEqual(indexBefore, after));
        ReadFileBytes(fragmentPath, after);
        CHECK(AreBytesEqual(fragmentBefore, after));

        SceneFragmentIndex restoredIndex;
        Array<Array<byte>> restoredFragments;
        String error;
        REQUIRE_FALSE(SceneFragmentStore::Load(sceneId, restoredIndex, restoredFragments, error));
        CHECK(restoredIndex.OwnerSceneGuid == sceneId);
        REQUIRE(restoredIndex.Fragments.Count() == 1);
        CHECK(restoredIndex.Fragments[0].RootActorLocalId == SceneObject::MakeLocalFileId(actorId));
    }

    SECTION("Rename duplicated external actors scene preserves cloned fragment identity")
    {
        const Guid sceneId = ParseGuid("88888888888888888888888888888881");
        const Guid actorId = ParseGuid("88888888888888888888888888888882");
        Guid cloneSceneId;
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
        RefreshTestScene(scenePath);

        REQUIRE(!AssetOperationService::CopyAsset(scenePath, clonePath, cloneSceneId));
        REQUIRE(cloneSceneId.IsValid());
        RefreshTestScene(clonePath);
        const String clonedFragments = SceneFragmentStore::GetScenePath(cloneSceneId);
        CHECK(FileSystem::DirectoryExists(clonedFragments));

        REQUIRE(!AssetOperationService::MoveAsset(clonePath, renamedPath));

        CHECK(FileSystem::DirectoryExists(clonedFragments));

        Array<String> renamedActorFiles;
        REQUIRE(!FileSystem::DirectoryGetFiles(renamedActorFiles, GetExternalActorsFolder(renamedPath), TEXT("*.sceneactor"), DirectorySearchOption::AllDirectories));
        CHECK(renamedActorFiles.Count() == 1);
    }

    SECTION("Delete external actors scene removes scene fragments")
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

        CHECK(FileSystem::DirectoryExists(GetSceneFragmentsFolder(scenePath)));
        REQUIRE(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
        const String fragmentsFolder = SceneFragmentStore::GetScenePath(sceneId);

        REQUIRE(!AssetOperationService::DeleteAsset(scenePath));

        CHECK(!FileSystem::DirectoryExists(fragmentsFolder));
    }

    SECTION("Delete loaded external actors scene removes scene fragments")
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
        RefreshTestScene(scenePath);

        SceneAsset* sceneAsset = Content::Load<SceneAsset>(scenePath);
        REQUIRE(sceneAsset);
        CHECK(FileSystem::DirectoryExists(GetSceneFragmentsFolder(scenePath)));
        REQUIRE(FileSystem::FileExists(GetExternalActorPath(scenePath, actorId)));
        const String fragmentsFolder = SceneFragmentStore::GetScenePath(sceneId);

        REQUIRE(!AssetOperationService::DeleteAsset(scenePath));

        CHECK(!FileSystem::DirectoryExists(fragmentsFolder));
        Content::UnloadAsset(sceneAsset);
    }

}

TEST_CASE("ActorClipboardPayloadValidation")
{
    const Guid sourceAssetId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1");
    constexpr int64 actorFileId = 2;
    const Guid actorId = SceneObject::MakeRuntimeObjectId(sourceAssetId, actorFileId);
    EmptyActor* actor = EmptyActor::Spawn(ScriptingObject::SpawnParams(actorId, EmptyActor::TypeInitializer));
    REQUIRE(actor);
    actor->SetPersistentDocumentIdentity(AssetGuid(sourceAssetId), actorFileId);
    actor->SetName(TEXT("Clipboard Actor"));

    Array<Actor*> source;
    source.Add(actor);
    Array<byte> valid = Actor::ToBytes(source);
    REQUIRE(valid.HasItems());

    actor->DeleteObject();
    ObjectsRemovalService::Flush();
    REQUIRE(Scripting::TryFindObject<Actor>(actorId) == nullptr);

    SECTION("Every truncated prefix is rejected without constructing an Actor")
    {
        for (int32 length = 0; length < valid.Count(); length++)
        {
            const auto restored = Actor::FromBytes(Span<byte>(valid.Get(), length));
            CHECK(restored.IsEmpty());
            CHECK(Scripting::TryFindObject<Actor>(actorId) == nullptr);
        }
    }

    SECTION("Oversized object count is rejected before allocation")
    {
        Array<byte> malformed = valid;
        const int32 oversizedCount = MAX_int32;
        const int32 objectsCountOffset = sizeof(int32) + sizeof(Guid);
        Platform::MemoryCopy(malformed.Get() + objectsCountOffset, &oversizedCount, sizeof(oversizedCount));
        CHECK(Actor::TryGetSerializedObjectsIds(Span<byte>(malformed.Get(), malformed.Count())).IsEmpty());
        CHECK(Actor::FromBytes(Span<byte>(malformed.Get(), malformed.Count())).IsEmpty());
        CHECK(Scripting::TryFindObject<Actor>(actorId) == nullptr);
    }

    SECTION("Oversized JSON length is rejected before reading past the payload")
    {
        Array<byte> malformed = valid;
        const int32 oversizedJson = MAX_int32;
        const int32 jsonSizeOffset = sizeof(int32) * 2 + sizeof(Guid) + sizeof(int64);
        Platform::MemoryCopy(malformed.Get() + jsonSizeOffset, &oversizedJson, sizeof(oversizedJson));
        CHECK(Actor::FromBytes(Span<byte>(malformed.Get(), malformed.Count())).IsEmpty());
        CHECK(Scripting::TryFindObject<Actor>(actorId) == nullptr);
    }

    SECTION("Valid payload still restores the complete Actor")
    {
        auto restored = Actor::FromBytes(Span<byte>(valid.Get(), valid.Count()));
        REQUIRE(restored.Count() == 1);
        CHECK(restored[0]->GetID().IsValid());
        CHECK(restored[0]->GetID() != actorId);
        CHECK(restored[0]->GetName() == TEXT("Clipboard Actor"));
        restored[0]->DeleteObject();
        ObjectsRemovalService::Flush();
    }

    SECTION("External parent references can be redirected during construction")
    {
        constexpr int64 sourceParentFileId = 3;
        constexpr int64 sourceChildFileId = 4;
        const Guid sourceParentId = SceneObject::MakeRuntimeObjectId(sourceAssetId, sourceParentFileId);
        const Guid sourceChildId = SceneObject::MakeRuntimeObjectId(sourceAssetId, sourceChildFileId);
        const Guid destinationAssetId = ParseGuid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2");
        constexpr int64 destinationParentFileId = 2;
        const Guid destinationParentId = SceneObject::MakeRuntimeObjectId(destinationAssetId, destinationParentFileId);
        const Guid restoredChildId = ParseGuid("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb5");
        EmptyActor* sourceParent = EmptyActor::Spawn(ScriptingObject::SpawnParams(sourceParentId, EmptyActor::TypeInitializer));
        EmptyActor* sourceChild = EmptyActor::Spawn(ScriptingObject::SpawnParams(sourceChildId, EmptyActor::TypeInitializer));
        REQUIRE(sourceParent);
        REQUIRE(sourceChild);
        sourceParent->SetPersistentDocumentIdentity(AssetGuid(sourceAssetId), sourceParentFileId);
        sourceChild->SetPersistentDocumentIdentity(AssetGuid(sourceAssetId), sourceChildFileId);
        sourceChild->SetParent(sourceParent);

        Array<Actor*> childSource;
        childSource.Add(sourceChild);
        Array<byte> childData = Actor::ToBytes(childSource);
        REQUIRE(childData.HasItems());
        sourceChild->DeleteObject();
        sourceParent->DeleteObject();
        ObjectsRemovalService::Flush();

        EmptyActor* destinationParent = EmptyActor::Spawn(ScriptingObject::SpawnParams(destinationParentId, EmptyActor::TypeInitializer));
        REQUIRE(destinationParent);
        destinationParent->SetPersistentDocumentIdentity(AssetGuid(destinationAssetId), destinationParentFileId);
        destinationParent->RegisterObject();
        Dictionary<Guid, Guid> idsMapping;
        idsMapping.Add(sourceChildId, restoredChildId);
        auto restoredIds = Actor::FromBytesToIds(Span<byte>(childData.Get(), childData.Count()), idsMapping, destinationParentId);
        REQUIRE(restoredIds.Count() == 1);
        CHECK(restoredIds[0] == restoredChildId);
        Actor* restored = Scripting::TryFindObject<Actor>(restoredIds[0]);
        REQUIRE(restored);
        CHECK(restored->GetParent() == destinationParent);

        restored->DeleteObject();
        destinationParent->DeleteObject();
        ObjectsRemovalService::Flush();
    }
}

#endif
