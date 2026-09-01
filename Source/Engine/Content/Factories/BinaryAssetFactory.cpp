// Copyright (c) Wojciech Figat. All rights reserved.

#include "BinaryAssetFactory.h"
#include "../BinaryAsset.h"
#include "Engine/Core/Log.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

bool BinaryAssetFactoryBase::Init(BinaryAsset* asset)
{
    ASSERT(asset && asset->Storage);
    auto storage = asset->Storage;

    // Load serialized asset data
    AssetInitData initData;
    const bool headerLoadFailed = storage->UsesAssetObjectIds()
        ? storage->LoadAssetHeader(asset->GetPersistentObjectId(), initData)
        : storage->LoadAssetHeader(asset->GetID(), initData);
    if (headerLoadFailed)
    {
        LOG(Error, "Cannot load asset header.\nInfo: {0}", AssetInfo(asset->GetID(), asset->GetTypeName(), storage->GetPath()).ToString());
        return true;
    }
    if (initData.Header.ID != asset->GetID() || initData.Header.TypeName != asset->GetTypeName())
    {
        LOG(Error, "Resolved artifact header does not match canonical asset identity.\nInfo: {0}", AssetInfo(asset->GetID(), asset->GetTypeName(), asset->GetPath()).ToString());
        return true;
    }

    // Check if serialized asset version is supported
    if (!IsVersionSupported(initData.SerializedVersion))
    {
        if (asset->IsUsingGeneratedArtifact())
        {
            asset->MarkArtifactRebuildRequired();
            LOG(Warning, "{0}: Generated artifact version {1} requires regeneration. Asset: '{2}'.", GetAssetPipelineDiagnosticCodeName(AssetPipelineDiagnosticCode::ArtifactRebuildRequired), initData.SerializedVersion, asset->GetPath());
            return true;
        }
        LOG(Warning, "Asset version {1} is not supported.\nInfo: {0}", AssetInfo(asset->GetID(), asset->GetTypeName(), storage->GetPath()).ToString(), initData.SerializedVersion);
        return true;
    }

    // Initialize asset
    if (asset->Init(initData))
    {
        LOG(Error, "Cannot initialize asset.\nInfo: {0}", AssetInfo(asset->GetID(), asset->GetTypeName(), storage->GetPath()).ToString());
        return true;
    }

    return false;
}

Asset* BinaryAssetFactoryBase::New(const AssetLoadLocation& location)
{
    const AssetInfo& info = location.Info;
    const String& storagePath = location.Artifact.StoragePath.Get();

    if (storagePath.IsEmpty() || location.Artifact.ObjectID != info.ObjectID ||
        location.Artifact.AssetID != info.ID || location.Artifact.TypeName != info.TypeName)
    {
        LOG(Warning, "Invalid resolved artifact identity or storage path.\nInfo: {0}", info.ToString());
        return nullptr;
    }

    // Get the asset storage container but don't load it now
    const auto storage = ContentStorageManager::GetStorage(storagePath, false);
    if (!storage)
    {
        // Note: missing file situation should be handled before asset creation
        LOG(Warning, "Missing asset storage container at \'{0}\'!\nInfo: ", storagePath, info.ToString());
        return nullptr;
    }

    // Create asset object
    auto result = Create(info);
    result->SetResolvedArtifact(location.Artifact);

    // Perform fast init, we assume that given AssetInfo is valid 
    // and we can create asset object now without further verification
    // which will be done during asset loading on content pool thread.
    // Asset storage validation and loading happens on the content pool thread.
    AssetHeader header;
    header.ID = info.ID;
    header.TypeName = info.TypeName;
    if (result->Init(storage, header))
    {
        LOG(Warning, "Cannot initialize asset.\nInfo: {0}", info.ToString());
        Delete(result);
        result = nullptr;
    }

    return result;
}

Asset* BinaryAssetFactoryBase::NewVirtual(const AssetInfo& info)
{
    // Create asset object
    auto result = Create(info);

    // Initialize with virtual data
    AssetInitData initData;
    initData.Header.ID = info.ID;
    initData.Header.TypeName = info.TypeName;
    initData.SerializedVersion = result->GetSerializedVersion();
    if (result->InitVirtual(initData))
    {
        LOG(Warning, "Cannot initialize asset.\nInfo: {0}", info.ToString());
        Delete(result);
        result = nullptr;
    }

    return result;
}
