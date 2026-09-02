// Copyright (c) Wojciech Figat. All rights reserved.

#include "Level.h"
#include "ActorsCache.h"
#include "LargeWorlds.h"
#include "SceneQuery.h"
#include "SceneObjectsFactory.h"
#include "FlaxEngine.Gen.h"
#include "Scene/Scene.h"
#include "ScenePrefabDocument.h"
#include "SceneFragments/SceneFragmentReconciler.h"
#include "SceneFragments/SceneFragmentStore.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Deprecated.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Core/Cache.h"
#include "Engine/Core/Collections/CollectionPoolCache.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Core/ObjectsRemovalService.h"
#include "Engine/Core/Config/LayersTagsSettings.h"
#include "Engine/Core/Types/LayersMask.h"
#include "Engine/Core/Types/Stopwatch.h"
#include "Engine/Debug/Exceptions/ArgumentException.h"
#include "Engine/Debug/Exceptions/ArgumentNullException.h"
#include "Engine/Debug/Exceptions/InvalidOperationException.h"
#include "Engine/Debug/Exceptions/JsonParseException.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Threading/JobSystem.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Profiler/ProfilerMemory.h"
#include "Engine/Scripting/Script.h"
#include "Engine/Engine/Time.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MDomain.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/BinaryModule.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Prefabs/Prefab.h"
#include <algorithm>
#if USE_EDITOR
#include "Editor/Editor.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Platform/MessageBox.h"
#include "Engine/Engine/CommandLine.h"
#include "Engine/Serialization/JsonSerializer.h"
#include "Editor/Scripting/ScriptsBuilder.h"
#endif

#if USE_LARGE_WORLDS
bool LargeWorlds::Enable = true;
#else
bool LargeWorlds::Enable = false;
#endif

void LargeWorlds::UpdateOrigin(Vector3& origin, const Vector3& position)
{
    if (Enable)
    {
        constexpr Real chunkSizeInv = 1.0 / ChunkSize;
        constexpr Real chunkSizeHalf = ChunkSize * 0.5;
        origin = Vector3(Int3((position - chunkSizeHalf) * chunkSizeInv)) * ChunkSize;
    }
}

bool LayersMask::HasLayer(const StringView& layerName) const
{
    return HasLayer(Level::GetLayerIndex(layerName));
}

LayersMask LayersMask::GetMask(Span<StringView> layerNames)
{
    LayersMask mask(0);
    for (StringView& layerName : layerNames)
    {
        // Ignore blank entries
        if (layerName.Length() == 0)
            continue;
        int32 index = Level::GetLayerIndex(layerName);
        if (index != -1)
            mask.Mask |= (uint32)(1 << index);
    }
    return mask;
}

enum class SceneEventType
{
    OnSceneSaving = 0,
    OnSceneSaved = 1,
    OnSceneSaveError = 2,
    OnSceneLoading = 3,
    OnSceneLoaded = 4,
    OnSceneLoadError = 5,
    OnSceneUnloading = 6,
    OnSceneUnloaded = 7,
};

enum class SceneResult
{
    Success,
    Failed,
    Wait,
};

#if USE_EDITOR

namespace
{
    constexpr const char* ExternalActorsKey = "ExternalActors";
    constexpr const char* DataKey = "Data";
    constexpr const char* IDKey = "ID";
    constexpr const char* TypeNameKey = "TypeName";
    constexpr const char* EngineBuildKey = "EngineBuild";
    constexpr const char* FileIdKey = "FileId";
    constexpr const char* ParentFileIdKey = "ParentFileId";
    constexpr const char* SourceObjectsKey = "objects";
    constexpr const char* SourceFileIdKey = "fileId";
    constexpr const char* SourceParentFileIdKey = "parentFileId";
    constexpr const char* OrderInParentKey = "OrderInParent";
    constexpr const char* SiblingOrderKeyKey = "SiblingOrderKey";

    const Char* GetExternalActorDiagnosticName(SceneFragmentDiagnosticCode code)
    {
        switch (code)
        {
        case SceneFragmentDiagnosticCode::IndexMissing: return TEXT("missing index");
        case SceneFragmentDiagnosticCode::Malformed: return TEXT("malformed data");
        case SceneFragmentDiagnosticCode::FutureVersion: return TEXT("unsupported version");
        case SceneFragmentDiagnosticCode::OwnerMismatch: return TEXT("wrong scene owner");
        case SceneFragmentDiagnosticCode::MissingFragment: return TEXT("missing fragment");
        case SceneFragmentDiagnosticCode::DuplicateLocalId: return TEXT("duplicate identity");
        case SceneFragmentDiagnosticCode::MisplacedFragment: return TEXT("misplaced fragment");
        case SceneFragmentDiagnosticCode::OrphanFragment: return TEXT("orphaned fragment");
        case SceneFragmentDiagnosticCode::ContentMismatch: return TEXT("content mismatch");
        default: return TEXT("unknown error");
        }
    }

    bool ValidateExternalActorFragments(const Guid& sceneGuid)
    {
        Array<SceneFragmentDiagnostic> diagnostics;
        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        for (const SceneFragmentDiagnostic& diagnostic : diagnostics)
        {
            LOG(Error, "ExternalActor storage for scene '{0}' is invalid ({1}) at '{2}': {3}",
                sceneGuid, GetExternalActorDiagnosticName(diagnostic.Code), diagnostic.Path, diagnostic.Message);
        }
        return diagnostics.HasItems();
    }

    bool ValidateSceneStorageTransitionSource(const StringView& path, bool expectedExternalActors, String& error)
    {
        BytesContainer bytes;
        rapidjson_flax::Document document;
        if (File::ReadAllBytes(path, bytes))
        {
            error = TEXT("Cannot read the scene source before changing actor storage.");
            return true;
        }
        document.Parse(bytes.Get<char>(), bytes.Length());
        if (document.HasParseError() || !document.IsObject())
        {
            error = TEXT("Unsupported authored scene format. Run the separate offline migrator from the old branch before changing actor storage.");
            return true;
        }
        const auto version = document.FindMember("sceneVersion");
        const auto objects = document.FindMember(SourceObjectsKey);
        const auto externalActors = document.FindMember("externalActors");
        const bool mixedVersionMarkers = document.HasMember("documentVersion") ||
            document.HasMember("settingsVersion") || document.HasMember("prefabVersion");
        const bool authoredExternalActors = externalActors != document.MemberEnd() &&
            externalActors->value.IsBool() && externalActors->value.GetBool();
        if (mixedVersionMarkers || version == document.MemberEnd() || !version->value.IsUint() || version->value.GetUint() != 4 ||
            objects == document.MemberEnd() || !objects->value.IsArray() ||
            (externalActors != document.MemberEnd() && !externalActors->value.IsBool()) ||
            authoredExternalActors != expectedExternalActors ||
            ScenePrefabDocument::ValidateObjects(objects->value, true, error))
        {
            if (error.IsEmpty())
            {
                error = TEXT("Unsupported authored scene format. Run the separate offline migrator from the old branch before changing actor storage.");
            }
            return true;
        }
        const auto rootExternalActors = objects->value[0].FindMember("useExternalActors");
        const bool authoredRootExternalActors = rootExternalActors != objects->value[0].MemberEnd() &&
            rootExternalActors->value.IsBool() && rootExternalActors->value.GetBool();
        if ((rootExternalActors != objects->value[0].MemberEnd() && !rootExternalActors->value.IsBool()) ||
            authoredRootExternalActors != expectedExternalActors)
        {
            error = TEXT("Scene root actor storage mode does not match the authored scene header.");
            return true;
        }
        return false;
    }

    struct ExternalActorFileInfo
    {
        String File;
        Array<byte> Bytes;
        int64 ActorId = 0;
        int64 ParentId = 0;
        ExternalSiblingOrderKey SiblingOrderKey;
        bool IsValid = false;
    };

    rapidjson_flax::Value JsonKey(const char* key, rapidjson_flax::Value::AllocatorType& allocator)
    {
        rapidjson_flax::Value value;
        value.SetString(key, static_cast<rapidjson::SizeType>(StringUtils::Length(key)), allocator);
        return value;
    }

    int32 CompareFileIds(int64 a, int64 b)
    {
        return a < b ? -1 : a > b ? 1 : 0;
    }

    ExternalSiblingOrderKey GetSerializedSiblingOrderKey(const rapidjson_flax::Value& value)
    {
        ExternalSiblingOrderKey result;
        auto keyMember = value.FindMember(SiblingOrderKeyKey);
        if (keyMember == value.MemberEnd())
            keyMember = value.FindMember("siblingOrderKey");
        if (keyMember != value.MemberEnd() && keyMember->value.IsString() &&
            ExternalSiblingOrderKey::TryParse(keyMember->value.GetText(), result))
        {
            return result;
        }

        auto legacyMember = value.FindMember(OrderInParentKey);
        if (legacyMember == value.MemberEnd())
            legacyMember = value.FindMember("orderInParent");
        const int64 legacyOrder = legacyMember != value.MemberEnd() && legacyMember->value.IsInt64()
                                      ? legacyMember->value.GetInt64()
                                      : 0;
        return ExternalSiblingOrderKey::FromLegacy(legacyOrder);
    }

    bool SortExternalActorFileInfo(const ExternalActorFileInfo& a, const ExternalActorFileInfo& b)
    {
        const int32 parentCompare = CompareFileIds(a.ParentId, b.ParentId);
        if (parentCompare != 0)
            return parentCompare < 0;
        const int32 orderCompare = a.SiblingOrderKey.Compare(b.SiblingOrderKey);
        if (orderCompare != 0)
            return orderCompare < 0;
        return CompareFileIds(a.ActorId, b.ActorId) < 0;
    }

    void EnsureExternalSiblingOrderKeys(const Array<SceneObject*>& objects)
    {
        if (objects.IsEmpty())
            return;

        Array<int32> predecessors;
        predecessors.Resize(objects.Count());
        predecessors.SetAll(-1);
        Array<int32> tails(objects.Count());

        // Keep the largest subset whose existing keys already encode the current order.
        for (int32 i = 0; i < objects.Count(); i++)
        {
            SceneObject* object = objects[i];
            if (!object->HasExternalSiblingOrderKeyForCurrentParent())
                continue;

            int32 left = 0;
            int32 right = tails.Count();
            while (left < right)
            {
                const int32 middle = left + (right - left) / 2;
                const SceneObject* tail = objects[tails[middle]];
                if (tail->GetExternalSiblingOrderKey().Compare(object->GetExternalSiblingOrderKey()) < 0)
                    left = middle + 1;
                else
                    right = middle;
            }
            if (left > 0)
                predecessors[i] = tails[left - 1];
            if (left == tails.Count())
                tails.Add(i);
            else
                tails[left] = i;
        }

        Array<byte> keepExisting;
        keepExisting.Resize(objects.Count());
        keepExisting.SetAll(0);
        if (tails.HasItems())
        {
            for (int32 i = tails.Last(); i != -1; i = predecessors[i])
                keepExisting[i] = 1;
        }

        const ExternalSiblingOrderKey* previousKey = nullptr;
        int32 index = 0;
        while (index < objects.Count())
        {
            if (keepExisting[index])
            {
                previousKey = &objects[index]->GetExternalSiblingOrderKey();
                index++;
                continue;
            }

            int32 blockEnd = index + 1;
            while (blockEnd < objects.Count() && !keepExisting[blockEnd])
                blockEnd++;
            const ExternalSiblingOrderKey* nextKey = blockEnd < objects.Count()
                                                               ? &objects[blockEnd]->GetExternalSiblingOrderKey()
                                                               : nullptr;
            for (; index < blockEnd; index++)
            {
                SceneObject* object = objects[index];
                const ExternalSiblingOrderKey key = ExternalSiblingOrderKey::CreateBetween(previousKey, nextKey, object->GetSceneObjectId());
                object->SetExternalSiblingOrderKey(key);
                previousKey = &object->GetExternalSiblingOrderKey();
            }
        }
    }

