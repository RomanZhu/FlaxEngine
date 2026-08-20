// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGBuilder.h"
#include "CSGMesh.h"
#include "CSGData.h"
#include "CSGCompilation.h"
#include "Engine/Level/Level.h"
#include "Engine/Level/SceneQuery.h"
#include "Engine/Level/Actor.h"
#include "Engine/Level/Actors/CSGModel.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Types/TimeSpan.h"
#include "Engine/Graphics/Models/ModelData.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/ContentImporters/AssetsImportingManager.h"
#include "Engine/ContentImporters/CreateCollisionData.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#if USE_EDITOR
#include "Editor/Editor.h"
#endif

#if COMPILE_WITH_CSG_BUILDER

using namespace CSG;

// Enable/disable locking scene during building CSG brushes nodes
#define CSG_USE_SCENE_LOCKS 0

struct BuildData;

namespace CSGBuilderImpl
{
    Array<Scene*> ScenesToRebuild;
    struct ModelRebuildEntry
    {
        CSGModel* Model;
        DateTime BuildTime;
    };
    Array<ModelRebuildEntry> ModelsToRebuild;

    void onSceneUnloading(Scene* scene, const Guid& sceneId);
    bool buildInner(Scene* scene, BuildData& data);
    bool buildInner(CSGModel* model, BuildData& data);
    void build(Scene* scene);
    void build(CSGModel* model);
    bool updatePreviewModel(CSGCompiledData& csgData, const ModelData& modelData);
    bool generateRawDataAsset(RawData& meshData, Guid& assetId, const String& assetPath);
}

using namespace CSGBuilderImpl;

class CSGBuilderService : public EngineService
{
public:
    CSGBuilderService()
        : EngineService(TEXT("CSG Builder"), 90)
    {
    }

    bool Init() override;
    void Update() override;
};

CSGBuilderService CSGBuilderServiceInstance;

Delegate<Brush*> Builder::OnBrushModified;

void CSGBuilderImpl::onSceneUnloading(Scene* scene, const Guid& sceneId)
{
    // Ensure to remove scene (prevent crashes)
    ScenesToRebuild.Remove(scene);
    for (int32 i = ModelsToRebuild.Count() - 1; i >= 0; i--)
    {
        if (ModelsToRebuild[i].Model && ModelsToRebuild[i].Model->GetScene() == scene)
        {
            ModelsToRebuild.RemoveAt(i);
        }
    }
}

bool CSGBuilderService::Init()
{
    Level::SceneUnloading.Bind(onSceneUnloading);

    return false;
}

void CSGBuilderService::Update()
{
    // Check if build is pending
    if (ScenesToRebuild.HasItems() && Engine::IsReady())
    {
        auto now = DateTime::NowUTC();

        for (int32 i = 0; ScenesToRebuild.HasItems() && i < ScenesToRebuild.Count(); i++)
        {
            auto scene = ScenesToRebuild[i];
            if (now - scene->CSGData.BuildTime >= 0)
            {
                scene->CSGData.BuildTime.Ticks = 0;
                ScenesToRebuild.RemoveAt(i--);
                build(scene);
            }
        }
    }

    if (ModelsToRebuild.HasItems() && Engine::IsReady())
    {
        auto now = DateTime::NowUTC();

        for (int32 i = 0; ModelsToRebuild.HasItems() && i < ModelsToRebuild.Count(); i++)
        {
            if (now - ModelsToRebuild[i].BuildTime >= 0)
            {
                auto model = ModelsToRebuild[i].Model;
                ModelsToRebuild.RemoveAt(i--);
                if (model)
                    build(model);
            }
        }
    }
}

bool Builder::IsActive()
{
    return ScenesToRebuild.HasItems() || ModelsToRebuild.HasItems();
}

void Builder::Build(Scene* scene, float timeoutMs)
{
    if (scene == nullptr)
        return;

#if USE_EDITOR
    // Disable building during play mode
    if (Editor::IsPlayMode)
        return;
#endif

    // Register building
    if (!ScenesToRebuild.Contains(scene))
    {
        ScenesToRebuild.Add(scene);
    }
    scene->CSGData.BuildTime = DateTime::NowUTC() + TimeSpan::FromMilliseconds(timeoutMs);
}

void Builder::Build(CSGModel* model, float timeoutMs)
{
    if (model == nullptr)
        return;

#if USE_EDITOR
    // Disable building during play mode
    if (Editor::IsPlayMode)
        return;
#endif

    const DateTime targetTime = DateTime::NowUTC() + TimeSpan::FromMilliseconds(timeoutMs);
    for (int32 i = 0; i < ModelsToRebuild.Count(); i++)
    {
        if (ModelsToRebuild[i].Model == model)
        {
            ModelsToRebuild[i].BuildTime = targetTime;
            return;
        }
    }
    ModelsToRebuild.Add({ model, targetTime });
}

