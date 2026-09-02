// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/Processors/JsonAssetProcessor.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Build/RuntimeDependencyClosure.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <cstring>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    AssetRecord MakeDocumentRecord(const String& path, const StringView& typeName)
    {
        AssetRecord record;
        record.ID = Guid::New();
        record.SourceAssetID = record.ID;
        record.LocalId = 1;
        record.TypeName = typeName;
        record.CanonicalPath = CanonicalAssetPath(path);
        record.SourcePath = SourceFilePath(path);
        record.ProcessorID = JsonAssetProcessor::ProcessorID();
        record.SourceKind = AssetSourceKind::TextDocument;
        record.Status = AssetRecordStatus::Ready;
        record.DatabaseRevision = 1;
        return record;
    }

    bool PrepareScene(const String& root, const String& content, const String& library, const char* json,
        AssetPipelineDiagnostic& diagnostic)
    {
        const String path = content / (Guid::New().ToString(Guid::FormatType::N) + TEXT(".scene"));
        if (File::WriteAllBytes(path, reinterpret_cast<const byte*>(json), static_cast<int32>(std::strlen(json))))
            return true;
        const AssetRecord record = MakeDocumentRecord(path, TEXT("FlaxEngine.SceneAsset"));
        const AssetProcessorDescriptor descriptor = JsonAssetProcessor::CreateDescriptor();
        SourceHashCache hashCache;
        AssetCancellationSource cancellation;
        PreparedAsset prepared;
        PrepareAssetContext context(root, content, library, record, descriptor, StringAnsiView("{}"), hashCache,
            cancellation.GetToken());
        return JsonAssetProcessor::Prepare(context, prepared, diagnostic);
    }

    bool PrepareDocument(const String& root, const String& content, const String& library, const StringView& extension,
        const StringView& typeName, const Guid& id, const char* json, PreparedAsset& prepared,
        AssetPipelineDiagnostic& diagnostic)
    {
        const String path = content / (id.ToString(Guid::FormatType::N) + extension);
        if (File::WriteAllBytes(path, reinterpret_cast<const byte*>(json), static_cast<int32>(std::strlen(json))))
            return true;
        AssetRecord record = MakeDocumentRecord(path, typeName);
        record.ID = id;
        record.SourceAssetID = id;
        const AssetProcessorDescriptor descriptor = JsonAssetProcessor::CreateDescriptor();
        SourceHashCache hashCache;
        AssetCancellationSource cancellation;
        PrepareAssetContext context(root, content, library, record, descriptor, StringAnsiView("{}"), hashCache,
            cancellation.GetToken());
        if (JsonAssetProcessor::Prepare(context, prepared, diagnostic))
            return true;
        return context.Finalize(record.DatabaseRevision, prepared, diagnostic);
    }

    Array<AssetObjectId> RuntimeReferences(const PreparedAsset& prepared)
    {
        Array<AssetObjectId> result;
        for (const AssetDependency& dependency : prepared.Dependencies)
        {
            if (dependency.Kind == AssetDependencyKind::RuntimeReference)
                result.Add(dependency.ObjectID);
        }
        return result;
    }

    bool WriteFragmentStore(const String& projectRoot, const Guid& sceneId, const char* payloadJson)
    {
        SceneFragmentWrite write;
        write.RootActorLocalId = 2;
        write.ContainedLocalIds.Add(2);
        write.Payload.Set(reinterpret_cast<const byte*>(payloadJson), static_cast<int32>(std::strlen(payloadJson)));
        Array<SceneFragmentWrite> writes;
        writes.Add(MoveTemp(write));
        SceneFragmentSavePlan plan;
        String error;
        if (SceneFragmentStore::PrepareSave(sceneId, writes, plan, error))
            return true;
        const String scenePath = SceneFragmentStore::GetScenePath(projectRoot, sceneId);
        if (FileSystem::CreateDirectory(scenePath) ||
            File::WriteAllBytes(scenePath / TEXT("scene-fragments.index"), plan.IndexData.Get(), plan.IndexData.Count()))
            return true;
        for (const PreparedSceneFragment& fragment : plan.Fragments)
        {
            const String path = scenePath / fragment.RelativePhysicalPath;
            if (FileSystem::CreateDirectory(StringUtils::GetDirectoryName(path)) ||
                File::WriteAllBytes(path, fragment.Data.Get(), fragment.Data.Count()))
                return true;
        }
        return false;
    }
}