    void EnsureExternalSiblingOrderKeys(Scene* scene)
    {
        Array<SceneObject*> rootScripts(scene->Scripts.Count());
        for (Script* script : scene->Scripts)
            rootScripts.Add(script);
        EnsureExternalSiblingOrderKeys(rootScripts);

        Array<SceneObject*> rootChildren(scene->Children.Count());
        for (Actor* child : scene->Children)
            rootChildren.Add(child);
        EnsureExternalSiblingOrderKeys(rootChildren);

        Array<Actor*> actors;
        SceneQuery::GetAllActors(scene, actors);
        for (Actor* actor : actors)
        {
            Array<SceneObject*> scripts(actor->Scripts.Count());
            for (Script* script : actor->Scripts)
                scripts.Add(script);
            EnsureExternalSiblingOrderKeys(scripts);

            Array<SceneObject*> children(actor->Children.Count());
            for (Actor* child : actor->Children)
                children.Add(child);
            EnsureExternalSiblingOrderKeys(children);
        }
    }

    void WriteSceneObject(JsonWriter& writer, SceneObject* obj, bool includeOrderInParent)
    {
        writer.StartObject();

        bool serialized = false;
        if (obj->HasPrefabLink())
        {
            auto prefab = Content::LoadAsset<Prefab>(obj->GetPrefabID());
            if (prefab)
            {
                prefab->GetDefaultInstance();
                SceneObject* prefabObject;
                if (prefab->ObjectsCache.TryGet(obj->GetPrefabObjectID(), prefabObject))
                {
                    obj->Serialize(writer, prefabObject);
                    serialized = true;
                }
                else
                {
                    LOG(Warning, "Missing object {1} in prefab {0}.", prefab->ToString(), obj->GetPrefabObjectID());
                }
            }
            else
            {
                LOG(Warning, "Missing prefab {0}.", obj->GetPrefabID());
            }
        }
        if (!serialized)
        {
            obj->Serialize(writer, obj->GetType().GetDefaultInstance());
        }

        if (includeOrderInParent && obj->HasParent())
        {
            if (obj->HasExternalLegacyOrderInParent())
            {
                writer.JKEY("OrderInParent");
                writer.Int64(obj->GetExternalOrderInParent());
            }
            else
            {
                writer.JKEY("SiblingOrderKey");
                writer.String(obj->GetExternalSiblingOrderKey().ToString());
            }
        }

        writer.EndObject();
    }

    bool BuildExternalActorFragment(Actor* actor, const HashSet<SceneObject*>& serializableObjects, SceneFragmentWrite& fragment)
    {
        rapidjson_flax::StringBuffer runtimeBuffer;
        PrettyJsonWriter writer(runtimeBuffer);
        writer.StartObject();
        writer.JKEY("Data");
        writer.StartArray();
        WriteSceneObject(writer, actor, true);
        int32 count = 1;
        for (Script* script : actor->Scripts)
        {
            if (serializableObjects.Contains(script))
            {
                WriteSceneObject(writer, script, true);
                count++;
            }
        }
        writer.EndArray(count);
        writer.EndObject();

        rapidjson_flax::Document runtime;
        runtime.Parse(runtimeBuffer.GetString(), runtimeBuffer.GetSize());
        const auto data = runtime.FindMember(DataKey);
        rapidjson_flax::Document source;
        source.SetObject();
        auto& allocator = source.GetAllocator();
        rapidjson_flax::Value objects;
        String conversionError;
        if (runtime.HasParseError() || data == runtime.MemberEnd() ||
            ScenePrefabDocument::ToSourceObjects(data->value, objects, allocator, false, conversionError))
        {
            LOG(Error, "Cannot create private scene fragment for actor '{0}': {1}", actor->GetName(), conversionError);
            return true;
        }
        rapidjson_flax::StringBuffer payload;
        PrettyJsonWriter payloadWriter(payload);
        objects.Accept(payloadWriter.GetWriter());
        fragment.RootActorLocalId = actor->GetLocalFileId();
        fragment.ContainedLocalIds.Add(fragment.RootActorLocalId);
        for (Script* script : actor->Scripts)
        {
            if (serializableObjects.Contains(script))
                fragment.ContainedLocalIds.Add(script->GetLocalFileId());
        }
        fragment.Payload.Set(reinterpret_cast<const byte*>(payload.GetString()), static_cast<int32>(payload.GetSize()));
        return false;
    }

    bool CopyDocumentMember(rapidjson_flax::Document& target, const rapidjson_flax::Value& source, const char* name)
    {
        const auto sourceMember = source.FindMember(name);
        if (sourceMember == source.MemberEnd())
            return false;
        auto& allocator = target.GetAllocator();
        rapidjson_flax::Value key = JsonKey(name, allocator);
        rapidjson_flax::Value value;
        value.CopyFrom(sourceMember->value, allocator);
        target.AddMember(key, value, allocator);
        return true;
    }

    bool ReadExternalActorDocument(const String& actorFile, const Array<byte>& fileData,
        rapidjson_flax::Document& actorDocument, const rapidjson_flax::Value*& actorData)
    {
        actorDocument.Parse(reinterpret_cast<const char*>(fileData.Get()), fileData.Count());
        if (actorDocument.HasParseError())
        {
            Log::JsonParseException(actorDocument.GetParseError(), actorDocument.GetErrorOffset(), actorFile);
            return true;
        }

        auto data = actorDocument.FindMember("payload");
        if (data == actorDocument.MemberEnd() || !data->value.IsArray())
        {
            LOG(Error, "Missing private scene fragment payload in '{0}'.", actorFile);
            return true;
        }

        actorData = &data->value;
        return false;
    }

    bool ReadExternalActorFileInfo(const String& actorFile, Array<byte>& bytes, ExternalActorFileInfo& info)
    {
        rapidjson_flax::Document actorDocument;
        const rapidjson_flax::Value* actorData = nullptr;
        if (ReadExternalActorDocument(actorFile, bytes, actorDocument, actorData))
            return true;
        if (actorData->Empty() || !(*actorData)[0].IsObject())
        {
            LOG(Error, "Invalid external actor Data member in '{0}'.", actorFile);
            return true;
        }

        info.File = actorFile;
        info.Bytes = MoveTemp(bytes);
        auto actorId = (*actorData)[0].FindMember(FileIdKey);
        if (actorId == (*actorData)[0].MemberEnd())
            actorId = (*actorData)[0].FindMember(SourceFileIdKey);
        auto parentId = (*actorData)[0].FindMember(ParentFileIdKey);
        if (parentId == (*actorData)[0].MemberEnd())
            parentId = (*actorData)[0].FindMember(SourceParentFileIdKey);
        info.ActorId = actorId != (*actorData)[0].MemberEnd() && actorId->value.IsInt64() ? actorId->value.GetInt64() : 0;
        info.ParentId = parentId != (*actorData)[0].MemberEnd() && parentId->value.IsInt64() ? parentId->value.GetInt64() : 0;
        info.SiblingOrderKey = GetSerializedSiblingOrderKey((*actorData)[0]);
        return false;
    }

    bool BuildExternalActorsSceneDocument(const JsonAssetBase* sceneAsset, rapidjson_flax::Document& document, Array<String>* externalActorFiles)
    {
        if (!sceneAsset || !sceneAsset->Data || !sceneAsset->Document.IsObject())
            return true;

        const Guid sceneGuid = sceneAsset->GetID();
        const String actorsFolder = SceneFragmentStore::GetScenePath(sceneGuid);
        if (ValidateExternalActorFragments(sceneGuid))
            return true;
        auto& allocator = document.GetAllocator();
        document.SetObject();
        CopyDocumentMember(document, sceneAsset->Document, IDKey);
        CopyDocumentMember(document, sceneAsset->Document, TypeNameKey);
        if (!CopyDocumentMember(document, sceneAsset->Document, EngineBuildKey))
        {
            document.AddMember(JsonKey(EngineBuildKey, allocator), FLAXENGINE_VERSION_BUILD, allocator);
        }

        rapidjson_flax::Value data(rapidjson::kArrayType);
        const auto sourceData = sceneAsset->Document.FindMember(DataKey);
        if (sourceData == sceneAsset->Document.MemberEnd() || !sourceData->value.IsArray() || sourceData->value.Empty())
            return true;
        for (rapidjson::SizeType i = 0; i < sourceData->value.Size(); i++)
        {
            rapidjson_flax::Value value;
            value.CopyFrom(sourceData->value[i], allocator);
            data.PushBack(value, allocator);
        }

        SceneFragmentIndex fragmentIndex;
        Array<Array<byte>> fragmentBytes;
        String fragmentError;
        if (SceneFragmentStore::Load(sceneGuid, fragmentIndex, fragmentBytes, fragmentError))
        {
            LOG(Error, "Cannot load private scene fragments for scene '{0}': {1}", sceneGuid, fragmentError);
            return true;
        }
        if (externalActorFiles)
            externalActorFiles->Add(SceneFragmentStore::GetIndexPath(sceneGuid));

        Array<ExternalActorFileInfo> actorInfos;
        HashSet<int64> actorIds;
        for (int32 fragmentIndexPosition = 0; fragmentIndexPosition < fragmentIndex.Fragments.Count(); fragmentIndexPosition++)
        {
            const String actorFile = actorsFolder / fragmentIndex.Fragments[fragmentIndexPosition].RelativePhysicalPath;
            if (externalActorFiles)
                externalActorFiles->Add(actorFile);

            ExternalActorFileInfo info;
            if (ReadExternalActorFileInfo(actorFile, fragmentBytes[fragmentIndexPosition], info))
                return true;
            if (info.ActorId == 0)
            {
                LOG(Error, "Cannot load ExternalActor fragment '{0}' because its actor identity is invalid.", actorFile);
                return true;
            }
            if (!actorIds.Add(info.ActorId))
            {
                LOG(Error, "Cannot load duplicate ExternalActor identity '{0}' from '{1}'.", info.ActorId, actorFile);
                return true;
            }
            actorInfos.Add(MoveTemp(info));
        }

        HashSet<int64> validParentIds;
        const int64 sceneFileId = 1;
        validParentIds.Add(sceneFileId);
        bool foundValidActor = true;
        while (foundValidActor)
        {
            foundValidActor = false;
            for (int32 i = 0; i < actorInfos.Count(); i++)
            {
                ExternalActorFileInfo& info = actorInfos[i];
                if (!info.IsValid && validParentIds.Contains(info.ParentId))
                {
                    info.IsValid = true;
                    validParentIds.Add(info.ActorId);
                    foundValidActor = true;
                }
            }
        }

        for (const ExternalActorFileInfo& info : actorInfos)
        {
            if (!info.IsValid)
            {
                LOG(Error, "Cannot load ExternalActor fragment '{0}' ({1}) because parent identity '{2}' is missing or invalid.",
                    info.File, info.ActorId, info.ParentId);
                return true;
            }
        }

        Sorting::QuickSort(actorInfos.Get(), actorInfos.Count(), SortExternalActorFileInfo);

        Dictionary<int64, Array<int32>> childrenByParent(actorInfos.Count());
        int32 validActors = 0;
        for (int32 i = 0; i < actorInfos.Count(); i++)
        {
            const ExternalActorFileInfo& info = actorInfos[i];
            if (info.IsValid)
            {
                childrenByParent[info.ParentId].Add(i);
                validActors++;
            }
        }

        int32 writtenActors = 0;
        Array<int32> pendingActors(actorInfos.Count());
        if (Array<int32>* rootActors = childrenByParent.TryGet(sceneFileId))
            pendingActors.Add(rootActors->Get(), rootActors->Count());
        for (int32 pendingActorIndex = 0; pendingActorIndex < pendingActors.Count(); pendingActorIndex++)
        {
            const ExternalActorFileInfo& info = actorInfos[pendingActors[pendingActorIndex]];

            rapidjson_flax::Document actorDocument;
            const rapidjson_flax::Value* actorData = nullptr;
            if (ReadExternalActorDocument(info.File, info.Bytes, actorDocument, actorData))
                return true;

            rapidjson_flax::Value runtimeObjects;
            String conversionError;
            if (ScenePrefabDocument::ToRuntimeObjects(*actorData, runtimeObjects, allocator, false, conversionError))
            {
                LOG(Error, "Cannot compile scene chunk '{0}': {1}", info.File, conversionError);
                return true;
            }
            for (rapidjson_flax::Value& value : runtimeObjects.GetArray())
                data.PushBack(value.Move(), allocator);

            writtenActors++;
            if (Array<int32>* childActors = childrenByParent.TryGet(info.ActorId))
                pendingActors.Add(childActors->Get(), childActors->Count());
        }

        if (writtenActors != validActors)
        {
            LOG(Error, "Failed to compose external actor hierarchy for scene '{0}'.", sceneAsset->GetPath());
            return true;
        }

        document.AddMember(JsonKey(DataKey, allocator), data, allocator);
        return false;
    }