namespace CSG
{
}

struct BuildData
{
    int32 brushesCount = 0;
    Guid outputModelAssetId = Guid::Empty;
    Guid outputRawDataAssetId = Guid::Empty;
    Guid outputCollisionDataAssetId = Guid::Empty;

    BuildData() = default;
};

bool CSGBuilderImpl::updatePreviewModel(CSGCompiledData& csgData, const ModelData& modelData)
{
    // Render lists store raw mesh buffer pointers. Exclude rendering while the
    // inactive preview is rebuilt and published, then keep the old preview alive
    // as the next cache entry. This is particularly important during the native
    // drag-and-drop loop, which renders frames from a worker thread.
    GPUDeviceLock gpuLock(GPUDevice::Instance);
    Array<int32, FixedAllocation<MODEL_MAX_LODS>> meshesCountPerLod;
    meshesCountPerLod.Resize(modelData.LODs.Count());
    for (int32 lodIndex = 0; lodIndex < modelData.LODs.Count(); lodIndex++)
        meshesCountPerLod[lodIndex] = modelData.LODs[lodIndex].Meshes.Count();

    // Reinitializing a virtual model with a different mesh topology can relocate
    // live Mesh objects and invalidate their GPU-resource ownership. Material
    // changes commonly alter the number of CSG mesh partitions, so only reuse the
    // inactive preview when its LOD and mesh counts already match exactly.
    AssetReference<Model> previewModel = csgData.PreviewModelCache;
    bool canReusePreview = previewModel && previewModel->LODs.Count() == meshesCountPerLod.Count();
    for (int32 lodIndex = 0; canReusePreview && lodIndex < meshesCountPerLod.Count(); lodIndex++)
        canReusePreview = previewModel->LODs[lodIndex].Meshes.Count() == meshesCountPerLod[lodIndex];
    if (!canReusePreview)
    {
        previewModel = Content::CreateVirtualAsset<Model>();
        if (!previewModel || previewModel->SetupLODs(Span<int32>(meshesCountPerLod.Get(), meshesCountPerLod.Count())))
            return true;
    }

    previewModel->MinScreenSize = modelData.MinScreenSize;
    previewModel->SetupMaterialSlots(Math::Max(modelData.Materials.Count(), 1));
    for (int32 slotIndex = 0; slotIndex < modelData.Materials.Count(); slotIndex++)
    {
        const auto& sourceSlot = modelData.Materials[slotIndex];
        auto& destinationSlot = previewModel->MaterialSlots[slotIndex];
        destinationSlot.Name = sourceSlot.Name;
        destinationSlot.ShadowsMode = sourceSlot.ShadowsMode;
        destinationSlot.Material = Content::LoadAsync<MaterialBase>(sourceSlot.AssetID);
    }

    for (int32 lodIndex = 0; lodIndex < modelData.LODs.Count(); lodIndex++)
    {
        const auto& sourceLod = modelData.LODs[lodIndex];
        auto& destinationLod = previewModel->LODs[lodIndex];
        destinationLod.ScreenSize = sourceLod.ScreenSize;
        for (int32 meshIndex = 0; meshIndex < sourceLod.Meshes.Count(); meshIndex++)
        {
            const auto* sourceMesh = sourceLod.Meshes[meshIndex];
            if (!sourceMesh || sourceMesh->Positions.IsEmpty() || sourceMesh->Indices.IsEmpty() || sourceMesh->Indices.Count() % 3 != 0)
                return true;

            const int32 vertexCount = sourceMesh->Positions.Count();
            const Float3* normals = sourceMesh->Normals.Count() == vertexCount ? sourceMesh->Normals.Get() : nullptr;
            const Float3* tangents = sourceMesh->Tangents.Count() == vertexCount ? sourceMesh->Tangents.Get() : nullptr;
            const Float2* uvs = sourceMesh->UVs.HasItems() && sourceMesh->UVs[0].Count() == vertexCount ? sourceMesh->UVs[0].Get() : nullptr;
            auto& destinationMesh = destinationLod.Meshes[meshIndex];
            destinationMesh.SetMaterialSlotIndex(sourceMesh->MaterialSlotIndex);
            if (destinationMesh.UpdateMesh(vertexCount, sourceMesh->Indices.Count() / 3, sourceMesh->Positions.Get(), sourceMesh->Indices.Get(), normals, tangents, uvs))
                return true;
        }
    }

    // Publish only after the entire model is ready. The render lock guarantees no
    // draw list still contains pointers to the model moving into the cache.
    csgData.PreviewModelCache = csgData.PreviewModel;
    csgData.PreviewModel = previewModel;

    return false;
}

