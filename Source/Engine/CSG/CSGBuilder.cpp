// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGBuilder.h"
#include "CSGMesh.h"
#include "CSGData.h"
#include "CSGCompilation.h"
#include "Engine/Level/Level.h"
#include "Engine/Level/SceneQuery.h"
#include "Engine/Level/Actor.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Types/TimeSpan.h"
#include "Engine/Graphics/Models/ModelData.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/ContentImporters/GeneratedAssetBuilder.h"
#include "Engine/ContentImporters/ImportModel.h"
#include "Engine/ContentImporters/CreateRawData.h"
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

struct BuildData
{
    int32 brushesCount = 0;
    Guid outputModelAssetId = Guid::Empty;
    Guid outputRawDataAssetId = Guid::Empty;
    Guid outputCollisionDataAssetId = Guid::Empty;

    BuildData() = default;
};

namespace CSGBuilderImpl
{
    Array<Scene*> ScenesToRebuild;

    void onSceneUnloading(Scene* scene, const Guid& sceneId);
    bool buildInner(Scene* scene, BuildData& data);
    void build(Scene* scene);
    bool updatePreviewModel(AssetReference<Model>& previewModel, const ModelData& modelData);
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

}

bool Builder::IsActive()
{
    return ScenesToRebuild.HasItems();
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


bool CSGBuilderImpl::updatePreviewModel(AssetReference<Model>& previewModel, const ModelData& modelData)
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
        destinationSlot.Material = Content::LoadRuntimeObjectAsync<MaterialBase>(sourceSlot.AssetID);
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

    return false;
}

Model* CSGPreviewBuilder::Build(Actor* root, Model* reusableModel)
{
    if (root == nullptr)
        return nullptr;

    CSG::Mesh combinedMesh;
    if (!CSGCompilation::CompileTargetMeshes(root, combinedMesh) || combinedMesh.GetPolygons()->IsEmpty())
        return nullptr;

    RawData meshData;
    Array<MeshVertex> vertexBuffer;
    combinedMesh.Triangulate(meshData, vertexBuffer);
    meshData.RemoveEmptySlots();
    if (meshData.Slots.IsEmpty())
        return nullptr;

    ModelData modelData;
    meshData.ToModelData(modelData);
    AssetReference<Model> previewModel = reusableModel;
    if (updatePreviewModel(previewModel, modelData))
    {
        LOG(Warning, "Failed to build transient CSG preview model");
        return nullptr;
    }
    return previewModel.Get();
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
            AssetReference<Model> previewModel = scene->CSGData.PreviewModelCache;
            if (updatePreviewModel(previewModel, modelData))
            {
                LOG(Warning, "Failed to update live CSG preview model");
                return true;
            }
            // Publish only after the entire model is ready. The render lock guarantees no
            // draw list still contains pointers to the model moving into the cache.
            scene->CSGData.PreviewModelCache = scene->CSGData.PreviewModel;
            scene->CSGData.PreviewModel = previewModel;

            // Import model data to the asset
            {
                Guid modelDataAssetId = scene->CSGData.Model.GetRuntimeInstanceId();
                if (!modelDataAssetId.IsValid())
                    modelDataAssetId = Guid::New();
                const String modelDataAssetPath = sceneDataFolderPath / TEXT("CSG_Mesh") + ASSET_FILES_EXTENSION_WITH_DOT;
                if (GeneratedAssetBuilder::Build(&ImportModel::Create, modelDataAssetPath, Model::TypeName, modelDataAssetId, &modelData))
                {
                    LOG(Warning, "Failed to import CSG mesh data");
                    return true;
                }
                data.outputModelAssetId = modelDataAssetId;
            }

            data.brushesCount = meshData.Brushes.Count();

            // Generate asset with CSG mesh metadata (for collisions and brush queries)
            {
                Guid rawDataAssetId = scene->CSGData.Data.GetRuntimeInstanceId();
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
                arg.Model = Content::ResolveRuntimeObjectId(data.outputModelAssetId);
                Guid collisionDataAssetId = scene->CSGData.CollisionData.GetRuntimeInstanceId();
                if (!collisionDataAssetId.IsValid())
                    collisionDataAssetId = Guid::New();
                const String collisionDataAssetPath = sceneDataFolderPath / TEXT("CSG_Collision") + ASSET_FILES_EXTENSION_WITH_DOT;
                if (GeneratedAssetBuilder::Build(&CreateCollisionData::Create, collisionDataAssetPath, CollisionData::TypeName, collisionDataAssetId, &arg))
                {
                    LOG(Warning, "Failed to cook CSG mesh collision data");
                    return true;
                }
                data.outputCollisionDataAssetId = collisionDataAssetId;
#endif
            }
        }
    }

    return false;
}

void CSGBuilderImpl::build(Scene* scene)
{
    if (scene == nullptr)
        return;

    auto startTime = DateTime::Now();
    LOG(Info, "Start building CSG for scene \'{0}\'...", scene->GetName());

    // Build
    BuildData data;
    if (buildInner(scene, data))
    {
        LOG(Warning, "Failed to build CSG for scene \'{0}\'.", scene->GetName());
        return;
    }

    // Assign results
    auto outputData = Content::LoadRuntimeObjectAsync<RawDataAsset>(data.outputRawDataAssetId);
    auto outputModel = Content::LoadRuntimeObjectAsync<Model>(data.outputModelAssetId);
    auto outputCollisionData = Content::LoadRuntimeObjectAsync<CollisionData>(data.outputCollisionDataAssetId);

    scene->CSGData.Data = outputData;
    if (!outputModel)
    {
        GPUDeviceLock gpuLock(GPUDevice::Instance);
        scene->CSGData.ClearTransientPreview();
        scene->CSGData.Model = outputModel;
    }
    else
    {
        scene->CSGData.Model = outputModel;
    }
    scene->CSGData.CollisionData = outputCollisionData;
    scene->CSGData.PostCSGBuild();

    auto endTime = DateTime::Now();
    LOG(Info, "CSG build for scene \'{0}\' in {1} ms! {2} brush(es)", scene->GetName(), (endTime - startTime).GetTotalMilliseconds(), data.brushesCount);
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
    return GeneratedAssetBuilder::Build(&CreateRawData::Create, assetPath, RawDataAsset::TypeName, assetId, &bytesContainer);
}

#endif