TEST_CASE("JSON scene and prefab processors reject unsupported versions without mutation")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonFormatRejection-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    struct RejectionCase
    {
        const Char* Extension;
        const Char* TypeName;
        const char* Json;
    };
    const RejectionCase cases[] =
    {
        { TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), R"({"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})" },
        { TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), R"({"sceneVersion":3,"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})" },
        { TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), R"({"sceneVersion":5,"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})" },
        { TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), R"({"sceneVersion":4,"prefabVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})" },
        { TEXT(".prefab"), TEXT("FlaxEngine.Prefab"), R"({"objects":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})" },
        { TEXT(".prefab"), TEXT("FlaxEngine.Prefab"), R"({"prefabVersion":3,"objects":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})" },
        { TEXT(".prefab"), TEXT("FlaxEngine.Prefab"), R"({"prefabVersion":5,"objects":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})" },
        { TEXT(".prefab"), TEXT("FlaxEngine.Prefab"), R"({"prefabVersion":4,"documentVersion":1,"objects":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})" },
    };
    for (const RejectionCase& test : cases)
    {
        const Guid id = Guid::New();
        const String path = content / (id.ToString(Guid::FormatType::N) + test.Extension);
        PreparedAsset prepared;
        AssetPipelineDiagnostic diagnostic;
        CHECK(PrepareDocument(root, content, library, test.Extension, test.TypeName, id, test.Json, prepared, diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
        BytesContainer bytes;
        REQUIRE_FALSE(File::ReadAllBytes(path, bytes));
        REQUIRE(bytes.Length() == static_cast<int32>(std::strlen(test.Json)));
        CHECK(Platform::MemoryCompare(bytes.Get(), test.Json, bytes.Length()) == 0);
    }
}

TEST_CASE("JSON scene processor accepts only canonical actor-local structured references")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonSceneReferences-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    CHECK_FALSE(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Sun":{"kind":1,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}},{"fileId":2,"type":"FlaxEngine.DirectionalLight","parentFileId":1}]})",
        diagnostic));

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":0,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":2,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":1,"guid":"00000000000000000000000000000000","fileId":0,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":1,"guid":"00000000000000000000000000000000","fileId":2}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":1,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":3}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"kind":9,"guid":"00000000000000000000000000000000","fileId":2,"prefabInstanceFileId":0}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    CHECK(PrepareScene(root, content, library,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Ref":{"guid":"00000000000000000000000000000000","fileId":2}}]})",
        diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
}

TEST_CASE("Current scene and prefab documents retain nested references for cook closure")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonScenePrefabCook-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid sceneId(0x61000001, 0x61000002, 0x61000003, 0x61000004);
    const Guid prefabId(0x61000011, 0x61000012, 0x61000013, 0x61000014);
    const Guid materialSourceId(0x61000021, 0x61000022, 0x61000023, 0x61000024);
    const Guid editedMaterialSourceId(0x61000031, 0x61000032, 0x61000033, 0x61000034);
    const AssetObjectId prefabObject = AssetObjectId::Main(AssetGuid(prefabId));
    const AssetObjectId materialObject(AssetGuid(materialSourceId), 7);
    const AssetObjectId editedMaterialObject(AssetGuid(editedMaterialSourceId), 9);

    PreparedAsset scene;
    PreparedAsset prefab;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(PrepareDocument(root, content, library, TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), sceneId,
        R"({"sceneVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.Scene","Nested":{"Prefab":{"kind":2,"guid":"61000011610000126100001361000014","fileId":81,"prefabInstanceFileId":0}}}]})",
        scene, diagnostic));
    REQUIRE_FALSE(PrepareDocument(root, content, library, TEXT(".prefab"), TEXT("FlaxEngine.Prefab"), prefabId,
        R"({"prefabVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.StaticModel","Material":{"kind":0,"guid":"61000021610000226100002361000024","fileId":7,"prefabInstanceFileId":0}}]})",
        prefab, diagnostic));

    const Array<AssetObjectId> sceneReferences = RuntimeReferences(scene);
    const Array<AssetObjectId> prefabReferences = RuntimeReferences(prefab);
    REQUIRE(sceneReferences.Count() == 1);
    CHECK(sceneReferences.Contains(prefabObject));
    REQUIRE(prefabReferences.Count() == 1);
    CHECK(prefabReferences.Contains(materialObject));

    RuntimeObjectDependencyRecord sceneRecord;
    sceneRecord.Object = AssetObjectId::Main(AssetGuid(sceneId));
    sceneRecord.Dependencies = sceneReferences;
    RuntimeObjectDependencyRecord prefabRecord;
    prefabRecord.Object = prefabObject;
    prefabRecord.Dependencies = prefabReferences;
    RuntimeObjectDependencyRecord materialRecord;
    materialRecord.Object = materialObject;
    Array<RuntimeObjectDependencyRecord> records;
    records.Add(sceneRecord);
    records.Add(prefabRecord);
    records.Add(materialRecord);
    Array<AssetObjectId> roots;
    roots.Add(sceneRecord.Object);
    RuntimeDependencyClosureResult closure;
    REQUIRE_FALSE(RuntimeDependencyClosure::Build(roots, records, closure, diagnostic));
    CHECK(closure.Objects.Count() == 3);
    CHECK(closure.Objects.Contains(sceneRecord.Object));
    CHECK(closure.Objects.Contains(prefabObject));
    CHECK(closure.Objects.Contains(materialObject));
    CHECK(closure.Edges.Count() == 2);

    PreparedAsset editedPrefab;
    REQUIRE_FALSE(PrepareDocument(root, content, library, TEXT(".prefab"), TEXT("FlaxEngine.Prefab"), prefabId,
        R"({"prefabVersion":4,"objects":[{"fileId":1,"type":"FlaxEngine.StaticModel","Material":{"kind":0,"guid":"61000031610000326100003361000034","fileId":9,"prefabInstanceFileId":0}}]})",
        editedPrefab, diagnostic));
    CHECK(editedPrefab.InputFingerprint != prefab.InputFingerprint);
    prefabRecord.Dependencies = RuntimeReferences(editedPrefab);
    REQUIRE(prefabRecord.Dependencies.Count() == 1);
    CHECK(prefabRecord.Dependencies.Contains(editedMaterialObject));
    RuntimeObjectDependencyRecord editedMaterialRecord;
    editedMaterialRecord.Object = editedMaterialObject;
    records[1] = prefabRecord;
    records.Add(editedMaterialRecord);
    REQUIRE_FALSE(RuntimeDependencyClosure::Build(roots, records, closure, diagnostic));
    CHECK(closure.Objects.Count() == 3);
    CHECK_FALSE(closure.Objects.Contains(materialObject));
    CHECK(closure.Objects.Contains(editedMaterialObject));
}