bool CSGBuilderImpl::buildInner(Scene* scene, BuildData& data)
{
    // Compile explicit stacks and implicit brushes under the scene target
    CSG::Mesh combinedMesh;
    if (!CSGCompilation::CompileTargetMeshes(scene, combinedMesh))
        return false;
    if (combinedMesh.GetPolygons()->IsEmpty())
        return false;

    // TODO: split too big meshes (too many verts, to far parts, etc.)

    // Triangulate meshes
    {
        // Convert CSG meshes into raw triangles data
        RawData meshData;
        Array<MeshVertex> vertexBuffer;
        combinedMesh.Triangulate(meshData, vertexBuffer);
        meshData.RemoveEmptySlots();
        if (meshData.Slots.HasItems())
        {
            const auto sceneDataFolderPath = scene->GetDataFolderPath();

            // Convert CSG mesh data to common storage type
            ModelData modelData;
            meshData.ToModelData(modelData);

            // Convert CSG mesh to the local transformation of the scene
            if (!scene->GetTransform().IsIdentity())
            {
                Matrix t;
                scene->GetWorldToLocalMatrix(t);
                modelData.TransformBuffer(t);
            }

            // Keep the viewport on a memory-backed model while the persisted model asset
            // is rewritten. This mirrors RealtimeCSG's dynamic-mesh update path and avoids
            // exposing the asset reload/unloaded state to rendering.
            if (updatePreviewModel(scene->CSGData, modelData))
            {
                LOG(Warning, "Failed to update live CSG preview model");
                return true;
            }

            // Import model data to the asset
            {
                Guid modelDataAssetId = scene->CSGData.Model.GetID();
                if (!modelDataAssetId.IsValid())
                    modelDataAssetId = Guid::New();
                const String modelDataAssetPath = sceneDataFolderPath / TEXT("CSG_Mesh") + ASSET_FILES_EXTENSION_WITH_DOT;
                if (AssetsImportingManager::Create(AssetsImportingManager::CreateModelTag, modelDataAssetPath, modelDataAssetId, &modelData))
                {
                    LOG(Warning, "Failed to import CSG mesh data");
                    return true;
                }
                data.outputModelAssetId = modelDataAssetId;
            }

            data.brushesCount = meshData.Brushes.Count();

            // Generate asset with CSG mesh metadata (for collisions and brush queries)
            {
                Guid rawDataAssetId = scene->CSGData.Data.GetID();
                if (!rawDataAssetId.IsValid())
                    rawDataAssetId = Guid::New();
                const String rawDataAssetPath = sceneDataFolderPath / TEXT("CSG_Data") + ASSET_FILES_EXTENSION_WITH_DOT;
                if (generateRawDataAsset(meshData, rawDataAssetId, rawDataAssetPath))
                {
                    LOG(Warning, "Failed to create raw CSG data");
                    return true;
                }
                data.outputRawDataAssetId = rawDataAssetId;
            }

            // Generate CSG mesh collision asset
            {
                // Convert CSG mesh to scene local space (fix issues when scene has transformation applied)
                if (!scene->GetTransform().IsIdentity())
                {
                    Matrix m1, m2;
                    scene->GetTransform().GetWorld(m1);
                    Matrix::Invert(m1, m2);

                    for (int32 lodIndex = 0; lodIndex < modelData.LODs.Count(); lodIndex++)
                    {
                        auto lod = &modelData.LODs[lodIndex];
                        for (int32 meshIndex = 0; meshIndex < lod->Meshes.Count(); meshIndex++)
                        {
                            Array<Float3>& v = lod->Meshes[meshIndex]->Positions;
                            for (int32 i = 0; i < v.Count(); i++)
                                Float3::Transform(v[i], m2, v[i]);
                        }
                    }
                }

#if COMPILE_WITH_PHYSICS_COOKING
                CollisionCooking::Argument arg;
                arg.Type = CollisionDataType::TriangleMesh;
                arg.OverrideModelData = &modelData;
                arg.Model = data.outputModelAssetId;
                Guid collisionDataAssetId = scene->CSGData.CollisionData.GetID();
                if (!collisionDataAssetId.IsValid())
                    collisionDataAssetId = Guid::New();
                const String collisionDataAssetPath = sceneDataFolderPath / TEXT("CSG_Collision") + ASSET_FILES_EXTENSION_WITH_DOT;
                if (AssetsImportingManager::Create(AssetsImportingManager::CreateCollisionDataTag, collisionDataAssetPath, collisionDataAssetId, &arg))
                {
                    LOG(Warning, "Failed to cook CSG mesh collision data");
                    return true;
                }
                data.outputCollisionDataAssetId = collisionDataAssetId;
#else
                data.outputCollisionDataAssetId = Guid::Empty;
#endif
            }
        }
    }

    return false;
}