    bool SaveExternalSceneAssetToBytes(const JsonAssetBase* sceneAsset, rapidjson_flax::StringBuffer& outData, Array<String>* externalActorFiles, bool prettyJson)
    {
        rapidjson_flax::Document document;
        if (BuildExternalActorsSceneDocument(sceneAsset, document, externalActorFiles))
            return true;

        if (prettyJson)
        {
            PrettyJsonWriter writer(outData);
            document.Accept(writer.GetWriter());
        }
        else
        {
            CompactJsonWriter writer(outData);
            document.Accept(writer.GetWriter());
        }
        return false;
    }
}

#endif

class SceneAction
{
public:
    virtual ~SceneAction()
    {
    }

    struct Context
    {
        // Amount of seconds that action can take to run within a budget.
        float TimeBudget = MAX_float;
    };

    virtual SceneResult Do(Context& context)
    {
        return SceneResult::Failed;
    }
};

#if USE_EDITOR

struct ScriptsReloadObject
{
    StringAnsi TypeName;
    ScriptingObject** Object;
    Array<byte> Data;
};

#endif

// Small utility for dividing the iterative work over data set that can run in equal slicer limited by time.
struct TimeSlicer
{
    int32 Index = -1;
    int32 Count = 0;
    double TimeBudget;
    double StartTime;

    void BeginSync(float timeBudget, int32 count, int32 startIndex = 0);
    bool StepSync();
    SceneResult End();
};

// Async map loading utility for state tracking and synchronization of various load stages.
class SceneLoader
{
public:
    struct Args
    {
        rapidjson_flax::Value& Data;
        StringView AssetPath;
        int32 EngineBuild;
        float TimeBudget;
    };

    enum class Stages
    {
        Begin,
        Spawn,
        SetupPrefabs,
        SyncNewPrefabs,
        Deserialize,
        SyncPrefabs,
        SetupTransforms,
        Initialize,
        BeginPlay,
        End,
        Loaded,
    } Stage = Stages::Begin;

    bool AsyncLoad;
    bool AsyncJobs;
    Guid SourceAssetId = Guid::Empty;
    Guid SceneId = Guid::Empty;
    Scene* Scene = nullptr;
    float TotalTime = 0.0f;
    uint64 StartFrame;

    // Cache data
    ISerializeModifier* Modifier = nullptr;
    ActorsCache::SceneObjectsListType* SceneObjects = nullptr;
    Array<Actor*> InjectedSceneChildren;
    SceneObjectsFactory::Context Context;
    SceneObjectsFactory::PrefabSyncData* PrefabSyncData = nullptr;
    ISerializable::SerializeDocument ExternalActorsDocument;
    bool HasExternalActorsDocument = false;
    TimeSlicer StageSlicer;

    SceneLoader(bool asyncLoad = false)
        : AsyncLoad(asyncLoad)
        , AsyncJobs(JobSystem::GetThreadsCount() > 1)
        , Modifier(Cache::ISerializeModifier.GetUnscoped())
        , Context(Modifier)
    {
        Context.SuppressMissingPrefabObjectWarnings = true;
    }

    ~SceneLoader()
    {
        if (PrefabSyncData)
            Delete(PrefabSyncData);
        if (SceneObjects)
            ActorsCache::SceneObjectsListCache.Put(SceneObjects);
        if (Modifier)
            Cache::ISerializeModifier.Put(Modifier);
    }

    NON_COPYABLE(SceneLoader);

    FORCE_INLINE void NextStage()
    {
        Stage = (Stages)((uint8)Stage + 1);
    }

    SceneResult Tick(Args& args);
    SceneResult OnBegin(Args& args);
    SceneResult OnSpawn(Args& args);
    SceneResult OnSetupPrefabs(Args& args);
    SceneResult OnSyncNewPrefabs(Args& args);
    SceneResult OnDeserialize(Args& args);
    SceneResult OnSyncPrefabs(Args& args);
    SceneResult OnSetupTransforms(Args& args);
    SceneResult OnInitialize(Args& args);
    SceneResult OnBeginPlay(Args& args);
    SceneResult OnEnd(Args& args);
};

namespace LevelImpl
{
    Array<SceneAction*> _sceneActions;
    CriticalSection _sceneActionsLocker;
    DateTime _lastSceneLoadTime(0);
#if USE_EDITOR
    Array<ScriptsReloadObject> ScriptsReloadObjects;
#endif

    void CallSceneEvent(SceneEventType eventType, Scene* scene, Guid sceneId);

    void flushActions();
    SceneResult loadScene(SceneLoader& loader, JsonAsset* sceneAsset, float* timeBudget = nullptr);
    SceneResult loadScene(SceneLoader& loader, const BytesContainer& sceneData, Scene** outScene = nullptr, float* timeBudget = nullptr);
    SceneResult loadScene(SceneLoader& loader, rapidjson_flax::Document& document, Scene** outScene = nullptr, float* timeBudget = nullptr);
    SceneResult loadScene(SceneLoader& loader, rapidjson_flax::Value& data, int32 engineBuild, Scene** outScene = nullptr, StringView assetPath = StringView(), float* timeBudget = nullptr);
    bool unloadScene(Scene* scene);
    bool unloadScenes();
    bool saveScene(Scene* scene);
    bool saveScene(Scene* scene, const String& path);
    bool saveScenes(const Array<Scene*>& scenes);
    bool saveScene(Scene* scene, rapidjson_flax::StringBuffer& outBuffer, bool prettyJson,
        bool useExternalActorsStorage = true, SceneFragmentSavePlan* fragmentPlan = nullptr);
    bool saveScene(Scene* scene, rapidjson_flax::StringBuffer& outBuffer, JsonWriter& writer, bool prettyJson,
        bool useExternalActorsStorage = true, SceneFragmentSavePlan* fragmentPlan = nullptr);
    bool spawnActor(Actor* actor, Actor* parent);
    bool deleteActor(Actor* actor);
}

using namespace LevelImpl;

class LevelService : public EngineService
{
public:
    LevelService()
        : EngineService(TEXT("Scene Manager"), 200)
    {
    }

    bool Init() override;
    void Update() override;
    void LateUpdate() override;
    void FixedUpdate() override;
    void LateFixedUpdate() override;
    void Dispose() override;
};

LevelService LevelServiceInstanceService;
extern double EngineIdleTime;

CriticalSection Level::ScenesLock;
Array<Scene*> Level::Scenes;
bool Level::TickEnabled = true;
float Level::StreamingFrameBudget = 0.3f;
Delegate<Actor*> Level::ActorSpawned;
Delegate<Actor*> Level::ActorDeleted;
Delegate<Actor*, Actor*> Level::ActorParentChanged;
Delegate<Actor*> Level::ActorOrderInParentChanged;
Delegate<Actor*> Level::ActorNameChanged;
Delegate<Actor*> Level::ActorActiveChanged;
#if USE_EDITOR
Delegate<Actor*> Level::ActorDestroyChildren;
#endif
Delegate<Scene*, const Guid&> Level::SceneSaving;
Delegate<Scene*, const Guid&> Level::SceneSaved;
Delegate<Scene*, const Guid&> Level::SceneSaveError;
Delegate<Scene*, const Guid&> Level::SceneLoading;
Delegate<Scene*, const Guid&> Level::SceneLoaded;
Delegate<Scene*, const Guid&> Level::SceneLoadError;
Delegate<Scene*, const Guid&> Level::SceneUnloading;
Delegate<Scene*, const Guid&> Level::SceneUnloaded;
#if USE_EDITOR
Action Level::ScriptsReloadStart;
Action Level::ScriptsReload;
Action Level::ScriptsReloaded;
Action Level::ScriptsReloadEnd;
#endif
String Level::Layers[32];

bool LevelImpl::spawnActor(Actor* actor, Actor* parent)
{
    if (actor == nullptr)
    {
        Log::ArgumentNullException(TEXT("Cannot spawn null actor."));
        return true;
    }

    if (actor->GetType().ManagedClass->IsAbstract())
    {
        Log::Exception(TEXT("Cannot spawn abstract actor type."));
        return true;
    }

    if (actor->Is<Scene>())
    {
        // Spawn scene
        actor->InitializeHierarchy();
        actor->OnTransformChanged();
        {
            SceneBeginData beginData;
            actor->BeginPlay(&beginData);
            beginData.OnDone();
        }
        CallSceneEvent(SceneEventType::OnSceneLoaded, (Scene*)actor, actor->GetID());
    }
    else
    {
        // Spawn actor
        if (Level::Scenes.IsEmpty())
        {
            Log::InvalidOperationException(TEXT("Cannot spawn actor. No scene loaded."));
            return true;
        }
        if (parent == nullptr)
            parent = Level::Scenes[0];

        actor->SetPhysicsScene(parent->GetPhysicsScene());
        actor->SetParent(parent, true, true);
    }

    return false;
}

bool LevelImpl::deleteActor(Actor* actor)
{
    if (actor == nullptr)
    {
        Log::ArgumentNullException(TEXT("Cannot delete null actor."));
        return true;
    }

    actor->DeleteObject();

    return false;
}

void LayersAndTagsSettings::Apply()
{
    // Note: we cannot remove tags/layers at runtime so this should deserialize them in additive mode
    // Tags/Layers are stored as index in actors so collection change would break the linkage
    for (auto& tag : Tags)
    {
        Tags::Get(tag);
    }
    for (int32 i = 0; i < ARRAY_COUNT(Level::Layers); i++)
    {
        const auto& src = Layers[i];
        auto& dst = Level::Layers[i];
        if (dst.IsEmpty() || !src.IsEmpty())
            dst = src;
    }
}

#define TICK_LEVEL(tickingStage, name) \
    PROFILE_CPU_NAMED(name); \
    PROFILE_MEM(Level); \
    ScopeLock lock(Level::ScenesLock); \
    auto& scenes = Level::Scenes; \
    if (!Time::GetGamePaused() && Level::TickEnabled) \
    { \
        for (int32 i = 0; i < scenes.Count(); i++) \
        { \
            if (scenes[i]->GetIsActive()) \
                scenes[i]->Ticking.tickingStage.Tick(); \
        } \
    }
#if USE_EDITOR
#define TICK_LEVEL_EDITOR(tickingStage) \
    else if (!Editor::IsPlayMode) \
    { \
        for (int32 i = 0; i < scenes.Count(); i++) \
        { \
            if (scenes[i]->GetIsActive()) \
                scenes[i]->Ticking.tickingStage.TickExecuteInEditor(); \
        } \
    }
#else
#define TICK_LEVEL_EDITOR(tickingStage)
#endif

bool LevelService::Init()
{
    return false;
}

void LevelService::Update()
{
    TICK_LEVEL(Update, "Level::Update")
    TICK_LEVEL_EDITOR(Update)
}

void LevelService::LateUpdate()
{
    TICK_LEVEL(LateUpdate, "Level::LateUpdate")
    TICK_LEVEL_EDITOR(LateUpdate)
    flushActions();
}

void LevelService::FixedUpdate()
{
    TICK_LEVEL(FixedUpdate, "Level::FixedUpdate")
    TICK_LEVEL_EDITOR(FixedUpdate)
}

void LevelService::LateFixedUpdate()
{
    TICK_LEVEL(LateFixedUpdate, "Level::LateFixedUpdate")
    TICK_LEVEL_EDITOR(LateFixedUpdate)
}

#undef TICK_LEVEL
#undef TICK_LEVEL_EDITOR

void LevelService::Dispose()
{
    // End scene actions
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.ClearDelete();

    // Unload scenes
    unloadScenes();

    // Ensure that all scenes and actors has been destroyed (we don't leak!)
    ASSERT(Level::Scenes.IsEmpty());
}

bool Level::IsAnyActorInGame()
{
    ScopeLock lock(ScenesLock);

    for (int32 i = 0; i < Scenes.Count(); i++)
    {
        if (Scenes[i]->Children.HasItems())
            return true;
    }

    return false;
}

bool Level::IsAnyActionPending()
{
    _sceneActionsLocker.Lock();
    const bool result = _sceneActions.HasItems();
    _sceneActionsLocker.Unlock();
    return result;
}

DateTime Level::GetLastSceneLoadTime()
{
    return _lastSceneLoadTime;
}

bool Level::SpawnActor(Actor* actor, Actor* parent)
{
    ASSERT(actor);
    ScopeLock lock(_sceneActionsLocker);
    return spawnActor(actor, parent);
}