TEST_CASE("External actor scene preparation embeds project-root fragments for self-contained cook")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonExternalActorsCook-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid sceneId = Guid::New();
    const Guid materialId(0x62000011, 0x62000012, 0x62000013, 0x62000014);
    REQUIRE_FALSE(WriteFragmentStore(root, sceneId,
        R"([{"fileId":2,"type":"FlaxEngine.StaticModel","parentFileId":1,"Material":{"kind":0,"guid":"62000011620000126200001362000014","fileId":7,"prefabInstanceFileId":0}}])"));

    const String sourcePath = content / TEXT("External.scene");
    const char sourceJson[] = R"({"sceneVersion":4,"externalActors":true,"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})";
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, reinterpret_cast<const byte*>(sourceJson), ARRAY_COUNT(sourceJson) - 1));
    AssetRecord record = MakeDocumentRecord(sourcePath, TEXT("FlaxEngine.SceneAsset"));
    record.ID = sceneId;
    record.SourceAssetID = sceneId;
    const AssetProcessorDescriptor descriptor = JsonAssetProcessor::CreateDescriptor();
    SourceHashCache hashCache;
    AssetCancellationSource cancellation;
    PreparedAsset prepared;
    AssetPipelineDiagnostic diagnostic;
    PrepareAssetContext prepareContext(root, content, library, record, descriptor, StringAnsiView("{}"), hashCache,
        cancellation.GetToken());
    REQUIRE_FALSE(descriptor.Prepare(prepareContext, prepared, diagnostic));
    REQUIRE_FALSE(prepareContext.Finalize(record.DatabaseRevision, prepared, diagnostic));

    const auto* payload = static_cast<const JsonAssetPreparedPayload*>(prepared.Payload.get());
    REQUIRE(payload);
    CHECK(payload->SceneUsesPartitions);
    REQUIRE(payload->ScenePartitions.Count() == 1);
    CHECK(payload->ScenePartitions[0].RootFileId == 2);
    const Array<AssetObjectId> references = RuntimeReferences(prepared);
    REQUIRE(references.Count() == 1);
    CHECK(references.Contains(AssetObjectId(AssetGuid(materialId), 7)));

    // Build must consume the prepared bytes, never reopen the private project source tree.
    REQUIRE_FALSE(FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(root, sceneId), true));
    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceFile)
        {
            sourceDependency = &dependency;
            break;
        }
    }
    REQUIRE(sourceDependency);
    ArtifactBuildInput input;
    input.StableIdentity = sourceDependency->StableIdentity;
    input.Path = sourcePath;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    ArtifactBuildContext buildContext(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
    REQUIRE_FALSE(buildContext.Initialize(diagnostic));
    REQUIRE_FALSE(descriptor.Build(buildContext, diagnostic));
    REQUIRE_FALSE(buildContext.Close(diagnostic));
    REQUIRE(buildContext.GetFiles().Count() == 1);

    auto storage = ContentStorageManager::GetStorage(buildContext.GetFiles()[0].AbsolutePath);
    REQUIRE(storage);
    AssetInitData initData;
    REQUIRE_FALSE(storage->LoadAssetHeader(sceneId, initData));
    REQUIRE(initData.Header.Chunks[0]);
    REQUIRE_FALSE(storage->LoadAssetChunk(initData.Header.Chunks[0]));
    const auto& runtimeBytes = initData.Header.Chunks[0]->Data;
    rapidjson_flax::Document runtime;
    runtime.Parse(reinterpret_cast<const char*>(runtimeBytes.Get()), runtimeBytes.Length());
    REQUIRE_FALSE(runtime.HasParseError());
    CHECK_FALSE(runtime.HasMember("ExternalActors"));
    REQUIRE(runtime.HasMember("Data"));
    REQUIRE(runtime["Data"].IsArray());
    CHECK(runtime["Data"].Size() == 2);
}