bool CSGBuilderImpl::buildInner(CSGModel* model, BuildData& data)
{
    // Compile explicit stacks and implicit brushes under the CSGModel target in model-local space
    CSG::Mesh combinedMesh;
    if (!CSGCompilation::CompileTargetMeshes(model, combinedMesh))
        return false;
    if (combinedMesh.GetPolygons()->IsEmpty())
        return false;

    // Triangulate meshes
    {
        RawData meshData;
        Array<MeshVertex> vertexBuffer;
        combinedMesh.Triangulate(meshData, vertexBuffer);
        meshData.RemoveEmptySlots();
        if (meshData.Slots.HasItems())
        {
            Scene* scene = model->GetScene();
            if (scene == nullptr)
                return false;
            const auto sceneDataFolderPath = scene->GetDataFolderPath();

            ModelData modelData;
            meshData.ToModelData(modelData);

            if (updatePreviewModel(model->CSGData, modelData))
            {
                LOG(Warning, "Failed to update live CSG preview model");
                return true;
            }

            // Import model data to the asset
            {
                Guid modelDataAssetId = model->CSGData.Model.GetID();
                if (!modelDataAssetId.IsValid())
                    modelDataAssetId = Guid::New();
                const String modelDataAssetPath = sceneDataFolderPath / String::Format(TEXT("CSG_Model_{0}"), model->GetID().ToString(Guid::FormatType::N)) + ASSET_FILES_EXTENSION_WITH_DOT;
                if (AssetsImportingManager::Create(AssetsImportingManager::CreateModelTag, modelDataAssetPath, modelDataAssetId, &modelData))
                {
                    LOG(Warning, "Failed to import CSG mesh data");
                    return true;
                }
                data.outputModelAssetId = modelDataAssetId;
            }

            data.brushesCount = meshData.Brushes.Count();

            // Generate asset with CSG mesh metadata
            {
                Guid rawDataAssetId = model->CSGData.Data.GetID();
                if (!rawDataAssetId.IsValid())
                    rawDataAssetId = Guid::New();
                const String rawDataAssetPath = sceneDataFolderPath / String::Format(TEXT("CSG_Data_{0}"), model->GetID().ToString(Guid::FormatType::N)) + ASSET_FILES_EXTENSION_WITH_DOT;
                if (generateRawDataAsset(meshData, rawDataAssetId, rawDataAssetPath))
                {
                    LOG(Warning, "Failed to create raw CSG data");
                    return true;
                }
                data.outputRawDataAssetId = rawDataAssetId;
            }

            // Generate CSG mesh collision asset
            {
#if COMPILE_WITH_PHYSICS_COOKING
                CollisionCooking::Argument arg;
                arg.Type = CollisionDataType::TriangleMesh;
                arg.OverrideModelData = &modelData;
                arg.Model = data.outputModelAssetId;
                Guid collisionDataAssetId = model->CSGData.CollisionData.GetID();
                if (!collisionDataAssetId.IsValid())
                    collisionDataAssetId = Guid::New();
                const String collisionDataAssetPath = sceneDataFolderPath / String::Format(TEXT("CSG_Collision_{0}"), model->GetID().ToString(Guid::FormatType::N)) + ASSET_FILES_EXTENSION_WITH_DOT;
                if (AssetsImportingManager::Create(AssetsImportingManager::CreateCollisionDataTag, collisionDataAssetPath, collisionDataAssetId, &arg))
                {
                    LOG(Warning, "Failed to cook CSG mesh collision data");
                    return true;
                }
                data.outputCollisionDataAssetId = collisionDataAssetId;
#else
                data.outputCollisionDataAssetId = Guid::Empty;
#endif
            }
        }
    }

    return false;
}