bool Level::DeleteActor(Actor* actor)
{
    ASSERT(actor);
    ScopeLock lock(_sceneActionsLocker);
    return deleteActor(actor);
}

void Level::CallBeginPlay(Actor* obj)
{
    if (obj && !obj->IsDuringPlay())
    {
        SceneBeginData beginData;
        obj->BeginPlay(&beginData);
        beginData.OnDone();
    }
}

void Level::DrawActors(RenderContextBatch& renderContextBatch, byte category)
{
    PROFILE_CPU();

    //ScopeLock lock(ScenesLock);

    for (Scene* scene : Scenes)
    {
        if (scene->IsActiveInHierarchy())
            scene->Rendering.Draw(renderContextBatch, (SceneRendering::DrawCategory)category);
    }
}

void Level::CollectPostFxVolumes(RenderContext& renderContext)
{
    PROFILE_CPU();

    //ScopeLock lock(ScenesLock);

    for (Scene* scene : Scenes)
    {
        if (scene->IsActiveInHierarchy())
            scene->Rendering.CollectPostFxVolumes(renderContext);
    }
}

class LoadSceneAction : public SceneAction
{
public:
    Guid SceneId;
    AssetReference<JsonAsset> SceneAsset;
    SceneLoader Loader;

    LoadSceneAction(const Guid& sceneId, JsonAsset* sceneAsset, bool async)
        : Loader(async)
    {
        SceneId = sceneId;
        SceneAsset = sceneAsset;
    }

    SceneResult Do(Context& context) override
    {
        if (SceneAsset == nullptr)
            return SceneResult::Failed;
        if (!SceneAsset->IsLoaded())
            return SceneResult::Wait;
        return LevelImpl::loadScene(Loader, SceneAsset, &context.TimeBudget);
    }
};

class UnloadSceneAction : public SceneAction
{
public:
    Guid TargetScene;

    UnloadSceneAction(Scene* scene)
    {
        TargetScene = scene->GetID();
    }

    SceneResult Do(Context& context) override
    {
        auto scene = Level::FindScene(TargetScene);
        if (!scene)
            return SceneResult::Failed;
        return unloadScene(scene) ? SceneResult::Failed : SceneResult::Success;
    }
};

class UnloadScenesAction : public SceneAction
{
public:
    UnloadScenesAction()
    {
    }

    SceneResult Do(Context& context) override
    {
        return unloadScenes() ? SceneResult::Failed : SceneResult::Success;
    }
};

class SaveSceneAction : public SceneAction
{
public:
    Scene* TargetScene;
    bool PrettyJson;

    SaveSceneAction(Scene* scene, bool prettyJson = true)
    {
        TargetScene = scene;
        PrettyJson = prettyJson;
    }

    SceneResult Do(Context& context) override
    {
        if (saveScene(TargetScene))
        {
            LOG(Error, "Failed to save scene {0}", TargetScene ? TargetScene->GetName() : String::Empty);
            return SceneResult::Failed;
        }
        return SceneResult::Success;
    }
};

class SaveScenesAction : public SceneAction
{
public:
    Array<Scene*> TargetScenes;

    explicit SaveScenesAction(const Array<Scene*>& scenes)
        : TargetScenes(scenes)
    {
    }

    SceneResult Do(Context& context) override
    {
        if (saveScenes(TargetScenes))
        {
            LOG(Error, "Failed to save scene batch.");
            return SceneResult::Failed;
        }
        return SceneResult::Success;
    }
};

#if USE_EDITOR

class ReloadScriptsAction : public SceneAction
{
public:
    ReloadScriptsAction()
    {
    }

    SceneResult Do(Context& context) override
    {
        // Reloading scripts workflow:
        // - save scenes (to temporary files)
        // - unload scenes
        // - unload user assemblies
        // - load user assemblies
        // - load scenes (from temporary files)
        // Note: we don't want to override original scene files

        PROFILE_CPU_NAMED("Level.ReloadScripts");
        PROFILE_MEM(Level);
        LOG(Info, "Scripts reloading start");
        const auto startTime = DateTime::NowUTC();

        // Cache data
        struct SceneData
        {
            Guid ID;
            String Name;
            rapidjson_flax::StringBuffer Data;

            SceneData() = default;

            SceneData(const SceneData& other)
            {
                CRASH;
            }

            const SceneData& operator=(SceneData&) const
            {
                CRASH;
                return *this;
            }

            void Init(Scene* scene)
            {
                ID = scene->GetID();
                Name = scene->GetName();
            }
        };
        const int32 scenesCount = Level::Scenes.Count();
        Array<SceneData> scenes;
        scenes.Resize(scenesCount);
        for (int32 i = 0; i < scenesCount; i++)
            scenes[i].Init(Level::Scenes[i]);

        // Fire event
        Level::ScriptsReloadStart();

        // Save scenes (to memory)
        for (int32 i = 0; i < scenesCount; i++)
        {
            const auto scene = Level::Scenes[i];
            LOG(Info, "Caching scene {0}", scenes[i].Name);

            // Serialize to json
            if (saveScene(scene, scenes[i].Data, false, false))
            {
                LOG(Error, "Failed to save scene '{0}' for scripts reload.", scenes[i].Name);
                CallSceneEvent(SceneEventType::OnSceneSaveError, scene, scene->GetID());
                return SceneResult::Failed;
            }
            CallSceneEvent(SceneEventType::OnSceneSaved, scene, scene->GetID());
        }

        // Unload scenes
        unloadScenes();

        // Reload scripting
        Level::ScriptsReload();
        Scripting::Reload();
        Level::ScriptsReloaded();

        // Restore objects
        for (auto& e : ScriptsReloadObjects)
        {
            const ScriptingTypeHandle typeHandle = Scripting::FindScriptingType(e.TypeName);
            *e.Object = ScriptingObject::NewObject(typeHandle);
            if (!*e.Object)
            {
                LOG(Warning, "Failed to restore hot-reloaded object of type {0}.", String(e.TypeName));
                continue;
            }
            auto* serializable = ScriptingObject::ToInterface<ISerializable>(*e.Object);
            if (serializable && e.Data.HasItems())
            {
                JsonSerializer::LoadFromBytes(serializable, e.Data, FLAXENGINE_VERSION_BUILD);
            }
        }
        ScriptsReloadObjects.Clear();

        // Restore scenes (from memory)
        for (int32 i = 0; i < scenesCount; i++)
        {
            LOG(Info, "Restoring scene {0}", scenes[i].Name);

            // Parse json
            const auto& sceneData = scenes[i].Data;
            ISerializable::SerializeDocument document;
            {
                PROFILE_CPU_NAMED("Json.Parse");
                document.Parse(sceneData.GetString(), sceneData.GetSize());
            }
            if (document.HasParseError())
            {
                LOG(Error, "Failed to deserialize scene {0}. SceneResult: {1}", scenes[i].Name, GetParseError_En(document.GetParseError()));
                return SceneResult::Failed;
            }

            // Load scene
            SceneLoader loader;
            if (LevelImpl::loadScene(loader, document) != SceneResult::Success)
            {
                LOG(Error, "Failed to deserialize scene {0}", scenes[i].Name);
                CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, scenes[i].ID);
                return SceneResult::Failed;
            }
        }
        scenes.Resize(0);

        // Fire event
        LOG(Info, "Scripts reloading end. Total time: {0}ms", static_cast<int32>((DateTime::NowUTC() - startTime).GetTotalMilliseconds()));
        Level::ScriptsReloadEnd();

        return SceneResult::Success;
    }
};

void Level::ScriptsReloadRegisterObject(ScriptingObject*& obj)
{
    if (!obj)
        return;
    auto& e = ScriptsReloadObjects.AddOne();
    e.Object = &obj;
    e.TypeName = obj->GetType().Fullname;
    if (auto* serializable = ScriptingObject::ToInterface<ISerializable>(obj))
        e.Data = JsonSerializer::SaveToBytes(serializable);
    ScriptingObject* o = obj;
    obj = nullptr;
    o->DeleteObjectNow();
}

#endif

class SpawnActorAction : public SceneAction
{
public:
    ScriptingObjectReference<Actor> TargetActor;
    ScriptingObjectReference<Actor> ParentActor;

    SpawnActorAction(Actor* actor, Actor* parent)
        : TargetActor(actor)
        , ParentActor(parent)
    {
    }

    SceneResult Do(Context& context) override
    {
        return spawnActor(TargetActor, ParentActor) ? SceneResult::Failed : SceneResult::Success;
    }
};

class DeleteActorAction : public SceneAction
{
public:
    ScriptingObjectReference<Actor> TargetActor;

    DeleteActorAction(Actor* actor)
        : TargetActor(actor)
    {
    }

    SceneResult Do(Context& context) override
    {
        return deleteActor(TargetActor) ? SceneResult::Failed : SceneResult::Success;
    }
};

void LevelImpl::CallSceneEvent(SceneEventType eventType, Scene* scene, Guid sceneId)
{
    PROFILE_CPU_NAMED("Level::CallSceneEvent");

    // Call event
    const auto scriptsDomain = Scripting::GetScriptsDomain();
    if (scriptsDomain != nullptr)
        scriptsDomain->Dispatch();
    switch (eventType)
    {
    case SceneEventType::OnSceneSaving:
        Level::SceneSaving(scene, sceneId);
        break;
    case SceneEventType::OnSceneSaved:
        Level::SceneSaved(scene, sceneId);
        break;
    case SceneEventType::OnSceneSaveError:
        Level::SceneSaveError(scene, sceneId);
        break;
    case SceneEventType::OnSceneLoading:
        Level::SceneLoading(scene, sceneId);
        break;
    case SceneEventType::OnSceneLoaded:
        Level::SceneLoaded(scene, sceneId);
        break;
    case SceneEventType::OnSceneLoadError:
        Level::SceneLoadError(scene, sceneId);
        break;
    case SceneEventType::OnSceneUnloading:
        Level::SceneUnloading(scene, sceneId);
        break;
    case SceneEventType::OnSceneUnloaded:
        Level::SceneUnloaded(scene, sceneId);
        break;
    }
}

int32 Level::GetNonEmptyLayerNamesCount()
{
    int32 result = 31;
    while (result >= 0 && Layers[result].IsEmpty())
        result--;
    return result + 1;
}

int32 Level::GetLayerIndex(const StringView& layer)
{
    int32 result = -1;
    for (int32 i = 0; i < 32; i++)
    {
        if (Layers[i] == layer)
        {
            result = i;
            break;
        }
    }
    return result;
}

StringView Level::GetLayerName(const int32 layerIndex)
{
    for (int32 i = 0; i < 32; i++)
    {
        if (i == layerIndex)
        {
            return Layers[i];
        }
    }
    return TEXT("");
}

void Level::callActorEvent(ActorEventType eventType, Actor* a, Actor* b)
{
    PROFILE_CPU();

    ASSERT(a);

    // Call event
    const auto scriptsDomain = Scripting::GetScriptsDomain();
    if (scriptsDomain != nullptr)
        scriptsDomain->Dispatch();
    switch (eventType)
    {
    case ActorEventType::OnActorSpawned:
        ActorSpawned(a);
        break;
    case ActorEventType::OnActorDeleted:
        ActorDeleted(a);
        break;
    case ActorEventType::OnActorParentChanged:
        ActorParentChanged(a, b);
        break;
    case ActorEventType::OnActorOrderInParentChanged:
        ActorOrderInParentChanged(a);
        break;
    case ActorEventType::OnActorNameChanged:
        ActorNameChanged(a);
        break;
    case ActorEventType::OnActorActiveChanged:
        ActorActiveChanged(a);
        break;
#if USE_EDITOR
    case ActorEventType::OnActorDestroyChildren:
        ActorDestroyChildren(a);
        break;
#endif
    }
}

