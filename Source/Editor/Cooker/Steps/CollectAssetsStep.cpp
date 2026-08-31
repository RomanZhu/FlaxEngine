// Copyright (c) Wojciech Figat. All rights reserved.

#include "CollectAssetsStep.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Asset.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Core/Log.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Cache/AssetsCache.h"

namespace
{
    bool HasExactRuntimeProcessor(const AssetRecord& record)
    {
        return record.ProcessorID == TEXT("Flax.Texture") ||
            record.ProcessorID == TEXT("Flax.Model") ||
            record.ProcessorID == TEXT("Flax.GraphDocument") ||
            record.ProcessorID == TEXT("Flax.ExistingJson") ||
            record.ProcessorID == TEXT("Flax.MaterialInstance") ||
            record.ProcessorID == TEXT("Flax.SkeletonMask") ||
            record.ProcessorID == TEXT("Flax.SceneAnimation") ||
            record.ProcessorID == TEXT("Flax.ParticleSystem") ||
            record.ProcessorID == TEXT("Flax.CollisionData") ||
            record.ProcessorID == TEXT("Flax.Audio") ||
            record.ProcessorID == TEXT("Flax.Font") ||
            record.ProcessorID == TEXT("Flax.Video") ||
            record.ProcessorID == TEXT("Flax.Text") ||
            record.ProcessorID == TEXT("Flax.ShaderSource");
    }

    bool QueueRuntimeObjectClosure(const AssetRecord& record, Array<Guid>& queue)
    {
        if (record.RuntimeObjectReferences.HasItems())
        {
            for (const AssetObjectId& reference : record.RuntimeObjectReferences)
            {
                AssetRecord referencedRecord;
                if (!AssetDatabase::Get().TryGetRecord(reference, referencedRecord))
                    return true;
                queue.Add(referencedRecord.ID);
            }
        }
        else
        {
            queue.Add(record.RuntimeReferences);
        }
        if (!record.IsMainAsset())
            return false;
        Array<AssetRecord> objects;
        AssetDatabase::Get().GetSubAssets(record.SourceAssetID, objects);
        for (const AssetRecord& object : objects)
        {
            // MissingSource is the durable tombstone state. Every other live object is queued
            // so a bad current artifact fails explicitly instead of being stripped silently.
            if (object.Status != AssetRecordStatus::MissingSource)
                queue.Add(object.ID);
        }
        return false;
    }
}

bool CollectAssetsStep::Perform(CookingData& data)
{
    LOG(Info, "Searching for assets to include in a build. Using {0} root assets.", data.RootAssets.Count());
    data.StepProgress(TEXT("Collecting assets"), 0);

    // Initialize assets queue
    Array<Guid> assetsQueue;
    assetsQueue.Clear();
    assetsQueue.EnsureCapacity(1024);
    for (auto i = data.RootAssets.Begin(); i.IsNotEnd(); ++i)
        assetsQueue.Add(i->Item);

    // Iterate through the assets graph
    AssetInfo assetInfo;
    Array<Guid> references;
    Array<String> files;
    const bool hardCut = AssetDatabase::Get().IsHardCutEnabled();
    while (assetsQueue.HasItems())
    {
        BUILD_STEP_CANCEL_CHECK;
        const Guid assetId = assetsQueue.Dequeue();

        // Skip already processed or invalid assets
        if (!assetId.IsValid() || data.Assets.Contains(assetId))
            continue;
        AssetRecord canonicalRecord;
        const bool hasCanonicalRecord = AssetDatabase::Get().TryGetRecord(assetId, canonicalRecord);
        if (hardCut)
        {
            if (!hasCanonicalRecord)
            {
                LOG(Error, "Hard-cut cook root/reference {0} has no canonical database record.", assetId);
                return true;
            }
            if (!HasExactRuntimeProcessor(canonicalRecord))
            {
                LOG(Error, "Hard-cut cook root/reference {0} uses processor '{1}', which has no exact runtime output.", assetId, canonicalRecord.ProcessorID);
                return true;
            }
            LOG_STR(Info, canonicalRecord.CanonicalPath.Get());
            data.Assets.Add(assetId);
            if (QueueRuntimeObjectClosure(canonicalRecord, assetsQueue))
            {
                LOG(Error, "Hard-cut cook object {0}:{1} has an unresolved exact runtime reference.",
                    canonicalRecord.SourceAssetID, canonicalRecord.LocalId);
                return true;
            }
            continue;
        }
        if (hasCanonicalRecord && canonicalRecord.SourceKind != AssetSourceKind::LegacyBinary &&
            canonicalRecord.ProcessorID != TEXT("Flax.Texture") && canonicalRecord.ProcessorID != TEXT("Flax.Model") &&
            canonicalRecord.ProcessorID != TEXT("Flax.GraphDocument") &&
            canonicalRecord.ProcessorID != TEXT("Flax.ExistingJson") &&
            canonicalRecord.ProcessorID != TEXT("Flax.MaterialInstance") &&
            canonicalRecord.ProcessorID != TEXT("Flax.SkeletonMask") &&
            canonicalRecord.ProcessorID != TEXT("Flax.SceneAnimation") &&
            canonicalRecord.ProcessorID != TEXT("Flax.ParticleSystem") &&
            canonicalRecord.ProcessorID != TEXT("Flax.CollisionData") &&
            canonicalRecord.ProcessorID != TEXT("Flax.Audio") &&
            canonicalRecord.ProcessorID != TEXT("Flax.Font") &&
            canonicalRecord.ProcessorID != TEXT("Flax.Video") &&
            canonicalRecord.ProcessorID != TEXT("Flax.Text") &&
            canonicalRecord.ProcessorID != TEXT("Flax.ShaderSource"))
        {
            LOG(Warning, "Skipping canonical cooker root {0}; processor '{1}' is not converted yet.", assetId, canonicalRecord.ProcessorID);
            continue;
        }
        if (hasCanonicalRecord)
            assetInfo = canonicalRecord.ToAssetInfo();
        else if (!Content::GetRegistry()->FindAsset(assetId, assetInfo))
            continue;

        // Skip some assets (with no refs and not required to load)
        if (assetInfo.TypeName == Texture::TypeName ||
            assetInfo.TypeName == CubeTexture::TypeName ||
            assetInfo.TypeName == Shader::TypeName ||
            (hasCanonicalRecord && canonicalRecord.ProcessorID == TEXT("Flax.Video")))
        {
            LOG_STR(Info, assetInfo.Path);
            data.Assets.Add(assetId);
            continue;
        }

        // Load asset
        AssetReference<Asset> asset = Content::LoadAsync<Asset>(assetId);
        if (asset == nullptr)
            continue;
        LOG_STR(Info, asset->GetPath());
        data.Assets.Add(assetId);

        // Skip virtual/temporary assets
        if (asset->IsVirtual())
            continue;

        // Asset should have loaded data
        if (asset->WaitForLoaded())
            continue;

        // Gather asset references
        references.Clear();
        asset->Locker.Lock();
        asset->GetReferences(references, files);
        asset->Locker.Unlock();
        assetsQueue.Add(references);
        for (String& file : files)
        {
            if (file.HasChars())
                data.Files.Add(MoveTemp(file));
        }
    }

    data.Stats.TotalAssets = data.Assets.Count();
    LOG(Info, "Found {0} assets to deploy!", data.Assets.Count());

    return false;
}
