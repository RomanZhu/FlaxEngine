// Copyright (c) Wojciech Figat. All rights reserved.

#include "CSGGeneratedOutputPaths.h"
#include "Engine/Level/Actors/CSGModel.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Content/Content.h"
#include "Engine/Engine/Globals.h"

using namespace CSG;

bool CSGGeneratedOutputPathResolver::ResolveForScene(CSGModel* model, CSGGeneratedOutputPaths& output)
{
    if (model == nullptr)
        return false;

    Scene* scene = model->GetScene();
    if (scene == nullptr)
        return false;

    const String sceneDataFolderPath = scene->GetDataFolderPath();
    if (!FileSystem::DirectoryExists(sceneDataFolderPath))
        FileSystem::CreateDirectory(sceneDataFolderPath);

    const String modelIdStr = model->GetID().ToString(Guid::FormatType::N);
    output.Model = sceneDataFolderPath / String::Format(TEXT("CSG_Model_{0}"), modelIdStr) + ASSET_FILES_EXTENSION_WITH_DOT;
    output.RawData = sceneDataFolderPath / String::Format(TEXT("CSG_Data_{0}"), modelIdStr) + ASSET_FILES_EXTENSION_WITH_DOT;
    output.Collision = sceneDataFolderPath / String::Format(TEXT("CSG_Collision_{0}"), modelIdStr) + ASSET_FILES_EXTENSION_WITH_DOT;
    return true;
}

bool CSGGeneratedOutputPathResolver::ResolveForAsset(const Guid& ownerAssetId, CSGModel* model, CSGGeneratedOutputPaths& output)
{
    if (model == nullptr)
        return false;

    if (!ownerAssetId.IsValid())
        return ResolveForScene(model, output);

    const String prefabDataFolderPath = Globals::ProjectContentFolder / TEXT("PrefabData") / ownerAssetId.ToString(Guid::FormatType::N);
    if (!FileSystem::DirectoryExists(prefabDataFolderPath))
        FileSystem::CreateDirectory(prefabDataFolderPath);

    const String modelIdStr = model->GetID().ToString(Guid::FormatType::N);
    output.Model = prefabDataFolderPath / String::Format(TEXT("CSG_Model_{0}"), modelIdStr) + ASSET_FILES_EXTENSION_WITH_DOT;
    output.RawData = prefabDataFolderPath / String::Format(TEXT("CSG_Data_{0}"), modelIdStr) + ASSET_FILES_EXTENSION_WITH_DOT;
    output.Collision = prefabDataFolderPath / String::Format(TEXT("CSG_Collision_{0}"), modelIdStr) + ASSET_FILES_EXTENSION_WITH_DOT;
    return true;
}