void LevelImpl::flushActions()
{
    // Calculate time budget for the streaming (relative to the game frame rate to scale across different devices)
    SceneAction::Context context;
    float targetFps = 60;
    if (Time::UpdateFPS > ZeroTolerance)
        targetFps = Time::UpdateFPS;
    else if (Engine::GetFramesPerSecond() > 0)
        targetFps = (float)Engine::GetFramesPerSecond();
    context.TimeBudget = Level::StreamingFrameBudget / targetFps;
    if (EngineIdleTime > 0.001)
        context.TimeBudget += (float)(EngineIdleTime * 0.5); // Increase time budget if engine has some idle time for spare
#if USE_EDITOR
    // Throttle up in Editor
    context.TimeBudget *= Editor::IsPlayMode ? 1.2f : 2.0f;
#endif
#if BUILD_DEBUG
    // Throttle up in Debug
    context.TimeBudget *= 1.2f;
#endif
    if (context.TimeBudget <= 0.0f)
        context.TimeBudget = MAX_float; // Unlimited if 0
    context.TimeBudget = Math::Max(context.TimeBudget, 0.001f); // Minimum 1ms

    // Runs actions in order
    ScopeLock lock(_sceneActionsLocker);
    for (int32 i = 0; i < _sceneActions.Count() && context.TimeBudget >= 0.0; i++)
    {
        auto action = _sceneActions[0];
        Stopwatch time;
        auto result = action->Do(context);
        time.Stop();
        context.TimeBudget -= time.GetTotalSeconds();
        if (result != SceneResult::Wait)
        {
            _sceneActions.RemoveAtKeepOrder(i--);
            Delete(action);
        }
    }
}

bool LevelImpl::unloadScene(Scene* scene)
{
    if (scene == nullptr)
    {
        Log::ArgumentNullException();
        return true;
    }
    const auto sceneId = scene->GetID();

    PROFILE_CPU_NAMED("Level.UnloadScene");
    PROFILE_MEM(Level);

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneUnloading, scene, sceneId);

    // Call end play
    if (scene->IsDuringPlay())
        scene->EndPlay();

    // Remove from scenes list
    Level::Scenes.Remove(scene);

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneUnloaded, scene, sceneId);

    // Simple enqueue scene root object to be deleted
    scene->DeleteObject();

    return false;
}

bool LevelImpl::unloadScenes()
{
    PROFILE_MEM(Level);
    auto scenes = Level::Scenes;
    for (int32 i = scenes.Count() - 1; i >= 0; i--)
    {
        if (unloadScene(scenes[i]))
            return true;
    }
    return false;
}

SceneResult LevelImpl::loadScene(SceneLoader& loader, JsonAsset* sceneAsset, float* timeBudget)
{
    // Keep reference to the asset (prevent unloading during action)
    AssetReference<JsonAsset> ref = sceneAsset;
    if (sceneAsset == nullptr || sceneAsset->WaitForLoaded())
    {
        LOG(Error, "Cannot load scene asset.");
        return SceneResult::Failed;
    }
    loader.SourceAssetId = sceneAsset->GetPersistentObjectId();

#if USE_EDITOR
    if (Level::IsExternalActorsSceneAsset(sceneAsset))
    {
        if (!loader.HasExternalActorsDocument)
        {
            if (BuildExternalActorsSceneDocument(sceneAsset, loader.ExternalActorsDocument, nullptr))
                return SceneResult::Failed;
            loader.HasExternalActorsDocument = true;
        }
        auto data = loader.ExternalActorsDocument.FindMember("Data");
        if (data == loader.ExternalActorsDocument.MemberEnd())
        {
            LOG(Error, "Missing Data member.");
            return SceneResult::Failed;
        }
        return loadScene(loader, data->value, sceneAsset->DataEngineBuild, nullptr, sceneAsset->GetPath(), timeBudget);
    }
#endif

    return loadScene(loader, *sceneAsset->Data, sceneAsset->DataEngineBuild, nullptr, sceneAsset->GetPath(), timeBudget);
}

SceneResult LevelImpl::loadScene(SceneLoader& loader, const BytesContainer& sceneData, Scene** outScene, float* timeBudget)
{
    if (sceneData.IsInvalid())
    {
        LOG(Error, "Missing scene data.");
        return SceneResult::Failed;
    }
    PROFILE_MEM(Level);

    // Parse scene JSON file
    rapidjson_flax::Document document;
    {
        PROFILE_CPU_NAMED("Json.Parse");
        document.Parse(sceneData.Get<char>(), sceneData.Length());
    }
    if (document.HasParseError())
    {
        Log::JsonParseException(document.GetParseError(), document.GetErrorOffset());
        return SceneResult::Failed;
    }

    ScopeLock lock(Level::ScenesLock);
    return loadScene(loader, document, outScene, timeBudget);
}

SceneResult LevelImpl::loadScene(SceneLoader& loader, rapidjson_flax::Document& document, Scene** outScene, float* timeBudget)
{
    loader.SourceAssetId = JsonTools::GetGuid(document, "ID");
    auto data = document.FindMember("Data");
    if (data == document.MemberEnd())
    {
        LOG(Error, "Missing Data member.");
        return SceneResult::Failed;
    }
    const int32 saveEngineBuild = JsonTools::GetInt(document, "EngineBuild", 0);
    return loadScene(loader, data->value, saveEngineBuild, outScene, nullptr, timeBudget);
}

SceneResult LevelImpl::loadScene(SceneLoader& loader, rapidjson_flax::Value& data, int32 engineBuild, Scene** outScene, StringView assetPath, float* timeBudget)
{
    PROFILE_CPU_NAMED("Level.LoadScene");
    PROFILE_MEM(Level);
#if USE_EDITOR
    ContentDeprecated::Clear();
#endif
    SceneResult result = SceneResult::Success;
    float timeLeft = timeBudget ? *timeBudget : MAX_float;
    SceneLoader::Args args = { data, assetPath, engineBuild, timeLeft };
    while (timeLeft > 0.0f && loader.Stage != SceneLoader::Stages::Loaded)
    {
        Stopwatch time;
        result = loader.Tick(args);
        time.Stop();
        const float delta = time.GetTotalSeconds();
        loader.TotalTime += delta;
        timeLeft -= delta;
        if (timeLeft < 0.0f && result == SceneResult::Success)
        {
            result = SceneResult::Wait;
            break;
        }
    }
    if (outScene)
        *outScene = loader.Scene;
    return result;
}

void TimeSlicer::BeginSync(float timeBudget, int32 count, int32 startIndex)
{
    if (Index == -1)
    {
        // Starting
        Index = startIndex;
        Count = count;
    }
    TimeBudget = (double)timeBudget;
    StartTime = Platform::GetTimeSeconds();
}

bool TimeSlicer::StepSync()
{
    Index++;
    double time = Platform::GetTimeSeconds();
    double dt = time - StartTime;
    return dt >= TimeBudget;
}

SceneResult TimeSlicer::End()
{
    if (Index >= Count)
    {
        // Finished
        *this = TimeSlicer();
        return SceneResult::Success;
    }

    return SceneResult::Wait;
}

SceneResult SceneLoader::Tick(Args& args)
{
    switch (Stage)
    {
    case Stages::Begin:
        return OnBegin(args);
    case Stages::Spawn:
        return OnSpawn(args);
    case Stages::SetupPrefabs:
        return OnSetupPrefabs(args);
    case Stages::SyncNewPrefabs:
        return OnSyncNewPrefabs(args);
    case Stages::Deserialize:
        return OnDeserialize(args);
    case Stages::SyncPrefabs:
        return OnSyncPrefabs(args);
    case Stages::Initialize:
        return OnInitialize(args);
    case Stages::SetupTransforms:
        return OnSetupTransforms(args);
    case Stages::BeginPlay:
        return OnBeginPlay(args);
    case Stages::End:
        return OnEnd(args);
    default:
        return SceneResult::Failed;
    }
}