TEST_CASE("External actor fragment edits and removals invalidate scene preparation")
{
    const String root = Globals::TemporaryFolder / (TEXT("JsonExternalActorsInvalidation-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid sceneId = Guid::New();
    const char sceneJson[] = R"({"sceneVersion":4,"externalActors":true,"objects":[{"fileId":1,"type":"FlaxEngine.Scene"}]})";
    AssetPipelineDiagnostic diagnostic;
    PreparedAsset initial;
    REQUIRE_FALSE(WriteFragmentStore(root, sceneId,
        R"([{"fileId":2,"type":"FlaxEngine.EmptyActor","parentFileId":1,"Name":"Initial"}])"));
    REQUIRE_FALSE(PrepareDocument(root, content, library, TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), sceneId,
        sceneJson, initial, diagnostic));

    PreparedAsset edited;
    REQUIRE_FALSE(WriteFragmentStore(root, sceneId,
        R"([{"fileId":2,"type":"FlaxEngine.EmptyActor","parentFileId":1,"Name":"Edited"}])"));
    REQUIRE_FALSE(PrepareDocument(root, content, library, TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), sceneId,
        sceneJson, edited, diagnostic));
    CHECK(edited.InputFingerprint != initial.InputFingerprint);

    Array<SceneFragmentWrite> noFragments;
    SceneFragmentSavePlan emptyPlan;
    String error;
    REQUIRE_FALSE(SceneFragmentStore::PrepareSave(sceneId, noFragments, emptyPlan, error));
    const String indexPath = SceneFragmentStore::GetScenePath(root, sceneId) / TEXT("scene-fragments.index");
    REQUIRE_FALSE(File::WriteAllBytes(indexPath, emptyPlan.IndexData.Get(), emptyPlan.IndexData.Count()));
    PreparedAsset removed;
    REQUIRE_FALSE(PrepareDocument(root, content, library, TEXT(".scene"), TEXT("FlaxEngine.SceneAsset"), sceneId,
        sceneJson, removed, diagnostic));
    CHECK(removed.InputFingerprint != edited.InputFingerprint);
    CHECK(removed.InputFingerprint != initial.InputFingerprint);
}

#endif