void CSGBuilderImpl::build(Scene* scene)
{
    // Start
    auto startTime = DateTime::Now();
    LOG(Info, "Start building CSG...");

    // Build
    BuildData data;
    bool failed = buildInner(scene, data);

    // A failed or transiently invalid edit must not replace the last good result
    // with empty references. The next successful request will update it.
    if (failed)
    {
        LOG(Warning, "Failed to build CSG. Preserving the previous result.");
        return;
    }

    // Link new (or empty) CSG mesh
    auto outputData = Content::LoadAsync<RawDataAsset>(data.outputRawDataAssetId);
    auto outputModel = Content::LoadAsync<Model>(data.outputModelAssetId);
    auto outputCollisionData = Content::LoadAsync<CollisionData>(data.outputCollisionDataAssetId);

    if (!outputModel)
    {
        GPUDeviceLock gpuLock(GPUDevice::Instance);
        scene->CSGData.PreviewModel = nullptr;
        scene->CSGData.PreviewModelCache = nullptr;
    }
    scene->CSGData.Data = outputData;
    scene->CSGData.Model = outputModel;
    scene->CSGData.CollisionData = outputCollisionData;
    // TODO: also set CSGData.InstanceBuffer - lightmap scales for the entries so csg mesh gets better quality in lightmaps
    scene->CSGData.PostCSGBuild();

    // End
    const int32 brushesCount = data.brushesCount;
    auto endTime = DateTime::Now();
    LOG(Info, "CSG build in {0} ms! {1} brush(es)", (endTime - startTime).GetTotalMilliseconds(), brushesCount);
}

void CSGBuilderImpl::build(CSGModel* model)
{
    if (model == nullptr)
        return;

    auto startTime = DateTime::Now();
    LOG(Info, "Start building CSG Model {0}...", model->GetName());

    BuildData data;
    bool failed = buildInner(model, data);

    if (failed)
    {
        LOG(Warning, "Failed to build CSG model. Preserving the previous result.");
        return;
    }

    auto outputData = Content::LoadAsync<RawDataAsset>(data.outputRawDataAssetId);
    auto outputModel = Content::LoadAsync<Model>(data.outputModelAssetId);
    auto outputCollisionData = Content::LoadAsync<CollisionData>(data.outputCollisionDataAssetId);

    if (!outputModel)
    {
        GPUDeviceLock gpuLock(GPUDevice::Instance);
        model->CSGData.PreviewModel = nullptr;
        model->CSGData.PreviewModelCache = nullptr;
    }
    model->CSGData.Data = outputData;
    model->CSGData.Model = outputModel;
    model->CSGData.CollisionData = outputCollisionData;
    model->CSGData.PostCSGBuild();

    const int32 brushesCount = data.brushesCount;
    auto endTime = DateTime::Now();
    LOG(Info, "CSG Model build in {0} ms! {1} brush(es)", (endTime - startTime).GetTotalMilliseconds(), brushesCount);
}

bool CSGBuilderImpl::generateRawDataAsset(RawData& meshData, Guid& assetId, const String& assetPath)
{
    // Prepare data
    MemoryWriteStream stream(4096);
    {
        // Header (with version number)
        stream.WriteInt32(1);
        const int32 brushesCount = meshData.Brushes.Count();
        stream.WriteInt32(brushesCount);

        // Surfaces data locations
        int32 surfacesDataOffset = sizeof(int32) * 2 + (sizeof(Guid) + sizeof(int32)) * brushesCount;
        for (auto brush = meshData.Brushes.Begin(); brush.IsNotEnd(); ++brush)
        {
            auto& surfaces = brush->Value.Surfaces;

            stream.Write(brush->Key);
            stream.WriteInt32(surfacesDataOffset);

            // Calculate offset in data storage to the next brush data
            int32 brushDataSize = 0;
            for (int32 i = 0; i < surfaces.Count(); i++)
            {
                brushDataSize += sizeof(int32) + sizeof(RawData::SurfaceTriangle) * surfaces[i].Triangles.Count();
            }
            surfacesDataOffset += brushDataSize;
        }

        // Surfaces data
        for (auto brush = meshData.Brushes.Begin(); brush.IsNotEnd(); ++brush)
        {
            auto& surfaces = brush->Value.Surfaces;

            for (int32 i = 0; i < surfaces.Count(); i++)
            {
                auto& triangles = surfaces[i].Triangles;

                stream.WriteInt32(triangles.Count());
                stream.WriteBytes(triangles.Get(), triangles.Count() * sizeof(RawData::SurfaceTriangle));
            }
        }
    }

    // Serialize
    BytesContainer bytesContainer;
    bytesContainer.Link(ToSpan(stream));
    return AssetsImportingManager::Create(AssetsImportingManager::CreateRawDataTag, assetPath, assetId, (void*)&bytesContainer);
}

#endif