SceneResult SceneLoader::OnBegin(Args& args)
{
    PROFILE_CPU_NAMED("Begin");
    LOG(Info, "Loading scene...");
    _lastSceneLoadTime = DateTime::Now();
    StartFrame = Engine::UpdateCount;

    // Validate arguments
    if (!args.Data.IsArray())
    {
        LOG(Error, "Invalid Data member.");
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, Guid::Empty);
        return SceneResult::Failed;
    }

    // Scene source identity comes from the asset metadata/header, never from a runtime object ID.
    SceneId = SourceAssetId;
    if (!SceneId.IsValid())
    {
        LOG(Error, "Invalid scene id.");
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, SceneId);
        return SceneResult::Failed;
    }

    // Peek meta
    if (args.EngineBuild < 6000)
    {
        LOG(Error, "Invalid serialized engine build.");
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, SceneId);
        return SceneResult::Failed;
    }
    Modifier->EngineBuild = args.EngineBuild;

    // Scripting backend should be loaded for the current project before loading scene
    if (!Scripting::HasGameModulesLoaded())
    {
        LOG(Error, "Cannot load scene without game modules loaded.");
#if USE_EDITOR
        if (!CommandLine::Options.Headless.IsTrue())
        {
            if (ScriptsBuilder::LastCompilationFailed())
                MessageBox::Show(TEXT("Script compilation failed.\n\nCannot load scene without game script modules. Please fix any compilation issues.\n\nSee Editor Console or logs for more info."), TEXT("Failed to compile scripts"), MessageBoxButtons::OK, MessageBoxIcon::Error);
            else
                MessageBox::Show(TEXT("Failed to load scripts.\n\nCannot load scene without game script modules.\n\nSee logs for more info."), TEXT("Missing game modules"), MessageBoxButtons::OK, MessageBoxIcon::Error);
        }
#endif
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, SceneId);
        return SceneResult::Failed;
    }

    // Skip is that scene is already loaded
    if (Level::FindScene(SceneId) != nullptr)
    {
        LOG(Info, "Scene {0} is already loaded.", SceneId);
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, SceneId);
        return SceneResult::Failed;
    }

    // Create scene actor
    Scene = New<::Scene>(ScriptingObjectSpawnParams(SceneId, Scene::TypeInitializer));
    Scene->_persistentSourceAsset = AssetGuid(SceneId);
    Scene->_localFileId = 1;
    Modifier->CurrentSourceAssetId = SceneId;
    Modifier->CurrentDocumentKind = GlobalObjectKind::SceneObject;
    Context.SourceAssetId = SceneId;
    Context.DocumentKind = GlobalObjectKind::SceneObject;
    Scene->RegisterObject();
    Scene->Deserialize(args.Data[0], Modifier);

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneLoading, Scene, SceneId);

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnSpawn(Args& args)
{
    PROFILE_CPU_NAMED("Spawn");

    // Get any injected children of the scene.
    InjectedSceneChildren = Scene->Children;

    // Allocate scene objects list
    SceneObjects = ActorsCache::SceneObjectsListCache.GetUnscoped();
    const int32 dataCount = (int32)args.Data.Size();
    SceneObjects->Resize(dataCount);
    SceneObjects->At(0) = Scene;
    AsyncJobs &= dataCount > 10;

    // Spawn all scene objects
    Context.Async = AsyncJobs;
    SceneObject** objects = SceneObjects->Get();
    if (Context.Async)
    {
        Level::ScenesLock.Unlock(); // Unlock scenes from Main Thread so Job Threads can use it to safely setup actors hierarchy (see Actor::Deserialize)
        JobSystem::Execute([&](int32 i)
        {
            PROFILE_MEM(Level);
            i++; // Start from 1. at index [0] was scene
            auto& stream = args.Data[i];
            bool missingPrefabObject;
            auto obj = SceneObjectsFactory::Spawn(Context, stream, &missingPrefabObject);
            objects[i] = obj;
            if (obj)
            {
                if (!obj->IsRegistered())
                    obj->RegisterObject();
#if USE_EDITOR
                // Auto-create C# objects for all actors in Editor during scene load when running in async (so main thread already has all of them)
                if (!obj->GetManagedInstance())
                    obj->CreateManaged();
#endif
            }
            else if (!missingPrefabObject || !Context.SuppressMissingPrefabObjectWarnings)
                SceneObjectsFactory::HandleObjectDeserializationError(stream);
        }, dataCount - 1);
        Level::ScenesLock.Lock();
    }
    else
    {
        for (int32 i = 1; i < dataCount; i++) // start from 1. at index [0] was scene
        {
            auto& stream = args.Data[i];
            bool missingPrefabObject;
            auto obj = SceneObjectsFactory::Spawn(Context, stream, &missingPrefabObject);
            objects[i] = obj;
            if (obj)
                obj->RegisterObject();
            else if (!missingPrefabObject || !Context.SuppressMissingPrefabObjectWarnings)
                SceneObjectsFactory::HandleObjectDeserializationError(stream);
        }
    }

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnSetupPrefabs(Args& args)
{
    // Capture prefab instances in a scene to restore any missing objects (eg. newly added objects to prefab that are missing in scene file)
    PrefabSyncData = New<SceneObjectsFactory::PrefabSyncData>(*SceneObjects, args.Data, Modifier);
    SceneObjectsFactory::SetupPrefabInstances(Context, *PrefabSyncData);

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnSyncNewPrefabs(Args& args)
{
    // Sync the new prefab instances by spawning missing objects that were added to prefab but were not saved in a scene
    // TODO: resave and force sync scenes during game cooking so this step could be skipped in game
    SceneObjectsFactory::SynchronizeNewPrefabInstances(Context, *PrefabSyncData);

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnDeserialize(Args& args)
{
    PROFILE_CPU_NAMED("Deserialize");
    const int32 dataCount = (int32)args.Data.Size();
    SceneObject** objects = SceneObjects->Get();
    bool wasAsync = Context.Async;
    Context.Async = false; // TODO: before doing full async for scene objects fix:
    // TODO: - fix Actor's Scripts and Children order when loading objects data out of order via async jobs
    // TODO: - add _loadNoAsync flag to SceneObject or Actor to handle non-async loading for those types (eg. UIControl/UICanvas)

    // Load all scene objects
    if (Context.Async)
    {
        Level::ScenesLock.Unlock(); // Unlock scenes from Main Thread so Job Threads can use it to safely setup actors hierarchy (see Actor::Deserialize)
#if USE_EDITOR
        volatile int64 deprecated = 0;
#endif
        JobSystem::Execute([&](int32 i)
        {
            i++; // Start from 1. at index [0] was scene
            auto obj = objects[i];
            if (obj)
            {
                auto& idMapping = Scripting::ObjectsLookupIdMapping.Get();
                idMapping = &Context.GetModifier()->IdsMapping;
                SceneObjectsFactory::Deserialize(Context, obj, args.Data[i]);
#if USE_EDITOR
                if (ContentDeprecated::Clear())
                    Platform::InterlockedIncrement(&deprecated);
#endif
                idMapping = nullptr;
            }
        }, dataCount - 1);
#if USE_EDITOR
        if (deprecated != 0)
            ContentDeprecated::Mark();
#endif
        Level::ScenesLock.Lock();
    }
    else
    {
        Scripting::ObjectsLookupIdMapping.Set(&Modifier->IdsMapping);
        StageSlicer.BeginSync(args.TimeBudget, dataCount, 1); // start from 1. at index [0] was scene
        while (StageSlicer.Index < StageSlicer.Count)
        {
            auto& objData = args.Data[StageSlicer.Index];
            auto obj = objects[StageSlicer.Index];
            if (obj)
                SceneObjectsFactory::Deserialize(Context, obj, objData);
            if (StageSlicer.StepSync())
                break;
        }
        Scripting::ObjectsLookupIdMapping.Set(nullptr);
    }
    Context.Async = wasAsync;

    auto result = StageSlicer.End();
    if (result != SceneResult::Wait)
        NextStage();
    return result;
}

SceneResult SceneLoader::OnSyncPrefabs(Args& args)
{
    // Add injected children of scene (via OnSceneLoading) into sceneObjects to be initialized
    for (auto child : InjectedSceneChildren)
    {
        Array<SceneObject*> injectedSceneObjects;
        injectedSceneObjects.Add(child);
        SceneQuery::GetAllSceneObjects(child, injectedSceneObjects);
        for (auto o : injectedSceneObjects)
        {
            if (!o->IsRegistered())
                o->RegisterObject();
            SceneObjects->Add(o);
        }
    }

    // Synchronize prefab instances (prefab may have objects removed or reordered so deserialized instances need to synchronize with it)
    // TODO: resave and force sync scenes during game cooking so this step could be skipped in game
    SceneObjectsFactory::SynchronizePrefabInstances(Context, *PrefabSyncData);

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnSetupTransforms(Args& args)
{
    // Cache actor transformations
    PROFILE_CPU_NAMED("SetupTransforms");
    Scene->OnTransformChanged();

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnInitialize(Args& args)
{
    // Initialize scene objects
    PROFILE_CPU_NAMED("Initialize");
    ASSERT_LOW_LAYER(IsInMainThread());
    SceneObject** objects = SceneObjects->Get();
    for (int32 i = 0; i < SceneObjects->Count(); i++)
    {
        SceneObject* obj = objects[i];
        if (obj)
        {
            obj->Initialize();

            // Delete objects without parent
            if (i != 0 && obj->GetParent() == nullptr)
            {
                LOG(Warning, "Scene object {0} {1} has missing parent object after load. Removing it.", obj->GetID(), obj->ToString());
                obj->DeleteObject();
            }
        }
    }
    PrefabSyncData->InitNewObjects();

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnBeginPlay(Args& args)
{
    PROFILE_CPU_NAMED("BeginPlay");
    ASSERT_LOW_LAYER(IsInMainThread());

    // Link scene
    ScopeLock lock(Level::ScenesLock);
    Level::Scenes.Add(Scene);

    // TODO: prototype time-slicing with load-balancing for Begin Play:
    // TODO: - collect all actors to enable
    // TODO: - invoke in order OnBeginPlay -> Child Actors Begin -> Child Scripts Begin -> OnEnable for each actor
    // TODO: - consider not drawing level until it's fully loaded (other engine systems should respect this too?)
    // TODO: - consider refactoring Joints creation maybe? to get rid of SceneBeginData

    // Start the game for scene objects
    SceneBeginData beginData;
    Scene->BeginPlay(&beginData);
    beginData.OnDone();

    NextStage();
    return SceneResult::Success;
}

SceneResult SceneLoader::OnEnd(Args& args)
{
    PROFILE_CPU_NAMED("End");
    Stopwatch time;

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneLoaded, Scene, SceneId);

    time.Stop();
    LOG(Info, "Scene loaded in {}ms ({} frames)", (int32)((TotalTime + time.GetTotalSeconds()) * 1000.0), Engine::UpdateCount - StartFrame);

#if USE_EDITOR
    // Resave assets that use deprecated data format
    for (auto& e : Context.DeprecatedPrefabs)
    {
        AssetReference<Prefab> prefab = e.Item;
        LOG(Info, "Resaving asset '{}' that uses deprecated data format", prefab->GetPath());
        if (prefab->Resave())
        {
            LOG(Error, "Failed to resave asset '{}'", prefab->GetPath());
        }
    }
    if (ContentDeprecated::Clear() && args.AssetPath != StringView())
    {
        LOG(Info, "Resaving asset '{}' that uses deprecated data format", args.AssetPath);
        if (saveScene(Scene, args.AssetPath))
        {
            LOG(Error, "Failed to resave asset '{}'", args.AssetPath);
        }
    }
#endif

    NextStage();
    return SceneResult::Success;
}

bool LevelImpl::saveScene(Scene* scene)
{
#if USE_EDITOR
    const auto path = scene->GetPath();
    if (path.IsEmpty())
    {
        LOG(Error, "Missing scene path.");
        return true;
    }

    return saveScene(scene, path);
#else
    LOG(Error, "Cannot save data to the cooked content.");
    return false;
#endif
}

#if USE_EDITOR
bool PublishSavedSceneSource(Scene* scene, const StringView& path)
{
    Array<String> paths;
    paths.Add(String(path));
    AssetPipelineDiagnostic diagnostic;
    if (AssetPipelineService::RefreshSources(paths, false, diagnostic))
    {
        LOG(Error, "Cannot refresh saved scene source '{0}': {1}", path, diagnostic.Message);
        return true;
    }
    AssetDatabaseRecordInfo record;
    if (!AssetDatabaseQueryService::TryGetRecord(scene->GetID(), record) || !record.IsMain)
    {
        LOG(Error, "Saved scene source '{0}' was not published under scene ID {1}.", path, scene->GetID());
        return true;
    }
    if (AssetPipelineService::BuildAsset(scene->GetID(), false, true))
    {
        diagnostic = AssetPipelineService::GetBuildDiagnostic(scene->GetID());
        LOG(Error, "Cannot publish saved scene artifact '{0}': {1}", path, diagnostic.Message);
        return true;
    }
    return false;
}

bool ReloadSavedSceneAsset(const Guid& sceneId, const StringView& path)
{
    Asset* asset = Content::GetRuntimeObject(sceneId);
    if (!asset)
        asset = Content::GetAsset(path);
    if (!asset)
        return false;
    asset->Reload();
    if (asset->WaitForLoaded())
    {
        LOG(Error, "Cannot reload published scene artifact '{0}'.", path);
        return true;
    }
    return false;
}
#endif

bool LevelImpl::saveScene(Scene* scene, const String& path)
{
    PROFILE_CPU_NAMED("Level.SaveScene");
    PROFILE_MEM(Level);
    ASSERT(scene && EnumHasNoneFlags(scene->Flags, ObjectFlags::WasMarkedToDelete));
    auto sceneId = scene->GetID();

    LOG(Info, "Saving scene {0} to \'{1}\'", scene->GetName(), path);
    Stopwatch stopwatch;

#if USE_EDITOR
    SceneSourceRevision expectedSource;
    String fragmentError;
    if (SceneFragmentStore::CaptureSourceRevision(path, expectedSource, fragmentError))
    {
        LOG(Error, "Cannot capture scene source revision for '{0}': {1}", path, fragmentError);
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, sceneId);
        return true;
    }
    SceneFragmentSavePlan fragmentPlan;
#endif

    // Serialize to json
    rapidjson_flax::StringBuffer buffer;
    if (saveScene(scene, buffer, true, true,
#if USE_EDITOR
        &fragmentPlan
#else
        nullptr
#endif
        ))
    {
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, sceneId);
        return true;
    }

#if USE_EDITOR
    if (SceneFragmentStore::CommitSceneSave(path, buffer.GetString(), static_cast<int32>(buffer.GetSize()),
        expectedSource, fragmentPlan, fragmentError))
    {
        LOG(Error, "Cannot commit scene and private fragments for '{0}': {1}", path, fragmentError);
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, sceneId);
        return true;
    }
    if (PublishSavedSceneSource(scene, path))
    {
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, sceneId);
        return true;
    }
#else
    if (File::WriteAllBytes(path, (byte*)buffer.GetString(), (int32)buffer.GetSize()))
        return true;
#endif

    stopwatch.Stop();
    LOG(Info, "Scene saved! Time {0}ms", stopwatch.GetMilliseconds());

#if USE_EDITOR
    if (ReloadSavedSceneAsset(sceneId, path))
    {
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, sceneId);
        return true;
    }
#endif

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneSaved, scene, sceneId);

    return false;
}

bool LevelImpl::saveScenes(const Array<Scene*>& scenes)
{
#if USE_EDITOR
    PROFILE_CPU_NAMED("Level.SaveScenes");
    if (scenes.IsEmpty())
        return false;

    const auto failBatch = [&scenes](const StringView& error)
    {
        LOG(Error, "Cannot save scene batch: {0}", error);
        for (Scene* scene : scenes)
        {
            if (scene)
                CallSceneEvent(SceneEventType::OnSceneSaveError, scene, scene->GetID());
        }
        return true;
    };

    Array<PreparedSceneSave> saves;
    saves.Resize(scenes.Count());
    HashSet<Guid> sceneIds;
    String error;
    for (int32 i = 0; i < scenes.Count(); i++)
    {
        Scene* scene = scenes[i];
        if (!scene || EnumHasAnyFlags(scene->Flags, ObjectFlags::WasMarkedToDelete) ||
            !sceneIds.Add(scene->GetID()) || scene->GetPath().IsEmpty())
        {
            return failBatch(TEXT("The scene list contains a missing, deleted, duplicate, or unsaved scene."));
        }
        PreparedSceneSave& save = saves[i];
        save.SourcePath = scene->GetPath();
        if (SceneFragmentStore::CaptureSourceRevision(save.SourcePath, save.ExpectedSource, error))
            return failBatch(error);
    }

    Stopwatch stopwatch;
    for (int32 i = 0; i < scenes.Count(); i++)
    {
        Scene* scene = scenes[i];
        LOG(Info, "Preparing scene {0} for atomic batch save to '{1}'", scene->GetName(), saves[i].SourcePath);
        rapidjson_flax::StringBuffer buffer;
        if (saveScene(scene, buffer, true, true, &saves[i].FragmentPlan))
            return failBatch(TEXT("A scene could not be serialized."));
        saves[i].SourceData.Set(reinterpret_cast<const byte*>(buffer.GetString()), static_cast<int32>(buffer.GetSize()));
    }

    if (SceneFragmentStore::CommitSceneSaves(saves, error))
        return failBatch(error);
    for (int32 i = 0; i < scenes.Count(); i++)
    {
        if (PublishSavedSceneSource(scenes[i], saves[i].SourcePath))
            return failBatch(TEXT("A saved scene artifact could not be published."));
    }

    stopwatch.Stop();
    LOG(Info, "Saved {0} scenes atomically. Time {1}ms", scenes.Count(), stopwatch.GetMilliseconds());
    for (Scene* scene : scenes)
    {
        if (ReloadSavedSceneAsset(scene->GetID(), scene->GetPath()))
            return failBatch(TEXT("A published scene artifact could not be reloaded."));
    }
    for (Scene* scene : scenes)
        CallSceneEvent(SceneEventType::OnSceneSaved, scene, scene->GetID());
    return false;
#else
    LOG(Error, "Cannot save scene batches to cooked content.");
    return true;
#endif
}

bool LevelImpl::saveScene(Scene* scene, rapidjson_flax::StringBuffer& outBuffer, bool prettyJson,
    bool useExternalActorsStorage, SceneFragmentSavePlan* fragmentPlan)
{
    PROFILE_CPU_NAMED("Level.SaveScene");
    PROFILE_MEM(Level);
    if (prettyJson)
    {
        PrettyJsonWriter writerObj(outBuffer);
        return saveScene(scene, outBuffer, writerObj, true, useExternalActorsStorage, fragmentPlan);
    }
    else
    {
        CompactJsonWriter writerObj(outBuffer);
        return saveScene(scene, outBuffer, writerObj, false, useExternalActorsStorage, fragmentPlan);
    }
}

bool LevelImpl::saveScene(Scene* scene, rapidjson_flax::StringBuffer& outBuffer, JsonWriter& writer,
    bool prettyJson, bool useExternalActorsStorage, SceneFragmentSavePlan* fragmentPlan)
{
    ASSERT(scene);
    const auto sceneId = scene->GetID();

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneSaving, scene, sceneId);

#if USE_EDITOR
    if (useExternalActorsStorage && scene->UseExternalActors)
    {
        const String fragmentPath = SceneFragmentStore::GetScenePath(sceneId);
        if ((FileSystem::DirectoryExists(fragmentPath) || FileSystem::FileExists(SceneFragmentStore::GetIndexPath(sceneId))) &&
            ValidateExternalActorFragments(sceneId))
        {
            return true;
        }
    }
    if (scene->UseExternalActors)
        EnsureExternalSiblingOrderKeys(scene);

    if (useExternalActorsStorage && scene->UseExternalActors)
    {
        if (!fragmentPlan)
        {
            LOG(Error, "External actor scene serialization requires a fragment save plan.");
            return true;
        }
        Array<SceneObject*> allObjects;
        SceneQuery::GetAllSerializableSceneObjects(scene, allObjects);

        HashSet<SceneObject*> serializableObjects;
        for (SceneObject* object : allObjects)
        {
            object->SetPersistentDocumentIdentity(AssetGuid(sceneId), object->GetLocalFileId());
            serializableObjects.Add(object);
        }

        Array<SceneFragmentWrite> fragments;
        Array<Actor*> actors;
        SceneQuery::GetAllActors(scene, actors);
        for (Actor* actor : actors)
        {
            if (!serializableObjects.Contains(actor))
                continue;
            SceneFragmentWrite fragment;
            if (BuildExternalActorFragment(actor, serializableObjects, fragment))
                return true;
            fragments.Add(MoveTemp(fragment));
        }
        String fragmentError;
        if (SceneFragmentStore::PrepareSave(sceneId, fragments, *fragmentPlan, fragmentError))
        {
            LOG(Error, "Cannot prepare private scene fragments for scene '{0}': {1}", sceneId, fragmentError);
            return true;
        }

        writer.StartObject();
        {
            PROFILE_CPU_NAMED("Serialize");

            // Json resource header
            writer.JKEY("ID");
            writer.Guid(sceneId);
            writer.JKEY("TypeName");
            writer.String("FlaxEngine.SceneAsset", ARRAY_COUNT("FlaxEngine.SceneAsset") - 1);
            writer.JKEY("EngineBuild");
            writer.Int(FLAXENGINE_VERSION_BUILD);
            writer.JKEY("ExternalActors");
            writer.Bool(true);
            // Json resource data
            writer.JKEY("Data");
            writer.StartArray();
            WriteSceneObject(writer, scene, false);
            int32 count = 1;
            for (Script* script : scene->Scripts)
            {
                if (serializableObjects.Contains(script))
                {
                    WriteSceneObject(writer, script, true);
                    count++;
                }
            }
            writer.EndArray(count);
        }
        writer.EndObject();
        String error;
        if (ScenePrefabDocument::RuntimeEnvelopeToSource(outBuffer, true, prettyJson, error))
        {
            LOG(Error, "Cannot create authored scene source: {0}", error);
            return true;
        }
        return false;
    }
    if (useExternalActorsStorage)
    {
        if (!fragmentPlan)
        {
            LOG(Error, "Scene serialization requires a fragment save plan.");
            return true;
        }
        const String fragmentPath = SceneFragmentStore::GetScenePath(sceneId);
        if ((FileSystem::DirectoryExists(fragmentPath) || FileSystem::FileExists(SceneFragmentStore::GetIndexPath(sceneId))) &&
            ValidateExternalActorFragments(sceneId))
        {
            return true;
        }
        String fragmentError;
        if (SceneFragmentStore::PrepareDelete(sceneId, *fragmentPlan, fragmentError))
        {
            LOG(Error, "Cannot prepare private scene fragment removal for scene '{0}': {1}", sceneId, fragmentError);
            return true;
        }
    }
#endif

    // Get all objects in the scene
    Array<SceneObject*> allObjects;
    SceneQuery::GetAllSerializableSceneObjects(scene, allObjects);

    // Serialize to json
    writer.StartObject();
    {
        PROFILE_CPU_NAMED("Serialize");

        // Json resource header
        writer.JKEY("ID");
        writer.Guid(sceneId);
        writer.JKEY("TypeName");
        writer.String("FlaxEngine.SceneAsset", ARRAY_COUNT("FlaxEngine.SceneAsset") - 1);
        writer.JKEY("EngineBuild");
        writer.Int(FLAXENGINE_VERSION_BUILD);

        // Json resource data
        writer.JKEY("Data");
        writer.StartArray();
        SceneObject** objects = allObjects.Get();
        for (int32 i = 0; i < allObjects.Count(); i++)
        {
#if USE_EDITOR
            if (scene->UseExternalActors)
                WriteSceneObject(writer, objects[i], true);
            else
#endif
                writer.SceneObject(objects[i]);
        }
        writer.EndArray();
    }
    writer.EndObject();
    String error;
    if (ScenePrefabDocument::RuntimeEnvelopeToSource(outBuffer, true, prettyJson, error))
    {
        LOG(Error, "Cannot create authored scene source: {0}", error);
        return true;
    }
    return false;
}

bool Level::SaveScene(Scene* scene, bool prettyJson)
{
    ScopeLock lock(_sceneActionsLocker);
    SceneAction::Context context;
    return SaveSceneAction(scene, prettyJson).Do(context) != SceneResult::Success;
}

bool Level::SaveSceneToBytes(Scene* scene, rapidjson_flax::StringBuffer& outData, bool prettyJson)
{
    ASSERT(scene);
    ScopeLock lock(_sceneActionsLocker);
    Stopwatch stopwatch;
    LOG(Info, "Saving scene {0} to bytes", scene->GetName());

    // Serialize to json
    if (saveScene(scene, outData, prettyJson, false))
    {
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, scene->GetID());
        return true;
    }

    stopwatch.Stop();
    LOG(Info, "Scene saved! Time {0}ms", stopwatch.GetMilliseconds());

    // Fire event
    CallSceneEvent(SceneEventType::OnSceneSaved, scene, scene->GetID());

    return false;
}

Array<byte> Level::SaveSceneToBytes(Scene* scene, bool prettyJson)
{
    Array<byte> data;
    rapidjson_flax::StringBuffer sceneData;
    if (!SaveSceneToBytes(scene, sceneData, prettyJson))
    {
        data.Set((const byte*)sceneData.GetString(), (int32)sceneData.GetSize() * sizeof(rapidjson_flax::StringBuffer::Ch));
    }
    return data;
}

void Level::SaveSceneAsync(Scene* scene)
{
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<SaveSceneAction>(scene));
}

bool Level::SaveScenes(const Array<Scene*>& scenes)
{
    ScopeLock lock(_sceneActionsLocker);
    SceneAction::Context context;
    return SaveScenesAction(scenes).Do(context) != SceneResult::Success;
}

void Level::SaveScenesAsync(const Array<Scene*>& scenes)
{
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<SaveScenesAction>(scenes));
}

bool Level::SaveAllScenes()
{
    ScopeLock lock(_sceneActionsLocker);
    SceneAction::Context context;
    return SaveScenesAction(Scenes).Do(context) != SceneResult::Success;
}

void Level::SaveAllScenesAsync()
{
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<SaveScenesAction>(Scenes));
}

bool Level::LoadScene(const Guid& id)
{
    // Check ID
    if (!id.IsValid())
    {
        Log::ArgumentException();
        return true;
    }

    // Skip is that scene is already loaded
    if (FindScene(id) != nullptr)
    {
        LOG(Info, "Scene {0} is already loaded.", id);
        return false;
    }

    // Now to deserialize scene in a proper way we need to load scripting
    if (!Scripting::IsEveryAssemblyLoaded())
    {
#if USE_EDITOR
        LOG(Error, "Scripts must be compiled without any errors in order to load a scene. Please fix it.");
#else
        LOG(Warning, "Scripts must be compiled without any errors in order to load a scene.");
#endif
        return true;
    }

    // Preload scene asset
    const auto sceneAsset = Content::LoadAssetAsync<JsonAsset>(id);
    if (sceneAsset == nullptr)
    {
        LOG(Error, "Cannot load scene asset.");
        return true;
    }

    // Load scene
    ScopeLock lock(ScenesLock);
    SceneLoader loader;
    if (loadScene(loader, sceneAsset) != SceneResult::Success)
    {
        LOG(Error, "Failed to deserialize scene {0}", id);
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, id);
        return true;
    }
    return false;
}

Scene* Level::LoadSceneFromBytes(const BytesContainer& data)
{
    Scene* scene = nullptr;
    SceneLoader loader;
    if (loadScene(loader, data, &scene) != SceneResult::Success)
    {
        LOG(Error, "Failed to deserialize scene from bytes");
        CallSceneEvent(SceneEventType::OnSceneLoadError, nullptr, Guid::Empty);
    }
    return scene;
}

bool Level::LoadSceneAsync(const Guid& id)
{
    if (!id.IsValid())
    {
        Log::ArgumentException();
        return true;
    }

    // Preload scene asset
    const auto sceneAsset = Content::LoadAssetAsync<JsonAsset>(id);
    if (sceneAsset == nullptr)
    {
        LOG(Error, "Cannot load scene asset.");
        return true;
    }

    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<LoadSceneAction>(id, sceneAsset, true));

    return false;
}

bool Level::UnloadScene(Scene* scene)
{
    return unloadScene(scene);
}

void Level::UnloadSceneAsync(Scene* scene)
{
    CHECK(scene);
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<UnloadSceneAction>(scene));
}

bool Level::UnloadAllScenes()
{
    ScopeLock lock(_sceneActionsLocker);
    return unloadScenes();
}

void Level::UnloadAllScenesAsync()
{
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<UnloadScenesAction>());
}

#if USE_EDITOR

bool Level::IsExternalActorsSceneAsset(const JsonAssetBase* sceneAsset)
{
    if (!sceneAsset || !sceneAsset->Document.IsObject())
        return false;

    const auto externalActors = sceneAsset->Document.FindMember(ExternalActorsKey);
    return externalActors != sceneAsset->Document.MemberEnd() && externalActors->value.IsBool() && externalActors->value.GetBool();
}

bool Level::SaveSceneAssetToBytes(JsonAssetBase* sceneAsset, rapidjson_flax::StringBuffer& outData, Array<String>* externalActorFiles, bool prettyJson)
{
    if (!sceneAsset)
    {
        Log::ArgumentNullException();
        return true;
    }

    if (IsExternalActorsSceneAsset(sceneAsset))
        return SaveExternalSceneAssetToBytes(sceneAsset, outData, externalActorFiles, prettyJson);

    if (prettyJson)
    {
        PrettyJsonWriter writerObj(outData);
        return sceneAsset->Save(writerObj);
    }

    CompactJsonWriter writerObj(outData);
    return sceneAsset->Save(writerObj);
}

bool Level::ConvertSceneToExternalActors(Scene* scene)
{
    if (!scene)
    {
        Log::ArgumentNullException();
        return true;
    }
    if (Editor::IsPlayMode)
    {
        LOG(Error, "Cannot convert scene storage while in play mode.");
        return true;
    }

    const String path = scene->GetPath();
    if (path.IsEmpty() || !FileSystem::FileExists(path))
    {
        LOG(Error, "Missing scene path.");
        return true;
    }

    ScopeLock lock(_sceneActionsLocker);
    String validationError;
    if (ValidateSceneStorageTransitionSource(path, scene->UseExternalActors, validationError))
    {
        LOG(Error, "Cannot change actor storage for scene '{0}': {1}", scene->GetID(), validationError);
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, scene->GetID());
        return true;
    }

    if (scene->UseExternalActors)
        return saveScene(scene);

    scene->UseExternalActors = true;
    if (saveScene(scene))
    {
        scene->UseExternalActors = false;
        return true;
    }
    return false;
}

bool Level::ConvertSceneToInternalActors(Scene* scene)
{
    if (!scene)
    {
        Log::ArgumentNullException();
        return true;
    }
    if (Editor::IsPlayMode)
    {
        LOG(Error, "Cannot convert scene storage while in play mode.");
        return true;
    }

    const String path = scene->GetPath();
    if (path.IsEmpty() || !FileSystem::FileExists(path))
    {
        LOG(Error, "Missing scene path.");
        return true;
    }

    ScopeLock lock(_sceneActionsLocker);
    String validationError;
    if (ValidateSceneStorageTransitionSource(path, scene->UseExternalActors, validationError))
    {
        LOG(Error, "Cannot change actor storage for scene '{0}': {1}", scene->GetID(), validationError);
        CallSceneEvent(SceneEventType::OnSceneSaveError, scene, scene->GetID());
        return true;
    }

    if (scene->UseExternalActors)
    {
        scene->UseExternalActors = false;
        if (saveScene(scene))
        {
            scene->UseExternalActors = true;
            return true;
        }
    }

    return false;
}

bool Level::ApplyExternalActorsSiblingKeys(Scene* scene)
{
    if (!scene)
    {
        Log::ArgumentNullException();
        return true;
    }
    if (Editor::IsPlayMode)
    {
        LOG(Error, "Cannot apply sibling keys while in play mode.");
        return true;
    }
    if (!scene->UseExternalActors)
    {
        LOG(Error, "Sibling keys can only be applied to an external actors scene.");
        return true;
    }

    const String path = scene->GetPath();
    if (path.IsEmpty() || !FileSystem::FileExists(path))
    {
        LOG(Error, "Missing scene path.");
        return true;
    }

    EnsureExternalSiblingOrderKeys(scene);
    Array<SceneObject*> objects;
    SceneQuery::GetAllSerializableSceneObjects(scene, objects);
    int32 migratedObjects = 0;
    for (SceneObject* object : objects)
    {
        if (object->HasParent() && object->HasExternalLegacyOrderInParent())
        {
            object->SetExternalSiblingOrderKey(object->GetExternalSiblingOrderKey());
            migratedObjects++;
        }
    }
    if (saveScene(scene))
        return true;

    LOG(Info, "Applied sibling keys to {0} scene objects in '{1}'.", migratedObjects, scene->GetName());
    return false;
}

void Level::ReloadScriptsAsync()
{
    ScopeLock lock(_sceneActionsLocker);
    _sceneActions.Enqueue(New<ReloadScriptsAction>());
}

#endif

Actor* Level::FindActor(const Guid& id)
{
    return Scripting::TryFindObject<Actor>(id);
}

Actor* Level::FindActor(const StringView& name)
{
    Actor* result = nullptr;
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; result == nullptr && i < Scenes.Count(); i++)
        result = Scenes[i]->FindActor(name);
    return result;
}

Actor* Level::FindActor(const MClass* type, bool activeOnly)
{
    CHECK_RETURN(type, nullptr);
    Actor* result = nullptr;
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; result == nullptr && i < Scenes.Count(); i++)
        result = Scenes[i]->FindActor(type, activeOnly);
    return result;
}

Actor* Level::FindActor(const MClass* type, const StringView& name)
{
    CHECK_RETURN(type, nullptr);
    Actor* result = nullptr;
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; result == nullptr && i < Scenes.Count(); i++)
        result = Scenes[i]->FindActor(type, name);
    return result;
}

Actor* FindActorRecursive(Actor* node, const Tag& tag, bool activeOnly)
{
    if (activeOnly && !node->GetIsActive())
        return nullptr;
    if (node->HasTag(tag))
        return node;
    Actor* result = nullptr;
    for (Actor* child : node->Children)
    {
        result = FindActorRecursive(child, tag, activeOnly);
        if (result)
            break;
    }
    return result;
}

Actor* FindActorRecursiveByType(Actor* node, const MClass* type, const Tag& tag, bool activeOnly)
{
    CHECK_RETURN(type, nullptr);
    if (activeOnly && !node->GetIsActive())
        return nullptr;
    if (node->HasTag(tag) && (node->GetClass()->IsSubClassOf(type) || node->GetClass()->HasInterface(type)))
        return node;
    Actor* result = nullptr;
    for (Actor* child : node->Children)
    {
        result = FindActorRecursiveByType(child, type, tag, activeOnly);
        if (result)
            break;
    }
    return result;
}

void FindActorsRecursive(Actor* node, const Tag& tag, const bool activeOnly, Array<Actor*>& result)
{
    if (activeOnly && !node->GetIsActive())
        return;
    if (node->HasTag(tag))
        result.Add(node);
    for (Actor* child : node->Children)
        FindActorsRecursive(child, tag, activeOnly, result);
}

void FindActorsRecursiveByParentTags(Actor* node, const Array<Tag>& tags, const bool activeOnly, Array<Actor*>& result)
{
    if (activeOnly && !node->GetIsActive())
        return;
    for (Tag tag : tags)
    {
        if (node->HasTag(tag))
        {
            result.Add(node);
            break;
        }
    }
    for (Actor* child : node->Children)
        FindActorsRecursiveByParentTags(child, tags, activeOnly, result);
}

Actor* Level::FindActor(const Tag& tag, bool activeOnly, Actor* root)
{
    PROFILE_CPU();
    if (root)
        return FindActorRecursive(root, tag, activeOnly);
    Actor* result = nullptr;
    for (Scene* scene : Scenes)
    {
        result = FindActorRecursive(scene, tag, activeOnly);
        if (result)
            break;
    }
    return result;
}

Actor* Level::FindActor(const MClass* type, const Tag& tag, bool activeOnly, Actor* root)
{
    CHECK_RETURN(type, nullptr);
    if (root)
        return FindActorRecursiveByType(root, type, tag, activeOnly);
    Actor* result = nullptr;
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; result == nullptr && i < Scenes.Count(); i++)
        result = Scenes[i]->FindActor(type, tag, activeOnly);
    return result;
}

void FindActorRecursive(Actor* node, const Tag& tag, Array<Actor*>& result)
{
    if (node->HasTag(tag))
        result.Add(node);
    for (Actor* child : node->Children)
        FindActorRecursive(child, tag, result);
}

Array<Actor*> Level::FindActors(const Tag& tag, const bool activeOnly, Actor* root)
{
    PROFILE_CPU();
    Array<Actor*> result;
    if (root)
    {
        FindActorsRecursive(root, tag, activeOnly, result);
    }
    else
    {
        ScopeLock lock(ScenesLock);
        for (Scene* scene : Scenes)
            FindActorsRecursive(scene, tag, activeOnly, result);
    }
    return result;
}

Array<Actor*> Level::FindActorsByParentTag(const Tag& parentTag, const bool activeOnly, Actor* root)
{
    PROFILE_CPU();
    Array<Actor*> result;
    const Array<Tag> subTags = Tags::GetSubTags(parentTag);

    if (subTags.Count() == 0)
    {
        return result;
    }
    if (subTags.Count() == 1)
    {
        result = FindActors(subTags[0], activeOnly, root);
        return result;
    }

    if (root)
    {
        FindActorsRecursiveByParentTags(root, subTags, activeOnly, result);
    }
    else
    {
        ScopeLock lock(ScenesLock);
        for (Scene* scene : Scenes)
            FindActorsRecursiveByParentTags(scene, subTags, activeOnly, result);
    }

    return result;
}

Script* Level::FindScript(const MClass* type)
{
    CHECK_RETURN(type, nullptr);
    Script* result = nullptr;
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; result == nullptr && i < Scenes.Count(); i++)
        result = Scenes[i]->FindScript(type);
    return result;
}

namespace
{
    void GetActors(const MClass* type, bool isInterface, Actor* actor, bool activeOnly, Array<Actor*>& result)
    {
        if (activeOnly && !actor->GetIsActive())
            return;
        if ((!isInterface && actor->GetClass()->IsSubClassOf(type)) ||
            (isInterface && actor->GetClass()->HasInterface(type)))
            result.Add(actor);
        for (auto child : actor->Children)
            GetActors(type, isInterface, child, activeOnly, result);
    }

    void GetScripts(const MClass* type, bool isInterface, Actor* actor, Array<Script*>& result)
    {
        for (auto script : actor->Scripts)
        {
            if ((!isInterface && script->GetClass()->IsSubClassOf(type)) ||
                (isInterface && script->GetClass()->HasInterface(type)))
                result.Add(script);
        }
        for (auto child : actor->Children)
            GetScripts(type, isInterface, child, result);
    }
}

Array<Actor*> Level::GetActors(const MClass* type, bool activeOnly)
{
    Array<Actor*> result;
    CHECK_RETURN(type, result);
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; i < Scenes.Count(); i++)
        ::GetActors(type, type->IsInterface(), Scenes[i], activeOnly, result);
    return result;
}

Array<Script*> Level::GetScripts(const MClass* type, Actor* root)
{
    Array<Script*> result;
    CHECK_RETURN(type, result);
    ScopeLock lock(ScenesLock);
    const bool isInterface = type->IsInterface();
    if (root)
        ::GetScripts(type, isInterface, root, result);
    else
        for (int32 i = 0; i < Scenes.Count(); i++)
            ::GetScripts(type, isInterface, Scenes[i], result);
    return result;
}

Scene* Level::FindScene(const Guid& id)
{
    ScopeLock lock(ScenesLock);
    for (int32 i = 0; i < Scenes.Count(); i++)
        if (Scenes[i]->GetID() == id)
            return Scenes[i];
    return nullptr;
}

void Level::GetScenes(Array<Scene*>& scenes)
{
    ScopeLock lock(ScenesLock);

    scenes = Scenes;
}

void Level::GetScenes(Array<Actor*>& scenes)
{
    ScopeLock lock(ScenesLock);

    scenes.Clear();
    scenes.EnsureCapacity(Scenes.Count());

    for (int32 i = 0; i < Scenes.Count(); i++)
        scenes.Add(Scenes[i]);
}

void Level::GetScenes(Array<Guid>& scenes)
{
    ScopeLock lock(ScenesLock);

    scenes.Clear();
    scenes.EnsureCapacity(Scenes.Count());

    for (int32 i = 0; i < Scenes.Count(); i++)
        scenes.Add(Scenes[i]->GetID());
}

void FillTree(Actor* node, Array<Actor*>& result)
{
    result.Add(node->Children);
    for (int i = 0; i < node->Children.Count(); i++)
    {
        FillTree(node->Children[i], result);
    }
}

void Level::ConstructSolidActorsTreeList(const Array<Actor*>& input, Array<Actor*>& output)
{
    for (int32 i = 0; i < input.Count(); i++)
    {
        auto target = input[i];

        // Check if has been already added
        if (output.Contains(target))
            continue;

        // Add whole child tree to the results
        output.Add(target);
        FillTree(target, output);
    }
}

void Level::ConstructParentActorsTreeList(const Array<Actor*>& input, Array<Actor*>& output)
{
    // Build solid part of the tree
    Array<Actor*> fullTree;
    ConstructSolidActorsTreeList(input, fullTree);

    for (int32 i = 0; i < input.Count(); i++)
    {
        Actor* target = input[i];

        // If there is no target node parent in the solid tree list,
        // then it means it's a local root node and can be added to the results.
        if (!fullTree.Contains(target->GetParent()))
            output.Add(target);
    }
}
