// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseFacade.h"
#include "EngineContentCatalog.h"
#include "AssetDatabaseStorage.h"
#include "AssetSourceRoots.h"
#include "AssetMeta.h"
#include "AssetMutationService.h"
#include "AssetMount.h"
#include "MigrationInventory.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Asset.h"
#include "Engine/Content/AssetPipeline/AssetPipelineBootstrap.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Content/Assets/SkeletonMask.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/JsonStorageProxy.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Documents/CollisionDataDocument.h"
#include "Engine/Content/Documents/MaterialInstanceDocument.h"
#include "Engine/Content/Documents/SceneAnimationDocument.h"
#include "Engine/Content/Documents/ParticleSystemDocument.h"
#include "Engine/Animations/CurveSerialization.h"
#include "Engine/Animations/AnimEvent.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/GameplayGlobals.h"
#include "Engine/Physics/CollisionData.h"
#include "LegacyAssetMigrator.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/CPUInfo.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Utilities/Crc.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Core/Collections/HashSet.h"
#if COMPILE_WITH_MATERIAL_GRAPH
#include "Engine/Tools/MaterialGenerator/Types.h"
#endif
#if COMPILE_WITH_TEXTURE_TOOL
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Render2D/SpriteAtlas.h"
#include "Engine/Content/Build/Processors/TextureProcessorSettings.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/TexturePipelineService.h"
#endif
#endif
#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include "Engine/Content/Build/Processors/ModelProcessorSettings.h"
#include "Engine/Content/Build/Processors/ModelProcessor.h"
#include "Engine/Content/Build/Processors/ModelSubAssetKeys.h"
#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#endif
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
#include "Engine/Content/Build/Processors/GraphDocumentProcessor.h"
#include "Engine/Content/Build/Processors/GraphPipelineService.h"
#include "Engine/Content/Build/Processors/ImportedSourceProcessor.h"
#endif
#include <algorithm>
#include <future>
#include <vector>

Delegate<uint64> AssetDatabaseFacade::DatabaseChanged;
Delegate<Guid> AssetDatabaseFacade::ArtifactPublished;

void AssetDatabaseFacade::NotifyArtifactPublished(const Guid& assetID)
{
    ArtifactPublished(assetID);
}

namespace
{
    CriticalSection StateLocker;
    CriticalSection MetadataMutationLocker;
    Array<AssetPipelineDiagnostic> LastDiagnostics;
    AssetDatabaseChangeInfo LastChange;
    Array<AssetDatabaseFileState> LastFileStates;
    SourceHashCache HashCache;
    bool IsBound = false;
    int32 ConfiguredAssetSystemVersion = 0;
    int32 ConfiguredWorkerLimit = 1;
    int32 ConfiguredMemoryLimitMegabytes = 4096;

    String AssetPipelineLibraryFolder()
    {
#if USE_EDITOR
        return Globals::ProjectLibraryFolder;
#else
        return Globals::ProjectFolder / TEXT("Library");
#endif
    }

    bool IsManifestBackedEngineContentMount(const AssetMount& mount)
    {
        return mount.Kind == AssetMountKind::EngineContent;
    }

    bool MountTablesMatch(const Array<AssetMount>& a, const Array<AssetMount>& b)
    {
        if (a.Count() != b.Count())
            return false;
        for (int32 i = 0; i < a.Count(); i++)
        {
            if (a[i].MountId != b[i].MountId || a[i].LogicalPrefix != b[i].LogicalPrefix ||
                !FileSystem::AreFilePathsEquivalent(a[i].PhysicalRoot, b[i].PhysicalRoot) ||
                a[i].Kind != b[i].Kind || a[i].Writable != b[i].Writable ||
                a[i].AllowLinkedRoot != b[i].AllowLinkedRoot)
                return false;
        }
        return true;
    }

    bool ActivateBootstrapMounts(const AssetPipelineBootstrapSnapshot& bootstrap, Array<AssetPipelineDiagnostic>& diagnostics)
    {
        Array<AssetMount> mounts = bootstrap.Mounts;
        bool hasEngineMount = false;
        for (const AssetMount& mount : mounts)
            hasEngineMount |= mount.Kind == AssetMountKind::EngineContent;

        // Retain the pre-descriptor engine content root for older projects. A validated
        // EngineContent descriptor is authoritative whenever one is present.
        const String legacyEngineRoot = AssetSourceRoots::GetEngineRoot();
        if (!hasEngineMount && FileSystem::DirectoryExists(legacyEngineRoot))
        {
            AssetMount engine;
            engine.MountId = Guid(0x765641d8, 0xbc544336, 0xa6204809, 0x47f624c0);
            engine.LogicalPrefix = TEXT("EngineContent");
            engine.PhysicalRoot = legacyEngineRoot;
            engine.Kind = AssetMountKind::EngineContent;
            engine.Writable = false;
            engine.AllowLinkedRoot = true;
            mounts.Add(MoveTemp(engine));
        }
        if (mounts.Count() > 1)
        {
            std::sort(mounts.Get(), mounts.Get() + mounts.Count(), [](const AssetMount& a, const AssetMount& b)
            {
                if (a.LogicalPrefix != b.LogicalPrefix)
                    return a.LogicalPrefix < b.LogicalPrefix;
                return a.MountId.ToString(Guid::FormatType::N) < b.MountId.ToString(Guid::FormatType::N);
            });
        }
        AssetPipelineDiagnostic diagnostic;
        if (mounts.IsEmpty() || AssetMountRegistry::Get().ReplaceAll(mounts, diagnostic))
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
                diagnostic.SourcePath = Globals::ProjectContentFolder;
                diagnostic.Message = TEXT("The validated asset-pipeline bootstrap contains no active content mounts.");
            }
            diagnostics.Add(MoveTemp(diagnostic));
            return true;
        }
        return false;
    }

    bool ReadProjectBootstrap(Array<AssetPipelineDiagnostic>& diagnostics)
    {
        Array<String> descriptors;
        if (FileSystem::DirectoryGetFiles(descriptors, Globals::ProjectFolder, TEXT("*.flaxproj"), DirectorySearchOption::TopDirectoryOnly) || descriptors.Count() != 1)
        {
            AssetPipelineDiagnostic diagnostic;
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
            diagnostic.SourcePath = Globals::ProjectFolder;
            diagnostic.Message = TEXT("Asset-system bootstrap requires exactly one project descriptor in the project root.");
            diagnostics.Add(MoveTemp(diagnostic));
            ConfiguredAssetSystemVersion = 0;
            ConfiguredWorkerLimit = 1;
            return true;
        }
        const AssetPipelineBootstrapSnapshot bootstrap = AssetPipelineBootstrap::Validate(descriptors[0], Globals::ProjectContentFolder);
        diagnostics.Add(bootstrap.Diagnostics);
        ConfiguredAssetSystemVersion = bootstrap.AssetSystemVersion;
        ConfiguredWorkerLimit = bootstrap.WorkerLimit > 0
            ? Math::Clamp(bootstrap.WorkerLimit, 1, 64)
            : Math::Clamp(static_cast<int32>(Platform::GetCPUInfo().ProcessorCoreCount) - 1, 1, 64);
        ConfiguredMemoryLimitMegabytes = Math::Max(128, bootstrap.MemoryLimitMegabytes);
        // Keep the shipping hard cut active for every recognized asset-system marker.
        // Pre-v3 projects are opened for migration/read-only flows, not mutable legacy editing.
        AssetDatabase::Get().SetHardCutEnabled(true);
        if (!bootstrap.Valid || bootstrap.ReadOnly || bootstrap.RequiresMigration)
            return true;
        return ActivateBootstrapMounts(bootstrap, diagnostics);
    }

    String GetMountProjectRoot(const AssetMount& mount)
    {
        if (mount.Kind == AssetMountKind::ProjectContent)
            return Globals::ProjectFolder;
        if (FileSystem::AreFilePathsEquivalent(mount.PhysicalRoot, AssetSourceRoots::GetEngineRoot()))
            return Globals::StartupFolder;
        return StringUtils::GetDirectoryName(mount.PhysicalRoot);
    }

    void QualifyMountRecords(const AssetMount& mount, Array<AssetRecord>& records)
    {
        for (AssetRecord& record : records)
        {
            String relative = FileSystem::ConvertAbsolutePathToRelative(mount.PhysicalRoot, record.SourcePath.Get());
            relative.Replace('\\', '/');
            if (relative == TEXT("."))
                relative.Clear();
            record.PortabilityKey = relative.IsEmpty()
                ? mount.LogicalPrefix
                : mount.LogicalPrefix + TEXT("/") + relative;
            record.PortabilityKey = record.PortabilityKey.ToLower();
        }
    }

    void AppendScanResult(AssetDatabaseScanResult& result, const AssetDatabaseScanResult& mountResult)
    {
        result.FilesExamined += mountResult.FilesExamined;
        result.SidecarsParsed += mountResult.SidecarsParsed;
        result.Cancelled |= mountResult.Cancelled;
        result.Diagnostics.Add(mountResult.Diagnostics);
        result.FileStates.Add(mountResult.FileStates);
    }

    bool CollectMount(const AssetMount& mount, const AssetDatabaseScanOptions& options,
        const AssetDatabaseSnapshot& previous, Array<AssetRecord>& records, AssetDatabaseScanResult& result)
    {
        Array<AssetRecord> mountRecords;
        AssetDatabaseScanResult mountResult;
        bool failed;
        if (options.AssetSystemVersion >= 3 && IsManifestBackedEngineContentMount(mount))
        {
            failed = EngineContentCatalog::Collect(mount.PhysicalRoot, mountRecords, mountResult.Diagnostics);
            mountResult.FilesExamined = mountRecords.Count();
        }
        else
        {
            failed = AssetDatabaseScanner::Collect(GetMountProjectRoot(mount), mount.PhysicalRoot,
                AssetPipelineLibraryFolder(), options, previous, mountRecords, mountResult);
        }
        QualifyMountRecords(mount, mountRecords);
        records.Add(MoveTemp(mountRecords));
        AppendScanResult(result, mountResult);
        return failed;
    }

    bool CollectMountFiles(const AssetMount& mount, const Array<String>& files, const AssetDatabaseScanOptions& options,
        const AssetDatabaseSnapshot& previous, Array<AssetRecord>& records, AssetDatabaseScanResult& result)
    {
        Array<AssetRecord> mountRecords;
        AssetDatabaseScanResult mountResult;
        bool failed;
        if (options.AssetSystemVersion >= 3 && IsManifestBackedEngineContentMount(mount))
        {
            failed = EngineContentCatalog::Collect(mount.PhysicalRoot, mountRecords, mountResult.Diagnostics);
            mountResult.FilesExamined = mountRecords.Count();
        }
        else
        {
            failed = AssetDatabaseScanner::CollectFromFiles(GetMountProjectRoot(mount), mount.PhysicalRoot,
                AssetPipelineLibraryFolder(), files, options, previous, mountRecords, mountResult);
        }
        QualifyMountRecords(mount, mountRecords);
        records.Add(MoveTemp(mountRecords));
        AppendScanResult(result, mountResult);
        return failed;
    }

    String SnapshotDirectory()
    {
#if USE_EDITOR
        return AssetPipelineLibraryFolder() / TEXT("AssetDatabase");
#else
        return String::Empty;
#endif
    }

    String DatabasePath()
    {
        return SnapshotDirectory() / TEXT("AssetDatabase.sqlite");
    }

    void OnDatabaseChanged(const AssetDatabaseChangeBatch& change)
    {
        {
            ScopeLock lock(StateLocker);
            LastChange = AssetDatabaseChangeInfo();
            LastChange.Revision = change.Revision;
            LastChange.Added = change.Added;
            LastChange.Removed = change.Removed;
            LastChange.Changed = change.Changed;
            LastChange.StatusChanged = change.StatusChanged;
        }
        AssetDatabaseFacade::DatabaseChanged(change.Revision);
    }

    void EnsureBound()
    {
        if (!IsBound)
        {
            AssetDatabase::Get().Changed.BindUnique<OnDatabaseChanged>();
            IsBound = true;
        }
    }

    AssetDatabaseRecordInfo ToInfo(const AssetRecord& record)
    {
        AssetDatabaseRecordInfo result;
        result.ID = record.ID;
        result.ObjectID = record.GetObjectId();
        result.BackingAssetID = record.ID;
        result.SourceAssetID = record.SourceAssetID;
        result.LocalId = record.LocalId;
        result.TypeName = record.TypeName;
        result.CanonicalPath = record.CanonicalPath.Get();
        result.SourcePath = record.SourcePath.Get();
        result.MetaPath = record.MetaPath.Get();
        result.SubAssetKey = record.SubAsset.Get();
        result.ProcessorID = record.ProcessorID;
        result.MetaSemanticHash = record.MetaSemanticHash;
        for (int32 i = 0; i < record.Labels.Count(); i++)
        {
            if (i != 0)
                result.LabelsSerialized += TEXT("\n");
            result.LabelsSerialized += record.Labels[i];
        }
        result.SourceKind = record.SourceKind;
        result.Status = record.Status;
        result.Revision = record.DatabaseRevision;
        result.IsMain = record.IsMainAsset();
        return result;
    }

    AssetMutationService& GetMutationService()
    {
        static AssetMutationService service(Globals::ProjectFolder, Globals::ProjectContentFolder,
            AssetPipelineLibraryFolder() / TEXT("AssetDatabase/MutationJournals"),
            AssetPipelineLibraryFolder() / TEXT("AssetDatabase/Recovery"));
        static bool databaseCommitBound = false;
        if (!databaseCommitBound)
        {
            service.DatabaseCommitHook = [](const AssetMutationResult& pending)
            {
                return AssetDatabaseFacade::RefreshSources(pending.ChangedPaths);
            };
            databaseCommitBound = true;
        }
        return service;
    }

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    typedef rapidjson_flax::Document AuthoredJsonDocument;
    typedef rapidjson_flax::Value AuthoredJsonValue;

    StringAnsi AuthoredGuidText(const Guid& id)
    {
        return StringAnsi(id.ToString(Guid::FormatType::N)).ToLower();
    }

    bool FailAuthoredSerialization(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = TEXT("Flax.AuthoredDocument");
        diagnostic.Message = message;
        return true;
    }

    bool WriteAuthoredJson(const AuthoredJsonDocument& document, const Array<StringAnsi>& order,
        StringAnsi& source, AssetPipelineDiagnostic& diagnostic)
    {
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(document, source, error, &order))
            return FailAuthoredSerialization(diagnostic, error.Message);
        return false;
    }

    StringAnsi EncodeAuthoredHex(const Span<byte>& bytes)
    {
        static const char digits[] = "0123456789abcdef";
        StringAnsi result;
        result.Resize(bytes.Length() * 2);
        for (int32 i = 0; i < bytes.Length(); i++)
        {
            result[i * 2] = digits[bytes[i] >> 4];
            result[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return result;
    }

    bool WriteRuntimePayloadSource(const StringView& typeName, const Span<byte>& chunk, const Array<Guid>& references,
        StringAnsi& source, AssetPipelineDiagnostic& diagnostic)
    {
        if (chunk.Length() == 0)
            return FailAuthoredSerialization(diagnostic, TEXT("Authored runtime payload is empty."));
        AuthoredJsonDocument document;
        document.SetObject();
        auto& allocator = document.GetAllocator();
        const StringAnsi type(typeName);
        const StringAnsi hex = EncodeAuthoredHex(chunk);
        document.AddMember("documentVersion", 1, allocator);
        document.AddMember("type", AuthoredJsonValue(type.Get(), type.Length(), allocator), allocator);
        document.AddMember("payloadEncoding", AuthoredJsonValue("hex", allocator), allocator);
        document.AddMember("runtimeChunk", AuthoredJsonValue(hex.Get(), hex.Length(), allocator), allocator);
        Array<StringAnsi> sortedReferences;
        for (const Guid& reference : references)
        {
            if (reference.IsValid())
                sortedReferences.Add(AuthoredGuidText(reference));
        }
        if (sortedReferences.Count() > 1)
        {
            std::sort(sortedReferences.Get(), sortedReferences.Get() + sortedReferences.Count());
            for (int32 i = sortedReferences.Count() - 1; i > 0; i--)
            {
                if (sortedReferences[i] == sortedReferences[i - 1])
                    sortedReferences.RemoveAt(i);
            }
        }
        AuthoredJsonValue referenceValues(rapidjson::kArrayType);
        for (const StringAnsi& reference : sortedReferences)
            referenceValues.PushBack(AuthoredJsonValue(reference.Get(), reference.Length(), allocator), allocator);
        document.AddMember("references", referenceValues, allocator);
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        order.Add("payloadEncoding");
        order.Add("runtimeChunk");
        order.Add("references");
        return WriteAuthoredJson(document, order, source, diagnostic);
    }

    bool WriteSemanticAuthoredSource(AuthoredJsonDocument& document, const StringView& typeName,
        StringAnsi& source, AssetPipelineDiagnostic& diagnostic)
    {
        Array<StringAnsi> order;
        order.Add("documentVersion");
        order.Add("type");
        if (typeName == MaterialInstance::TypeName)
        {
            order.Add("baseMaterial");
            order.Add("overrides");
        }
        else if (typeName == SkeletonMask::TypeName)
        {
            order.Add("skeleton");
            order.Add("maskedNodes");
        }
        else if (typeName == SceneAnimation::TypeName)
        {
            order.Add("framesPerSecond");
            order.Add("durationFrames");
            order.Add("tracks");
        }
        else if (typeName == ParticleSystem::TypeName)
        {
            order.Add("framesPerSecond");
            order.Add("durationFrames");
            order.Add("tracks");
            order.Add("parameterOverrides");
        }
        else if (typeName == CollisionData::TypeName)
        {
            order.Add("collisionType");
            order.Add("sourceModel");
            order.Add("modelLodIndex");
            order.Add("materialSlotsMask");
            order.Add("convexFlags");
            order.Add("convexVertexLimit");
        }
        return WriteAuthoredJson(document, order, source, diagnostic);
    }

    bool DecodeMaterialInstanceSource(MaterialInstance* asset, AuthoredJsonDocument& document, String& error)
    {
        MemoryWriteStream stream(512);
        const Guid baseMaterial = asset->GetBaseMaterial() ? asset->GetBaseMaterial()->GetID() : Guid::Empty;
        stream.Write(baseMaterial);
        asset->Params.Save(&stream);
        return MaterialInstanceDocument::DecodeRuntime(ToSpan(stream), document, error);
    }

    void DecodeSkeletonMaskSource(SkeletonMask* asset, AuthoredJsonDocument& document)
    {
        document.SetObject();
        auto& allocator = document.GetAllocator();
        const StringAnsi type(SkeletonMask::TypeName);
        const StringAnsi skeleton = AuthoredGuidText(asset->Skeleton.GetID());
        document.AddMember("documentVersion", 1, allocator);
        document.AddMember("type", AuthoredJsonValue(type.Get(), type.Length(), allocator), allocator);
        document.AddMember("skeleton", AuthoredJsonValue(skeleton.Get(), skeleton.Length(), allocator), allocator);
        Array<String> names = asset->GetMaskedNodes();
        if (names.Count() > 1)
            std::sort(names.Get(), names.Get() + names.Count());
        AuthoredJsonValue nodes(rapidjson::kArrayType);
        for (const String& name : names)
        {
            const StringAnsi value(name);
            nodes.PushBack(AuthoredJsonValue(value.Get(), value.Length(), allocator), allocator);
        }
        document.AddMember("maskedNodes", nodes, allocator);
    }

    void SerializeAnimationChunk(Animation* asset, MemoryWriteStream& stream)
    {
        stream.Write(103);
        stream.Write(asset->Data.Duration);
        stream.Write(asset->Data.FramesPerSecond);
        stream.Write(static_cast<byte>(asset->Data.RootMotionFlags));
        stream.Write(asset->Data.RootNodeName, 13);
        stream.WriteInt32(asset->Data.Channels.Count());
        for (auto& animation : asset->Data.Channels)
        {
            stream.Write(animation.NodeName, 172);
            Serialization::Serialize(stream, animation.Position);
            Serialization::Serialize(stream, animation.Rotation);
            Serialization::Serialize(stream, animation.Scale);
        }
        stream.WriteInt32(asset->Events.Count());
        for (auto& eventTrack : asset->Events)
        {
            stream.Write(eventTrack.First, 172);
            stream.Write(eventTrack.Second.GetKeyframes().Count());
            for (const auto& keyframe : eventTrack.Second.GetKeyframes())
            {
                stream.Write(keyframe.Time);
                stream.Write(keyframe.Value.Duration);
                stream.Write(keyframe.Value.TypeName, 17);
                stream.WriteJson(keyframe.Value.Instance);
            }
        }
        stream.WriteInt32(asset->NestedAnims.Count());
        for (auto& nestedTrack : asset->NestedAnims)
        {
            stream.Write(nestedTrack.First, 172);
            auto& animation = nestedTrack.Second;
            byte flags = 0;
            if (animation.Enabled)
                flags |= 1;
            if (animation.Loop)
                flags |= 2;
            stream.Write(flags);
            stream.Write(animation.Anim.GetID());
            stream.Write(animation.Time);
            stream.Write(animation.Duration);
            stream.Write(animation.Speed);
            stream.Write(animation.StartTime);
        }
    }

    void SerializeGameplayGlobalsChunk(GameplayGlobals* asset, MemoryWriteStream& stream)
    {
        stream.Write(asset->Variables.Count());
        Array<String> names;
        names.EnsureCapacity(asset->Variables.Count());
        for (const auto& entry : asset->Variables)
            names.Add(entry.Key);
        if (names.Count() > 1)
            std::sort(names.Get(), names.Get() + names.Count());
        for (const String& name : names)
        {
            const auto* variable = asset->Variables.TryGet(name);
            stream.Write(name, 71);
            stream.Write(variable->DefaultValue);
        }
    }
#endif

    bool ReplaceMetadataTransactional(const StringView& sourcePath, const AssetMeta& meta, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> source;
        if (File::ReadAllBytes(sourcePath, source))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Source bytes could not be staged for an atomic metadata update.");
            return true;
        }
        AssetMutationResult mutation;
        if (GetMutationService().ReplaceAsset(sourcePath,
            StringAnsiView(reinterpret_cast<const char*>(source.Get()), source.Count()), meta, mutation))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.SourcePath = sourcePath;
            diagnostic.AssetGuid = meta.ID;
            diagnostic.Message = mutation.Message;
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    AssetMutationResultInfo ToInfo(const AssetMutationResult& result)
    {
        AssetMutationResultInfo info;
        info.Succeeded = result.Succeeded;
        info.RequiresRecovery = result.RequiresRecovery;
        info.TransactionID = result.TransactionID;
        info.AssetID = result.AssetID;
        info.SourcePath = result.SourcePath;
        info.DestinationPath = result.DestinationPath;
        info.RecoveryPath = result.RecoveryPath;
        info.Message = result.Message;
        return info;
    }

    bool FileStatesStillMatch(const Array<AssetDatabaseFileState>& states)
    {
        Dictionary<String, const AssetDatabaseFileState*> byPath;
        for (const AssetDatabaseFileState& state : states)
            byPath.Add(state.Path.ToLower(), &state);
        int32 matched = 0;
        const auto checkRoot = [&byPath, &matched](const StringView& root)
        {
            if (!FileSystem::DirectoryExists(root))
                return false;
            Array<String> files;
            if (FileSystem::DirectoryGetFiles(files, String(root), TEXT("*"), DirectorySearchOption::AllDirectories))
                return false;
            for (const String& file : files)
            {
                String normalized = file.ToLower();
                normalized.Replace(TEXT('\\'), TEXT('/'));
                if (normalized.Contains(TEXT("/cache/")) || normalized.Contains(TEXT("/output/")) || normalized.Contains(TEXT("/generated/")) ||
                    normalized.Contains(TEXT("/migrationbackup/")) || normalized.Contains(TEXT("/.asset-pipeline/")) || normalized.Contains(TEXT("/.git/")))
                    continue;
                const AssetDatabaseFileState* const* state = byPath.TryGet(file.ToLower());
                if (!state || !SourceHashCache::IsStateCurrent(**state))
                    return false;
                matched++;
            }
            return true;
        };
        const Array<AssetMount> mounts = AssetMountRegistry::GetMounts();
        for (const AssetMount& mount : mounts)
        {
            if (ConfiguredAssetSystemVersion >= 3 && IsManifestBackedEngineContentMount(mount))
                continue;
            if (!checkRoot(mount.PhysicalRoot))
                return false;
        }
        return matched == states.Count();
    }

    void SetDiagnostics(const Array<AssetPipelineDiagnostic>& diagnostics)
    {
        ScopeLock lock(StateLocker);
        LastDiagnostics = diagnostics;
    }

    String NormalizeAbsolutePath(const StringView& path)
    {
        String result(path);
        if (result.IsEmpty())
            return result;
        FileSystem::NormalizePath(result);
        return result;
    }

    String ResolveFacadeAssetPath(const StringView& path)
    {
        AssetMountResolution resolution;
        AssetPipelineDiagnostic diagnostic;
        String value(path);
        value.Replace('\\', '/');
        if (FileSystem::IsRelative(value))
            return AssetMountRegistry::Get().ResolveLogical(value, resolution, diagnostic) ? String::Empty : resolution.PhysicalPath;
        FileSystem::NormalizePath(value);
        return AssetMountRegistry::Get().ResolvePhysical(value, resolution, diagnostic) ? String::Empty : resolution.PhysicalPath;
    }

    String ToLogicalAssetPath(const StringView& path)
    {
        AssetMountResolution resolution;
        AssetPipelineDiagnostic diagnostic;
        return AssetMountRegistry::Get().ResolvePhysical(path, resolution, diagnostic) ? String::Empty : resolution.LogicalPath;
    }

    bool IsFacadeAssetPath(const StringView& path)
    {
        AssetMountResolution resolution;
        AssetPipelineDiagnostic diagnostic;
        return !AssetMountRegistry::Get().ResolvePhysical(path, resolution, diagnostic);
    }

    String PathKey(const StringView& path)
    {
        String result = NormalizeAbsolutePath(path);
        result.Replace(TEXT('\\'), TEXT('/'));
        return result.ToLower();
    }

    bool IsMetaPath(const StringView& path)
    {
        return path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
    }

    bool IsV3MetadataExcluded(const StringView& path)
    {
        String normalized(path);
        FileSystem::NormalizePath(normalized);
        normalized = normalized.ToLower();
        String root(Globals::ProjectContentFolder);
        FileSystem::NormalizePath(root);
        root = root.ToLower();
        String relative = normalized.StartsWith(root) ? normalized.Substring(root.Length()) : normalized;
        relative = TEXT("/") + relative + TEXT("/");
        const Char* excluded[] =
        {
            TEXT("/library/"), TEXT("/cache/"), TEXT("/output/"), TEXT("/generated/"),
            TEXT("/migrationbackup/"), TEXT("/.asset-pipeline/"), TEXT("/.git/")
        };
        for (const Char* segment : excluded)
        {
            if (relative.Contains(segment))
                return true;
        }
        return false;
    }

    void AddUniquePath(const StringView& path, HashSet<String>& keys, Array<String>& expanded)
    {
        const String normalized = NormalizeAbsolutePath(path);
        if (normalized.IsEmpty())
            return;
        const String key = PathKey(normalized);
        if (!keys.Add(key))
            return;
        expanded.Add(normalized);
    }

    void ExpandRefreshPath(const StringView& path, HashSet<String>& keys, Array<String>& expanded)
    {
        const String normalized = NormalizeAbsolutePath(path);
        if (normalized.IsEmpty())
            return;
        if (FileSystem::DirectoryExists(normalized))
        {
            Array<String> nested;
            if (!FileSystem::DirectoryGetFiles(nested, normalized, TEXT("*"), DirectorySearchOption::AllDirectories))
            {
                for (const String& nestedPath : nested)
                    AddUniquePath(nestedPath, keys, expanded);
            }
            return;
        }
        AddUniquePath(normalized, keys, expanded);
        if (IsMetaPath(normalized))
            AddUniquePath(normalized.Left(normalized.Length() - 5), keys, expanded);
        else
            AddUniquePath(normalized + TEXT(".meta"), keys, expanded);
    }

    bool RecordPathAffected(const AssetRecord& record, const HashSet<String>& keys)
    {
        return keys.Contains(PathKey(record.SourcePath.Get())) ||
            (!record.MetaPath.Get().IsEmpty() && keys.Contains(PathKey(record.MetaPath.Get())));
    }

    bool PathIsUnder(const StringView& path, const StringView& root)
    {
        if (path.IsEmpty() || root.IsEmpty())
            return false;
        const String pathKey = PathKey(path);
        const String rootKey = PathKey(root);
        if (pathKey == rootKey)
            return true;
        return pathKey.Length() > rootKey.Length() && pathKey.StartsWith(rootKey) && pathKey[rootKey.Length()] == '/';
    }

    // Both arguments must already be PathKey-normalized.
    bool IsKeyUnderAnyRoot(const String& pathKey, const Array<String>& rootKeys)
    {
        for (const String& rootKey : rootKeys)
        {
            if (pathKey == rootKey)
                return true;
            if (pathKey.Length() > rootKey.Length() && pathKey.StartsWith(rootKey) && pathKey[rootKey.Length()] == '/')
                return true;
        }
        return false;
    }

    // A scoped refresh only knows about the paths it was given, so diagnostics for every other
    // source must survive. Unattributable entries are dropped so they cannot accumulate forever.
    void MergeScopedDiagnostics(const HashSet<String>& affectedKeys, const Array<AssetPipelineDiagnostic>& fresh)
    {
        Array<AssetPipelineDiagnostic> merged;
        {
            ScopeLock lock(StateLocker);
            merged.EnsureCapacity(LastDiagnostics.Count() + fresh.Count());
            for (const AssetPipelineDiagnostic& diagnostic : LastDiagnostics)
            {
                if (diagnostic.SourcePath.IsEmpty() || affectedKeys.Contains(PathKey(diagnostic.SourcePath)))
                    continue;
                merged.Add(diagnostic);
            }
        }
        merged.Add(fresh);
        SetDiagnostics(merged);
    }

    bool SnapshotIdentityChanged(const AssetDatabaseSnapshot& previous, const Array<AssetRecord>& merged)
    {
        if (previous.Records.Count() != merged.Count())
            return true;
        Dictionary<Guid, const AssetRecord*> previousById;
        for (const AssetRecord& record : previous.Records)
            previousById.Add(record.ID, &record);
        for (const AssetRecord& record : merged)
        {
            const AssetRecord* const* previousRecord = previousById.TryGet(record.ID);
            if (!previousRecord || !(*previousRecord)->HasSameIdentityAndContent(record) || (*previousRecord)->Status != record.Status)
                return true;
        }
        return false;
    }

    bool PersistSnapshot()
    {
        const String directory = SnapshotDirectory();
        if (!FileSystem::DirectoryExists(directory))
            FileSystem::CreateDirectory(directory);
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseStorage::Save(DatabasePath(), Globals::ProjectFolder, Globals::ProjectContentFolder,
            AssetDatabase::Get().GetSnapshot(), LastFileStates, diagnostic))
        {
            ScopeLock lock(StateLocker);
            LastDiagnostics.Add(diagnostic);
            return true;
        }
        return false;
    }

    bool RefreshPath(const StringView& path)
    {
        Array<String> paths;
        paths.Add(String(path));
        return AssetDatabaseFacade::RefreshSources(paths);
    }

    bool RefreshPath(const String& path)
    {
        return RefreshPath(StringView(path));
    }

    // Migration is driven one asset at a time, and every conversion invalidates the persisted file
    // states, so revalidating the whole snapshot per call is quadratic. Load once, then keep the
    // database current through scoped refreshes.
    bool EnsureDatabaseLoaded()
    {
        if (AssetDatabase::Get().GetRevision() != 0)
            return false;
        return AssetDatabaseFacade::LoadOrScan(false);
    }

#if USE_EDITOR
    enum class CanonicalBatchBuildKind : byte
    {
        None,
        Texture,
        Model,
        Imported,
    };

    struct CanonicalBatchWork
    {
        String SourcePath;
        String StagingPath;
        AssetMeta Meta;
        AssetPipelineDiagnostic Diagnostic;
        CanonicalBatchBuildKind BuildKind = CanonicalBatchBuildKind::None;
        bool AllowExistingStaging = false;
        bool Failed = false;
    };

    bool FailCanonicalBatchWork(CanonicalBatchWork& work, AssetPipelineDiagnosticCode code, const StringView& message)
    {
        work.Diagnostic = AssetPipelineDiagnostic();
        work.Diagnostic.Code = code;
        work.Diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        work.Diagnostic.SourcePath = work.SourcePath;
        work.Diagnostic.Message = message;
        return true;
    }

    bool PrepareDefaultCanonicalMetadata(CanonicalBatchWork& work)
    {
        const bool isFolder = FileSystem::DirectoryExists(work.SourcePath);
        if (!isFolder && !FileSystem::FileExists(work.SourcePath))
            return FailCanonicalBatchWork(work, AssetPipelineDiagnosticCode::SourceMissing, TEXT("Canonical source does not exist."));
        if (!work.AllowExistingStaging && FileSystem::FileExists(work.StagingPath))
            return FailCanonicalBatchWork(work, AssetPipelineDiagnosticCode::PathCollision, TEXT("Canonical metadata staging path already exists."));

        const String extension = FileSystem::GetExtension(work.SourcePath).ToLower();
        AssetMeta& meta = work.Meta;
        meta.ID = Guid::New();
        meta.FolderAsset = isFolder;
        if (isFolder)
        {
            meta.AssetType = TEXT("FlaxEngine.Folder");
            meta.SourceKind = AssetSourceKind::Folder;
            meta.Processor.ID = TEXT("Flax.Folder");
            meta.Processor.SettingsVersion = 1;
            meta.Processor.SettingsJson = "{}\n";
            return false;
        }
        if (JsonStorageProxy::IsValidExtension(extension))
        {
            Guid sourceHeaderID;
            if (!JsonStorageProxy::GetAssetInfo(work.SourcePath, sourceHeaderID, meta.AssetType) ||
                !sourceHeaderID.IsValid() || meta.AssetType.IsEmpty())
            {
                return FailCanonicalBatchWork(work, AssetPipelineDiagnosticCode::InvalidMeta,
                    TEXT("Authored JSON source is missing a valid ID and TypeName header."));
            }
            meta.SourceKind = AssetSourceKind::ExistingJson;
            meta.Processor.ID = TEXT("Flax.ExistingJson");
            meta.Processor.SettingsVersion = 1;
            meta.Processor.SettingsJson = "{}\n";
            work.BuildKind = CanonicalBatchBuildKind::Imported;
            return false;
        }
        meta.SourceKind = AssetSourceKind::ImportedSource;

#if COMPILE_WITH_TEXTURE_TOOL
        const bool isTexture = extension == TEXT("png") || extension == TEXT("tga") || extension == TEXT("exr") ||
            extension == TEXT("bmp") || extension == TEXT("gif") || extension == TEXT("tiff") || extension == TEXT("tif") ||
            extension == TEXT("jpeg") || extension == TEXT("jpg") || extension == TEXT("dds") || extension == TEXT("hdr") ||
            extension == TEXT("raw");
        if (isTexture)
        {
            TextureTool::Options options;
            TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
            if (settings.Validate(work.Diagnostic))
                return true;
            meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
            if (!options.IsAtlas)
            {
                TextureData sourceData;
                if (!TextureTool::ImportTexture(work.SourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
                    meta.AssetType = CubeTexture::TypeName;
            }
            meta.Processor.ID = TextureProcessorSettings::ProcessorID();
            meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
            work.BuildKind = CanonicalBatchBuildKind::Texture;
            return settings.ToJson(meta.Processor.SettingsJson, work.Diagnostic);
        }
#endif

#if COMPILE_WITH_MODEL_TOOL
        const bool isModel = extension == TEXT("obj") || extension == TEXT("fbx") || extension == TEXT("x") ||
            extension == TEXT("dae") || extension == TEXT("gltf") || extension == TEXT("glb") || extension == TEXT("blend") ||
            extension == TEXT("bvh") || extension == TEXT("ase") || extension == TEXT("ply") || extension == TEXT("dxf") ||
            extension == TEXT("ifc") || extension == TEXT("nff") || extension == TEXT("smd") || extension == TEXT("vta") ||
            extension == TEXT("mdl") || extension == TEXT("md2") || extension == TEXT("md3") || extension == TEXT("md5mesh") ||
            extension == TEXT("q3o") || extension == TEXT("q3s") || extension == TEXT("ac") || extension == TEXT("stl") ||
            extension == TEXT("lwo") || extension == TEXT("lws") || extension == TEXT("lxo");
        if (isModel)
        {
            ModelTool::Options options;
            ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
            if (settings.Validate(work.Diagnostic))
                return true;
            ModelSourceAnalysis analysis;
            if (ModelProcessor::AnalyzeSource(work.SourcePath, settings, analysis, work.Diagnostic))
                return true;
            options.Type = analysis.SourceSkeletonBoneCount > 0 || analysis.SourceAnimationCount > 0
                ? ModelTool::ModelType::SkinnedModel
                : ModelTool::ModelType::Model;
            settings = ModelProcessorSettings::FromLegacyOptions(options);
            meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel ? SkinnedModel::TypeName : Model::TypeName;
            meta.Processor.ID = ModelProcessorSettings::ProcessorID();
            meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
            if (settings.ToJson(meta.Processor.SettingsJson, work.Diagnostic))
                return true;
            SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, analysis.Candidates, true);
            if (reconciliation.RequiresUserReconciliation)
            {
                work.Diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : work.Diagnostic;
                work.Diagnostic.AssetGuid = meta.ID;
                work.Diagnostic.SourcePath = work.SourcePath;
                return true;
            }
            meta.SubAssets = MoveTemp(reconciliation.Resolved);
            work.BuildKind = CanonicalBatchBuildKind::Model;
            return false;
        }
#endif

#if COMPILE_WITH_AUDIO_TOOL
        if (extension == TEXT("wav") || extension == TEXT("mp3") || extension == TEXT("ogg"))
        {
            rapidjson_flax::StringBuffer settingsBuffer;
            CompactJsonWriter settingsWriter(settingsBuffer);
            settingsWriter.StartObject();
            AudioTool::Options options;
            options.Serialize(settingsWriter, nullptr);
            settingsWriter.EndObject();
            meta.AssetType = TEXT("FlaxEngine.AudioClip");
            meta.Processor.ID = TEXT("Flax.Audio");
            meta.Processor.SettingsVersion = 1;
            meta.Processor.SettingsJson = StringAnsi(settingsBuffer.GetString(), static_cast<int32>(settingsBuffer.GetSize()));
            work.BuildKind = CanonicalBatchBuildKind::Imported;
            return false;
        }
#endif

        if (extension == TEXT("ttf") || extension == TEXT("otf"))
        {
            meta.AssetType = TEXT("FlaxEngine.FontAsset");
            meta.Processor.ID = TEXT("Flax.Font");
        }
        else if (extension == TEXT("shader"))
        {
            meta.AssetType = TEXT("FlaxEngine.Shader");
            meta.Processor.ID = TEXT("Flax.ShaderSource");
        }
        else if (extension == TEXT("mp4") || extension == TEXT("webm") || extension == TEXT("mov") || extension == TEXT("mkv"))
        {
            meta.AssetType = TEXT("FlaxEngine.Video");
            meta.Processor.ID = TEXT("Flax.Video");
        }
        else if (extension == TEXT("txt"))
        {
            meta.AssetType = RawDataAsset::TypeName;
            meta.SourceKind = AssetSourceKind::TextDocument;
            meta.Processor.ID = TEXT("Flax.Text");
        }
        else
        {
            meta.AssetType = RawDataAsset::TypeName;
            meta.Processor.ID = TEXT("Flax.Unsupported");
            meta.Processor.SettingsVersion = 1;
            meta.Processor.SettingsJson = "{}\n";
            work.BuildKind = CanonicalBatchBuildKind::None;
            return false;
        }
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        work.BuildKind = CanonicalBatchBuildKind::Imported;
        return false;
    }

    bool HasDefaultCanonicalImporter(const StringView& sourcePath)
    {
        const String extension = FileSystem::GetExtension(sourcePath).ToLower();
        return extension == TEXT("png") || extension == TEXT("tga") || extension == TEXT("exr") ||
            extension == TEXT("bmp") || extension == TEXT("gif") || extension == TEXT("tiff") || extension == TEXT("tif") ||
            extension == TEXT("jpeg") || extension == TEXT("jpg") || extension == TEXT("dds") || extension == TEXT("hdr") ||
            extension == TEXT("raw") || extension == TEXT("obj") || extension == TEXT("fbx") || extension == TEXT("x") ||
            extension == TEXT("dae") || extension == TEXT("gltf") || extension == TEXT("glb") || extension == TEXT("blend") ||
            extension == TEXT("bvh") || extension == TEXT("ase") || extension == TEXT("ply") || extension == TEXT("dxf") ||
            extension == TEXT("ifc") || extension == TEXT("nff") || extension == TEXT("smd") || extension == TEXT("vta") ||
            extension == TEXT("mdl") || extension == TEXT("md2") || extension == TEXT("md3") || extension == TEXT("md5mesh") ||
            extension == TEXT("q3o") || extension == TEXT("q3s") || extension == TEXT("ac") || extension == TEXT("stl") ||
            extension == TEXT("lwo") || extension == TEXT("lws") || extension == TEXT("lxo") || extension == TEXT("wav") ||
            extension == TEXT("mp3") || extension == TEXT("ogg") || extension == TEXT("ttf") || extension == TEXT("otf") ||
            extension == TEXT("shader") || extension == TEXT("mp4") || extension == TEXT("webm") || extension == TEXT("mov") ||
            extension == TEXT("mkv") || extension == TEXT("txt");
    }

    bool RegisterExistingMetadata(const StringView& sourcePath, const AssetMeta& metadata, bool replaceExisting,
        AssetPipelineDiagnostic& diagnostic)
    {
        AssetMutationResult result;
        if (!GetMutationService().RegisterExisting(sourcePath, metadata, replaceExisting, result))
            return false;
        diagnostic.Code = result.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = metadata.ID;
        diagnostic.Message = result.Message;
        return true;
    }

    bool EnsureDefaultCanonicalMetadata(const StringView& sourcePath, AssetPipelineDiagnostic& diagnostic)
    {
        const String metaPath = String(sourcePath) + TEXT(".meta");
        const bool isFolder = FileSystem::DirectoryExists(sourcePath);
        const bool isFile = FileSystem::FileExists(sourcePath);
        if (!isFile && !isFolder)
            return false;
        if (FileSystem::FileExists(metaPath))
        {
            if (ConfiguredAssetSystemVersion < 3)
                return false;
            AssetMeta metadata;
            if (AssetMeta::Load(metaPath, metadata, diagnostic))
                return true;
            if (!metadata.MetaUpgradeRequired)
                return false;
            metadata.MetaVersion = AssetMeta::CurrentMetaVersion;
            metadata.MetaUpgradeRequired = false;
            return RegisterExistingMetadata(sourcePath, metadata, true, diagnostic);
        }
        AssetMeta metadata;
        metadata.ID = Guid::New();
        metadata.FolderAsset = isFolder;
        if (isFolder)
        {
            metadata.AssetType = TEXT("FlaxEngine.Folder");
            metadata.SourceKind = AssetSourceKind::Folder;
            metadata.Processor.ID = TEXT("Flax.Folder");
            metadata.Processor.SettingsVersion = 1;
            metadata.Processor.SettingsJson = "{}\n";
            return RegisterExistingMetadata(sourcePath, metadata, false, diagnostic);
        }
        const String extension = FileSystem::GetExtension(sourcePath).ToLower();
        if (extension == TEXT("flax"))
            return false;
        if (JsonStorageProxy::IsValidExtension(extension))
        {
            Guid sourceHeaderID;
            if (!JsonStorageProxy::GetAssetInfo(sourcePath, sourceHeaderID, metadata.AssetType) || !sourceHeaderID.IsValid() || metadata.AssetType.IsEmpty())
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
                diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
                diagnostic.SourcePath = sourcePath;
                diagnostic.Message = TEXT("Authored JSON source is missing a valid ID and TypeName header.");
                return true;
            }
            metadata.SourceKind = AssetSourceKind::ExistingJson;
            metadata.Processor.ID = TEXT("Flax.ExistingJson");
            metadata.Processor.SettingsVersion = 1;
            metadata.Processor.SettingsJson = "{}\n";
            return RegisterExistingMetadata(sourcePath, metadata, false, diagnostic);
        }
        if (!HasDefaultCanonicalImporter(sourcePath))
        {
            metadata.AssetType = RawDataAsset::TypeName;
            metadata.SourceKind = AssetSourceKind::ImportedSource;
            metadata.Processor.ID = TEXT("Flax.Unsupported");
            metadata.Processor.SettingsVersion = 1;
            metadata.Processor.SettingsJson = "{}\n";
            return RegisterExistingMetadata(sourcePath, metadata, false, diagnostic);
        }
        CanonicalBatchWork work;
        work.SourcePath = sourcePath;
        work.StagingPath = metaPath;
        if (PrepareDefaultCanonicalMetadata(work) || RegisterExistingMetadata(sourcePath, work.Meta, false, work.Diagnostic))
        {
            diagnostic = work.Diagnostic;
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool CollectSourceDirectories(const StringView& root, Array<String>& result)
    {
        Array<String> pending;
        pending.Add(String(root));
        for (int32 index = 0; index < pending.Count(); index++)
        {
            Array<String> children;
            if (FileSystem::GetChildDirectories(children, pending[index]))
                return true;
            for (String& child : children)
            {
                result.Add(child);
                pending.Add(MoveTemp(child));
            }
        }
        return false;
    }

#endif

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    enum class GenericBuildRequestResult : byte
    {
        Unsupported,
        Queued,
        Failed,
    };

    bool WaitForGenericBuild(const Guid& assetID,
        AssetBuildJobStatus (*getStatus)(const Guid&, AssetPipelineDiagnostic&), AssetPipelineDiagnostic& diagnostic)
    {
        for (;;)
        {
            const AssetBuildJobStatus status = getStatus(assetID, diagnostic);
            if (status == AssetBuildJobStatus::Succeeded)
                return false;
            if (status == AssetBuildJobStatus::Failed || status == AssetBuildJobStatus::Cancelled || status == AssetBuildJobStatus::Invalid)
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                    diagnostic.AssetGuid = assetID;
                    diagnostic.Message = TEXT("Synchronous asset import did not complete successfully.");
                }
                return true;
            }
            Platform::Sleep(1);
        }
    }

    GenericBuildRequestResult RequestGenericBuild(const AssetRecord& record, bool force, bool synchronous,
        AssetPipelineDiagnostic& diagnostic)
    {
#if COMPILE_WITH_TEXTURE_TOOL
        if (record.ProcessorID == TextureProcessorSettings::ProcessorID())
        {
            if (TexturePipelineService::RequestBuild(record.ID, force, diagnostic) ||
                (synchronous && WaitForGenericBuild(record.ID, TexturePipelineService::GetStatus, diagnostic)))
                return GenericBuildRequestResult::Failed;
            return GenericBuildRequestResult::Queued;
        }
#endif
#if COMPILE_WITH_MODEL_TOOL
        if (record.ProcessorID == ModelProcessorSettings::ProcessorID())
        {
            if (ModelPipelineService::RequestBuild(record.ID, force, diagnostic) ||
                (synchronous && WaitForGenericBuild(record.ID, ModelPipelineService::GetStatus, diagnostic)))
                return GenericBuildRequestResult::Failed;
            return GenericBuildRequestResult::Queued;
        }
#endif
        if (GraphPipelineService::OwnsProcessor(record.ProcessorID))
        {
            const bool failed = synchronous
                ? GraphPipelineService::RequestBuildAndWait(record.ID, force, diagnostic)
                : GraphPipelineService::RequestBuild(record.ID, force, diagnostic);
            return failed ? GenericBuildRequestResult::Failed : GenericBuildRequestResult::Queued;
        }
        diagnostic = AssetPipelineDiagnostic();
        return GenericBuildRequestResult::Unsupported;
    }
#endif

    bool RejectPostCutoverLegacyMutation(const StringView& sourcePath, AssetPipelineDiagnostic& diagnostic)
    {
        if (!AssetDatabase::Get().IsHardCutEnabled())
            return false;
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Legacy migration filesystem mutations are private to the pre-cutover migration phase.");
        return true;
    }

    bool StageImportedFiles(const StringView& legacyPath, const StringView& extractedPath, const StringView& destinationPath,
        const StringView& backupPath, const AssetMeta& meta, AssetPipelineDiagnostic& diagnostic)
    {
        if (RejectPostCutoverLegacyMutation(legacyPath, diagnostic))
            return true;
        const String destinationMeta = String(destinationPath) + TEXT(".meta");
        if (!FileSystem::FileExists(legacyPath) || !FileSystem::FileExists(extractedPath) ||
            FileSystem::FileExists(destinationPath) || FileSystem::FileExists(destinationMeta) || FileSystem::FileExists(backupPath))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = legacyPath;
            diagnostic.Message = TEXT("Imported migration paths are missing or collide with existing files.");
            return true;
        }
        const String destinationFolder(StringUtils::GetDirectoryName(destinationPath));
        const String backupFolder(StringUtils::GetDirectoryName(backupPath));
        if ((!FileSystem::DirectoryExists(destinationFolder) && FileSystem::CreateDirectory(destinationFolder)) ||
            (!FileSystem::DirectoryExists(backupFolder) && FileSystem::CreateDirectory(backupFolder)) ||
            FileSystem::CopyFile(destinationPath, extractedPath) || AssetMeta::SaveAtomic(destinationMeta, meta, diagnostic))
        {
            FileSystem::DeleteFile(destinationMeta);
            FileSystem::DeleteFile(destinationPath);
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
                diagnostic.SourcePath = destinationPath;
                diagnostic.Message = TEXT("Canonical imported source staging failed.");
            }
            return true;
        }
        ContentStorageManager::EnsureAccess(legacyPath);
        if (FileSystem::MoveFile(backupPath, legacyPath, false))
        {
            FileSystem::DeleteFile(destinationMeta);
            FileSystem::DeleteFile(destinationPath);
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = legacyPath;
            diagnostic.Message = TEXT("Legacy imported asset could not be moved into reversible staging.");
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

uint64 AssetDatabaseFacade::GetRevision()
{
    return AssetDatabase::Get().GetRevision();
}

void AssetDatabaseFacade::SetMutationDecisionHook(AssetMutationDecisionHook&& hook)
{
    GetMutationService().DecisionHook = MoveTemp(hook);
}

int32 AssetDatabaseFacade::GetDesiredWorkerCount()
{
    return ConfiguredWorkerLimit;
}

void AssetDatabaseFacade::SetDesiredWorkerCount(int32 value)
{
    if (value < 1 || value > 64)
        return;
    ConfiguredWorkerLimit = value;
#if COMPILE_WITH_TEXTURE_TOOL && COMPILE_WITH_ASSETS_IMPORTER
    TexturePipelineService::SetMaximumWorkers(value);
#endif
}

int32 AssetDatabaseFacade::GetConfiguredMemoryLimitMegabytes()
{
    return ConfiguredMemoryLimitMegabytes;
}

Array<AssetDatabaseRecordInfo> AssetDatabaseFacade::GetRecords()
{
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    Array<AssetDatabaseRecordInfo> result;
    result.EnsureCapacity(snapshot.Records.Count());
    for (const AssetRecord& record : snapshot.Records)
        result.Add(ToInfo(record));
    if (result.Count() > 1)
    {
        std::sort(result.Get(), result.Get() + result.Count(), [](const AssetDatabaseRecordInfo& a, const AssetDatabaseRecordInfo& b)
        {
            if (a.CanonicalPath != b.CanonicalPath)
                return a.CanonicalPath < b.CanonicalPath;
            return a.SubAssetKey < b.SubAssetKey;
        });
    }
    return result;
}

Array<AssetPipelineDiagnostic> AssetDatabaseFacade::GetDiagnostics()
{
    ScopeLock lock(StateLocker);
    return LastDiagnostics;
}

AssetDatabaseChangeInfo AssetDatabaseFacade::GetLastChange()
{
    ScopeLock lock(StateLocker);
    return LastChange;
}

Guid AssetDatabaseFacade::AssetPathToGUID(const StringView& path)
{
    if (path.IsEmpty() || EnsureDatabaseLoaded())
        return Guid::Empty;
    const String resolved = ResolveFacadeAssetPath(path);
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.IsMainAsset() && FileSystem::AreFilePathsEqual(record.CanonicalPath.Get(), resolved))
            return record.ID;
    }
    return Guid::Empty;
}

String AssetDatabaseFacade::GUIDToAssetPath(const Guid& assetID)
{
    if (!assetID.IsValid() || EnsureDatabaseLoaded())
        return String::Empty;
    AssetRecord record;
    return AssetDatabase::Get().TryGetRecord(assetID, record) && record.IsMainAsset() && record.SourceAssetID == assetID
        ? ToLogicalAssetPath(record.CanonicalPath.Get())
        : String::Empty;
}

Array<String> AssetDatabaseFacade::GetAllAssetPaths()
{
    Array<String> result;
    if (EnsureDatabaseLoaded())
        return result;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.IsMainAsset())
            result.Add(ToLogicalAssetPath(record.CanonicalPath.Get()));
    }
    if (result.Count() > 1)
        std::sort(result.Get(), result.Get() + result.Count());
    return result;
}

Array<String> AssetDatabaseFacade::GetDependencies(const Guid& assetID, bool recursive)
{
    Array<String> result;
    if (!assetID.IsValid() || EnsureDatabaseLoaded())
        return result;
    AssetRecord root;
    if (!AssetDatabase::Get().TryGetRecord(assetID, root))
        return result;
    Array<Guid> pending = root.BuildInputDependencies;
    HashSet<Guid> visited;
    for (int32 index = 0; index < pending.Count(); index++)
    {
        const Guid dependencyID = pending[index];
        if (!visited.Add(dependencyID))
            continue;
        AssetRecord dependency;
        if (!AssetDatabase::Get().TryGetRecord(dependencyID, dependency))
            continue;
        const String path = ToLogicalAssetPath(dependency.CanonicalPath.Get());
        if (!result.Contains(path))
            result.Add(path);
        if (recursive)
        {
            for (const Guid& transitive : dependency.BuildInputDependencies)
                pending.Add(transitive);
        }
    }
    if (result.Count() > 1)
        std::sort(result.Get(), result.Get() + result.Count());
    return result;
}

Guid AssetDatabaseFacade::GetDependencyHash(const Guid& assetID)
{
    if (!assetID.IsValid() || EnsureDatabaseLoaded())
        return Guid::Empty;
    AssetRecord root;
    if (!AssetDatabase::Get().TryGetRecord(assetID, root))
        return Guid::Empty;
    ArtifactKeyBuilder builder(StringAnsiView("flax-managed-dependency-closure-v1"));
    builder.AddGuid(StringAnsiView("root"), root.SourceAssetID);
    builder.AddUInt64(StringAnsiView("root-meta"), root.MetaSemanticHash);
    const Array<String> dependencies = GetDependencies(assetID, true);
    Array<StringAnsi> dependencyPaths;
    dependencyPaths.EnsureCapacity(dependencies.Count());
    for (const String& path : dependencies)
        dependencyPaths.Add(StringAnsi(path));
    builder.AddSortedStrings(StringAnsiView("paths"), dependencyPaths);
    const ContentHash hash = builder.Finalize().Digest;
    return Guid(hash.Values[0], hash.Values[1], hash.Values[2], hash.Values[3]);
}

bool AssetDatabaseFacade::SetLabels(const Guid& assetID, const Array<String>& labels, uint64 expectedRevision)
{
#if USE_EDITOR
    if (!assetID.IsValid() || EnsureDatabaseLoaded())
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset() ||
        (expectedRevision != 0 && record.DatabaseRevision != expectedRevision))
        return true;
    Array<String> normalized = labels;
    if (normalized.Count() > 1)
        std::sort(normalized.Get(), normalized.Get() + normalized.Count());
    for (int32 i = 0; i < normalized.Count(); i++)
    {
        if (normalized[i].IsEmpty() || normalized[i].Contains(TEXT("\n")) || normalized[i].Contains(TEXT("\r")) ||
            (i != 0 && normalized[i] == normalized[i - 1]))
            return true;
    }
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic) || meta.ID != record.SourceAssetID)
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    if (meta.Labels == normalized)
        return false;
    meta.Labels = normalized;
    if ((meta.FolderAsset ? RegisterExistingMetadata(record.SourcePath.Get(), meta, true, diagnostic) :
        ReplaceMetadataTransactional(record.SourcePath.Get(), meta, diagnostic)))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    Array<String> paths;
    paths.Add(record.SourcePath.Get());
    paths.Add(record.MetaPath.Get());
    return RefreshSources(paths);
#else
    return true;
#endif
}

AssetImporterMetaInfo AssetDatabaseFacade::GetImporterMetadata(const Guid& assetID)
{
    AssetImporterMetaInfo result;
#if USE_EDITOR
    if (!assetID.IsValid() || EnsureDatabaseLoaded())
        return result;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset())
        return result;
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic) || meta.ID != record.SourceAssetID)
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return result;
    }
    result.SourceAssetID = meta.ID;
    result.Revision = record.DatabaseRevision;
    result.ImporterID = meta.Processor.ID;
    result.SettingsSchemaVersion = meta.Processor.SettingsVersion;
    result.SettingsJson = String(meta.Processor.SettingsJson);
    result.ExternalObjectsJson = String(meta.Processor.ExternalObjectsJson);
    result.UserData = meta.Processor.UserData;
    result.AssetBundleName = meta.AssetBundleName;
    result.AssetBundleVariant = meta.AssetBundleVariant;
#endif
    return result;
}

bool AssetDatabaseFacade::ApplyImporterMetadata(const Guid& assetID, uint64 expectedRevision, const StringView& importerID,
    int32 settingsSchemaVersion, const StringView& settingsJson, const StringView& externalObjectsJson,
    const StringView& userData, const StringView& assetBundleName, const StringView& assetBundleVariant)
{
#if USE_EDITOR
    if (!assetID.IsValid() || importerID.IsEmpty() || settingsSchemaVersion < 1 || EnsureDatabaseLoaded())
        return true;
    AssetRecord record;
    AssetPipelineDiagnostic diagnostic;
    {
        ScopeLock mutationLock(MetadataMutationLocker);
        if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset() ||
            (expectedRevision != 0 && record.DatabaseRevision != expectedRevision))
            return true;
        AssetMeta meta;
        if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic) || meta.ID != record.SourceAssetID)
        {
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(MoveTemp(diagnostic));
            SetDiagnostics(diagnostics);
            return true;
        }
        StringAnsi currentJson;
        if (meta.ToJson(currentJson, diagnostic) || Crc::MemCrc32(currentJson.Get(), currentJson.Length()) != record.MetaSemanticHash)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = record.MetaPath.Get();
            diagnostic.AssetGuid = record.SourceAssetID;
            diagnostic.Message = TEXT("Importer metadata changed after the proxy was loaded.");
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(MoveTemp(diagnostic));
            SetDiagnostics(diagnostics);
            return true;
        }
        meta.Processor.ID = importerID;
        meta.Processor.SettingsVersion = settingsSchemaVersion;
        meta.Processor.SettingsJson = StringAnsi(settingsJson);
        meta.Processor.ExternalObjectsJson = StringAnsi(externalObjectsJson);
        meta.Processor.UserData = userData;
        meta.AssetBundleName = assetBundleName;
        meta.AssetBundleVariant = assetBundleVariant;
        if (ReplaceMetadataTransactional(record.SourcePath.Get(), meta, diagnostic))
        {
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(MoveTemp(diagnostic));
            SetDiagnostics(diagnostics);
            return true;
        }
    }
    Array<String> paths;
    paths.Add(record.SourcePath.Get());
    paths.Add(record.MetaPath.Get());
    return RefreshSources(paths);
#else
    return true;
#endif
}

bool AssetDatabaseFacade::ResetImporterMetadataToDefault(const Guid& assetID, uint64 expectedRevision)
{
#if USE_EDITOR
    AssetRecord record;
    if (!assetID.IsValid() || EnsureDatabaseLoaded() || !AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset())
        return true;
    AssetMeta current;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(record.MetaPath.Get(), current, diagnostic))
        return true;
    AssetProcessorMeta defaults;
    if (record.SourceKind == AssetSourceKind::Folder)
    {
        defaults.ID = TEXT("Flax.Folder");
    }
    else if (record.SourceKind == AssetSourceKind::ExistingJson)
    {
        defaults.ID = TEXT("Flax.ExistingJson");
    }
    else
    {
        CanonicalBatchWork work;
        work.SourcePath = record.SourcePath.Get();
        work.StagingPath = record.MetaPath.Get();
        if (PrepareDefaultCanonicalMetadata(work))
            return true;
        defaults = work.Meta.Processor;
    }
    return ApplyImporterMetadata(assetID, expectedRevision, defaults.ID, defaults.SettingsVersion,
        String(defaults.SettingsJson), String(defaults.ExternalObjectsJson), current.Processor.UserData,
        current.AssetBundleName, current.AssetBundleVariant);
#else
    return true;
#endif
}

bool AssetDatabaseFacade::ForceReserializeMetadata(const Array<String>& paths)
{
#if USE_EDITOR
    if (EnsureDatabaseLoaded())
        return true;
    HashSet<String> selected;
    for (const String& path : paths)
        selected.Add(PathKey(ResolveFacadeAssetPath(path)));
    Array<String> refreshPaths;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (!record.IsMainAsset() || (selected.HasItems() && !selected.Contains(PathKey(record.SourcePath.Get()))))
            continue;
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic) ||
            (meta.FolderAsset ? RegisterExistingMetadata(record.SourcePath.Get(), meta, true, diagnostic) :
                ReplaceMetadataTransactional(record.SourcePath.Get(), meta, diagnostic)))
        {
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(MoveTemp(diagnostic));
            SetDiagnostics(diagnostics);
            return true;
        }
        refreshPaths.Add(record.SourcePath.Get());
        refreshPaths.Add(record.MetaPath.Get());
    }
    return refreshPaths.HasItems() && RefreshSources(refreshPaths);
#else
    return true;
#endif
}

bool AssetDatabaseFacade::RegisterCustomDependency(const StringView& name, const Guid& hash)
{
#if USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    if (AssetDatabaseStorage::RegisterCustomDependency(AssetPipelineLibraryFolder(), name, hash, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    return Refresh(ImportAssetOptions::Default);
#else
    return true;
#endif
}

bool AssetDatabaseFacade::UnregisterCustomDependencyPrefix(const StringView& prefix)
{
#if USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    if (AssetDatabaseStorage::UnregisterCustomDependencyPrefix(AssetPipelineLibraryFolder(), prefix, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    return Refresh(ImportAssetOptions::Default);
#else
    return true;
#endif
}

AssetMutationResultInfo AssetDatabaseFacade::ValidateAssetMove(const StringView& sourcePath, const StringView& destinationPath)
{
    AssetMutationResult result;
    GetMutationService().Validate(AssetMutationOperation::Move, sourcePath, destinationPath, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::MoveAssetPair(const StringView& sourcePath, const StringView& destinationPath)
{
    AssetMutationResult result;
    GetMutationService().Move(sourcePath, destinationPath, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::MoveAssetPairs(const Array<String>& sourcePaths, const Array<String>& destinationPaths)
{
    AssetMutationResult result;
    GetMutationService().MoveBatch(sourcePaths, destinationPaths, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::CopyAssetPair(const StringView& sourcePath, const StringView& destinationPath)
{
    AssetMutationResult result;
    GetMutationService().Copy(sourcePath, destinationPath, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::CopyAssetPairs(const Array<String>& sourcePaths, const Array<String>& destinationPaths)
{
    AssetMutationResult result;
    GetMutationService().CopyBatch(sourcePaths, destinationPaths, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::DeleteAssetPairToRecovery(const StringView& sourcePath)
{
    AssetMutationResult result;
    GetMutationService().DeleteToRecovery(sourcePath, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::DeleteAssetPairsToRecovery(const Array<String>& sourcePaths)
{
    AssetMutationResult result;
    GetMutationService().DeleteToRecoveryBatch(sourcePaths, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::CreateAssetFolder(const StringView& path)
{
    AssetMutationResult result;
    GetMutationService().CreateFolder(path, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::PublishExternalSource(const StringView& externalSourcePath,
    const StringView& destinationPath, const StringView& typeName, const StringView& processorId, bool replaceExisting)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    const String existingMetaPath = String(destinationPath) + TEXT(".meta");
    if (!replaceExisting || !FileSystem::FileExists(existingMetaPath) || AssetMeta::Load(existingMetaPath, meta, diagnostic))
        meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = processorId == TEXT("Flax.Text") ? AssetSourceKind::TextDocument : AssetSourceKind::ImportedSource;
    meta.Processor.ID = processorId;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    AssetMutationResult result;
    GetMutationService().PublishExternal(externalSourcePath, destinationPath, meta, replaceExisting, result);
#if COMPILE_WITH_ASSETS_IMPORTER
    if (result.Succeeded && processorId != TEXT("Flax.Unsupported") && GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
    {
        result.Succeeded = false;
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
    }
#endif
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::RegisterCanonicalSource(const StringView& sourcePath, bool replaceExistingMetadata)
{
#if !USE_EDITOR
    AssetMutationResult result;
    result.Message = TEXT("Canonical source registration is editor-only.");
    return ToInfo(result);
#else
    CanonicalBatchWork work;
    work.SourcePath = sourcePath;
    work.StagingPath = String(sourcePath) + TEXT(".meta");
    work.AllowExistingStaging = replaceExistingMetadata;
    AssetMutationResult result;
    if (PrepareDefaultCanonicalMetadata(work))
    {
        result.Message = work.Diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(work.Diagnostic));
        SetDiagnostics(diagnostics);
        return ToInfo(result);
    }
    GetMutationService().RegisterExisting(sourcePath, work.Meta, replaceExistingMetadata, result);
#if COMPILE_WITH_ASSETS_IMPORTER
    if (result.Succeeded)
    {
        AssetPipelineDiagnostic diagnostic;
        bool buildFailed = false;
        switch (work.BuildKind)
        {
#if COMPILE_WITH_TEXTURE_TOOL
        case CanonicalBatchBuildKind::Texture:
            buildFailed = TexturePipelineService::RequestBuild(work.Meta.ID, false, diagnostic);
            break;
#endif
#if COMPILE_WITH_MODEL_TOOL
        case CanonicalBatchBuildKind::Model:
            buildFailed = ModelPipelineService::RequestBuild(work.Meta.ID, false, diagnostic);
            break;
#endif
        case CanonicalBatchBuildKind::Imported:
            buildFailed = GraphPipelineService::RequestBuild(work.Meta.ID, true, diagnostic);
            break;
        default:
            break;
        }
        if (buildFailed)
        {
            result.Succeeded = false;
            result.Message = diagnostic.Message;
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(MoveTemp(diagnostic));
            SetDiagnostics(diagnostics);
        }
    }
#endif
    return ToInfo(result);
#endif
}

AssetMutationResultInfo AssetDatabaseFacade::RecoverAssetPair(const StringView& recoveryPath, const StringView& destinationPath)
{
    AssetMutationResult result;
    GetMutationService().Recover(recoveryPath, destinationPath, result);
    return ToInfo(result);
}

AssetMutationResultInfo AssetDatabaseFacade::RecoverAssetPairs(const Array<String>& recoveryPaths, const Array<String>& destinationPaths)
{
    AssetMutationResult result;
    GetMutationService().RecoverBatch(recoveryPaths, destinationPaths, result);
    return ToInfo(result);
}

bool AssetDatabaseFacade::TryGetAssetObjectId(Asset* asset, AssetObjectId& result)
{
    result = AssetObjectId();
    if (!asset || EnsureDatabaseLoaded())
        return false;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(asset->GetID(), record))
        return false;
    result = AssetObjectId(record.SourceAssetID, record.LocalId);
    return true;
}

Guid AssetDatabaseFacade::GetBackingAssetID(const AssetObjectId& objectID)
{
    if (!objectID.IsValid() || EnsureDatabaseLoaded())
        return Guid::Empty;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.SourceAssetID == objectID.Guid && record.LocalId == objectID.LocalId)
            return record.ID;
    }
    return Guid::Empty;
}

String AssetDatabaseFacade::GetCanonicalSourcePath(const Guid& assetID)
{
    AssetRecord record;
    return assetID.IsValid() && AssetDatabase::Get().TryGetRecord(assetID, record)
        ? String(record.SourcePath.Get())
        : String::Empty;
}

Asset* AssetDatabaseFacade::LoadAssetPreview(const Guid& assetID)
{
#if USE_EDITOR
    return Content::LoadAsyncPreview(assetID, Asset::TypeInitializer);
#else
    return nullptr;
#endif
}

Guid AssetDatabaseFacade::GetPublishedArtifactCacheID(const Guid& assetID, const StringView& outputKind)
{
#if USE_EDITOR
    ArtifactResolver& resolver = ArtifactResolver::Get();
    if (!resolver.IsConfigured() || !assetID.IsValid() || outputKind.IsEmpty())
        return Guid::Empty;
    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = resolver.GetDefaultTarget();
    request.OutputKind = StringAnsi(outputKind);
    request.Policy = ArtifactResolvePolicy::PublishedOnly;
    ResolvedArtifact artifact;
    AssetPipelineDiagnostic diagnostic;
    ArtifactKey key;
    if (resolver.Resolve(request, artifact, diagnostic) || ArtifactKey::Parse(artifact.Key, key))
        return Guid::Empty;
    return Guid(key.Digest.Values[0], key.Digest.Values[1], key.Digest.Values[2], key.Digest.Values[3]);
#else
    return Guid::Empty;
#endif
}

bool AssetDatabaseFacade::HasPublishedArtifact(const Guid& assetID, const StringView& outputKind)
{
#if USE_EDITOR
    ArtifactResolver& resolver = ArtifactResolver::Get();
    if (!assetID.IsValid() || outputKind.IsEmpty())
        return false;
#if COMPILE_WITH_TEXTURE_TOOL && COMPILE_WITH_ASSETS_IMPORTER
    if (!resolver.IsConfigured())
    {
        AssetPipelineDiagnostic initializationDiagnostic;
        if (!TexturePipelineService::GetBuildService(initializationDiagnostic))
            return false;
    }
#endif
    if (!resolver.IsConfigured())
        return false;
    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = resolver.GetDefaultTarget();
    request.OutputKind = StringAnsi(outputKind);
    request.Policy = ArtifactResolvePolicy::PublishedOnly;
    ResolvedArtifact artifact;
    AssetPipelineDiagnostic diagnostic;
    return !resolver.Resolve(request, artifact, diagnostic);
#else
    return false;
#endif
}

bool AssetDatabaseFacade::Scan(bool strictMetadata)
{
#if USE_EDITOR
    EnsureBound();
    Array<AssetPipelineDiagnostic> metadataDiagnostics;
    if (ReadProjectBootstrap(metadataDiagnostics))
    {
        SetDiagnostics(metadataDiagnostics);
        return true;
    }
    const int32 assetSystemVersion = ConfiguredAssetSystemVersion;
    AssetDatabase::Get().SetHardCutEnabled(true);
    AssetDatabaseScanOptions options;
    options.AssetSystemVersion = assetSystemVersion;
    options.StrictMetadata = strictMetadata;
    options.HashCache = &HashCache;
    AssetDatabaseScanResult result;
    Array<AssetRecord> records;
    const AssetDatabaseSnapshot previous = AssetDatabase::Get().GetSnapshot();
    bool failed = false;
    const Array<AssetMount> mounts = AssetMountRegistry::GetMounts();
    for (const AssetMount& mount : mounts)
    {
        if (CollectMount(mount, options, previous, records, result))
        {
            failed = true;
            break;
        }
    }
    if (!failed)
        AssetDatabaseScanner::ProjectRuntimeReferences(records, result.Diagnostics);
    result.Diagnostics.Add(metadataDiagnostics);
    if (!failed)
    {
        AssetPipelineDiagnostic publishDiagnostic;
        failed = AssetDatabase::Get().PublishFullSnapshot(records, publishDiagnostic);
        if (failed)
            result.Diagnostics.Add(MoveTemp(publishDiagnostic));
        else
            result.Revision = AssetDatabase::Get().GetRevision();
    }
    SetDiagnostics(result.Diagnostics);
    if (!failed)
    {
        LastFileStates = result.FileStates;
        const String directory = SnapshotDirectory();
        if (!FileSystem::DirectoryExists(directory))
            FileSystem::CreateDirectory(directory);
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseStorage::Save(DatabasePath(), Globals::ProjectFolder, Globals::ProjectContentFolder, AssetDatabase::Get().GetSnapshot(), LastFileStates, diagnostic))
        {
            ScopeLock lock(StateLocker);
            LastDiagnostics.Add(diagnostic);
        }
    }
    return failed;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::LoadOrScan(bool strictMetadata)
{
    EnsureBound();
    Array<AssetPipelineDiagnostic> bootstrapDiagnostics;
    if (ReadProjectBootstrap(bootstrapDiagnostics))
    {
        SetDiagnostics(bootstrapDiagnostics);
        return true;
    }
    AssetDatabase::Get().SetHardCutEnabled(true);
    const bool importerWorker = Engine::GetCommandLine().Contains(TEXT("-assetImportWorker"), StringSearchCase::IgnoreCase);
    if (importerWorker)
    {
        Array<AssetDatabaseFileState> states;
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseStorage::Load(DatabasePath(), Globals::ProjectFolder, Globals::ProjectContentFolder,
            AssetDatabase::Get(), states, diagnostic, true))
        {
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(MoveTemp(diagnostic));
            SetDiagnostics(diagnostics);
            return true;
        }
        HashCache.Seed(states);
        LastFileStates = MoveTemp(states);
        SetDiagnostics(Array<AssetPipelineDiagnostic>());
        return false;
    }
    Array<AssetMutationResult> recoveryResults;
    const bool recoveryFailed = GetMutationService().RecoverPending(recoveryResults);
    if (recoveryFailed)
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        for (const AssetMutationResult& recovery : recoveryResults)
        {
            if (!recovery.RequiresRecovery)
                continue;
            AssetPipelineDiagnostic item;
            item.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            item.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            item.SourcePath = recovery.SourcePath;
            item.Message = recovery.Message;
            item.Remediation = TEXT("Resolve the preserved mutation journal before editing this source path.");
            diagnostics.Add(MoveTemp(item));
        }
        SetDiagnostics(diagnostics);
        return true;
    }
    if (recoveryResults.HasItems())
        return Scan(strictMetadata);
    Array<AssetDatabaseFileState> states;
    AssetPipelineDiagnostic diagnostic;
    if (!AssetDatabaseStorage::Load(DatabasePath(), Globals::ProjectFolder, Globals::ProjectContentFolder, AssetDatabase::Get(), states, diagnostic))
    {
        HashCache.Seed(states);
        LastFileStates = states;
        if (FileStatesStillMatch(states))
        {
            if (!strictMetadata)
            {
                SetDiagnostics(Array<AssetPipelineDiagnostic>());
                return false;
            }
        }
    }
    return Scan(strictMetadata);
}

bool AssetDatabaseFacade::RefreshSources(const Array<String>& paths)
{
#if USE_EDITOR
    EnsureBound();
    if (paths.IsEmpty())
        return false;

    const Array<AssetMount> previousMounts = AssetMountRegistry::GetMounts();
    Array<AssetPipelineDiagnostic> metadataDiagnostics;
    if (ReadProjectBootstrap(metadataDiagnostics))
    {
        SetDiagnostics(metadataDiagnostics);
        return true;
    }
    if (!MountTablesMatch(previousMounts, AssetMountRegistry::GetMounts()))
        return Scan(true);
    const int32 assetSystemVersion = ConfiguredAssetSystemVersion;
    AssetDatabase::Get().SetHardCutEnabled(true);
    const AssetDatabaseSnapshot previous = AssetDatabase::Get().GetSnapshot();
    HashSet<String> affectedKeys;
    Array<String> expanded;
    Array<String> refreshedRootKeys;
    for (const String& path : paths)
    {
        ExpandRefreshPath(path, affectedKeys, expanded);
        const String normalized = NormalizeAbsolutePath(path);
        if (normalized.IsEmpty())
            continue;
        if (FileSystem::DirectoryExists(normalized))
        {
            AddUniquePath(normalized, affectedKeys, expanded);
            AddUniquePath(normalized + TEXT(".meta"), affectedKeys, expanded);
            Array<String> directories;
            if (!CollectSourceDirectories(normalized, directories))
            {
                for (const String& directory : directories)
                {
                    AddUniquePath(directory, affectedKeys, expanded);
                    AddUniquePath(directory + TEXT(".meta"), affectedKeys, expanded);
                }
            }
        }
        refreshedRootKeys.Add(PathKey(normalized));
        if (FileSystem::FileExists(normalized) && !FileSystem::DirectoryExists(normalized))
            continue;
        for (const AssetRecord& record : previous.Records)
        {
            if (!PathIsUnder(record.SourcePath.Get(), normalized) &&
                (record.MetaPath.Get().IsEmpty() || !PathIsUnder(record.MetaPath.Get(), normalized)))
                continue;
            AddUniquePath(record.SourcePath.Get(), affectedKeys, expanded);
            if (!record.MetaPath.Get().IsEmpty())
                AddUniquePath(record.MetaPath.Get(), affectedKeys, expanded);
        }
    }

    // A conflict status is a statement about the whole database, so a record can never stop
    // reporting one unless it is re-collected. Conflicts are normally absent, which makes pulling
    // every one of them into each refresh cheap.
    for (const AssetRecord& record : previous.Records)
    {
        if (record.Status != AssetRecordStatus::PathCollision && record.Status != AssetRecordStatus::DuplicateGuid)
            continue;
        AddUniquePath(record.SourcePath.Get(), affectedKeys, expanded);
        if (!record.MetaPath.Get().IsEmpty())
            AddUniquePath(record.MetaPath.Get(), affectedKeys, expanded);
    }

    const Array<AssetMount> mounts = AssetMountRegistry::GetMounts();
    Array<Array<String>> mountFiles;
    mountFiles.Resize(mounts.Count());
    for (const String& path : expanded)
    {
        if (!FileSystem::FileExists(path) && !FileSystem::DirectoryExists(path))
            continue;
        AssetMountResolution resolution;
        AssetPipelineDiagnostic resolutionDiagnostic;
        if (AssetMountRegistry::Get().ResolvePhysical(path, resolution, resolutionDiagnostic))
            continue;
        for (int32 mountIndex = 0; mountIndex < mounts.Count(); mountIndex++)
        {
            if (mounts[mountIndex].MountId == resolution.Mount.MountId)
            {
                mountFiles[mountIndex].Add(path);
                break;
            }
        }
    }
    AssetDatabaseScanOptions options;
    options.AssetSystemVersion = assetSystemVersion;
    options.HashCache = &HashCache;
    // Matches a full editor Scan, so a source that shows up without a sidecar still reports
    // MissingMeta and can be picked up by the metadata registration queue.
    options.StrictMetadata = true;
    AssetDatabaseScanResult result;
    Array<AssetRecord> collected;
    for (int32 mountIndex = 0; mountIndex < mounts.Count(); mountIndex++)
    {
        if (mountFiles[mountIndex].IsEmpty())
            continue;
        if (CollectMountFiles(mounts[mountIndex], mountFiles[mountIndex], options, previous, collected, result))
            return true;
    }

    Array<AssetRecord> merged;
    merged.EnsureCapacity(previous.Records.Count() + collected.Count());
    for (const AssetRecord& record : previous.Records)
    {
        if (!RecordPathAffected(record, affectedKeys))
            merged.Add(record);
    }
    merged.Add(collected);
    AssetDatabaseScanner::ProjectRuntimeReferences(merged, result.Diagnostics);

    const bool identityChanged = SnapshotIdentityChanged(previous, merged);
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(result.Diagnostics);
    diagnostics.Add(metadataDiagnostics);
    bool publishFailed = false;
    if (identityChanged)
    {
        AssetPipelineDiagnostic publishDiagnostic;
        if (AssetDatabase::Get().PublishFullSnapshot(merged, publishDiagnostic))
        {
            diagnostics.Add(MoveTemp(publishDiagnostic));
            publishFailed = true;
        }
    }
    MergeScopedDiagnostics(affectedKeys, diagnostics);
    if (publishFailed)
        return true;

    Array<AssetDatabaseFileState> nextStates;
    nextStates.EnsureCapacity(LastFileStates.Count() + result.FileStates.Count());
    for (const AssetDatabaseFileState& state : LastFileStates)
    {
        const String key = PathKey(state.Path);
        if (affectedKeys.Contains(key))
            continue;
        // Files that vanished under a refreshed root, such as a deleted directory, would otherwise
        // be carried forever and keep invalidating the persisted snapshot. Existence is probed only
        // for those, so an ordinary single-file refresh stays free of extra syscalls here.
        if (IsKeyUnderAnyRoot(key, refreshedRootKeys) && !FileSystem::FileExists(state.Path))
            continue;
        nextStates.Add(state);
    }
    nextStates.Add(result.FileStates);
    LastFileStates = MoveTemp(nextStates);
    return PersistSnapshot();
#else
    return true;
#endif
}

bool AssetDatabaseFacade::ImportAsset(const StringView& path, ImportAssetOptions options)
{
#if USE_EDITOR
    if (path.IsEmpty())
        return true;
    String resolved = ResolveFacadeAssetPath(path);
    if (IsMetaPath(resolved))
        resolved = resolved.Left(resolved.Length() - 5);
    if (!IsFacadeAssetPath(resolved))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Asset import path must be under Content or EngineContent.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    if (!FileSystem::FileExists(resolved) && !FileSystem::DirectoryExists(resolved))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Asset import source does not exist.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    const bool isDirectory = FileSystem::DirectoryExists(resolved);
    if (isDirectory && !EnumHasAnyFlags(options, ImportAssetOptions::ImportRecursive))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Folder import requires ImportRecursive.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }

    Array<String> sources;
    if (isDirectory)
    {
        if (FileSystem::DirectoryGetFiles(sources, resolved, TEXT("*"), DirectorySearchOption::AllDirectories))
            return true;
        if (sources.Count() > 1)
            std::sort(sources.Get(), sources.Get() + sources.Count());
    }
    else if (FileSystem::FileExists(resolved))
    {
        sources.Add(resolved);
    }

    Array<AssetPipelineDiagnostic> preparationDiagnostics;
    for (const String& source : sources)
    {
        if (IsMetaPath(source) || IsV3MetadataExcluded(source) || !AssetPathPolicy::IsSameOrChild(source, Globals::ProjectContentFolder))
            continue;
        AssetPipelineDiagnostic diagnostic;
        if (EnsureDefaultCanonicalMetadata(source, diagnostic))
            preparationDiagnostics.Add(MoveTemp(diagnostic));
    }
    if (preparationDiagnostics.HasItems())
    {
        SetDiagnostics(preparationDiagnostics);
        return true;
    }

    Array<String> refreshPaths;
    refreshPaths.Add(resolved);
    if (RefreshSources(refreshPaths))
        return true;
#if COMPILE_WITH_ASSETS_IMPORTER
    const bool force = EnumHasAnyFlags(options, ImportAssetOptions::ForceUpdate) ||
        EnumHasAnyFlags(options, ImportAssetOptions::ForceUncompressedImport);
    const bool synchronous = EnumHasAnyFlags(options, ImportAssetOptions::ForceSynchronousImport);
    bool matched = false;
    Array<AssetPipelineDiagnostic> buildDiagnostics;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (!record.IsMainAsset())
            continue;
        const bool pathMatches = isDirectory
            ? PathIsUnder(record.SourcePath.Get(), resolved)
            : FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolved);
        if (!pathMatches)
            continue;
        matched = true;
        AssetPipelineDiagnostic diagnostic;
        const GenericBuildRequestResult request = RequestGenericBuild(record, force, synchronous, diagnostic);
        if (request == GenericBuildRequestResult::Failed)
            buildDiagnostics.Add(MoveTemp(diagnostic));
        else if (request == GenericBuildRequestResult::Unsupported && record.Status == AssetRecordStatus::UnsupportedProcessor)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = record.ID;
            diagnostic.SourcePath = record.SourcePath.Get();
            diagnostic.Message = TEXT("Imported source has no registered asset processor.");
            buildDiagnostics.Add(MoveTemp(diagnostic));
        }
    }
    if (buildDiagnostics.HasItems())
    {
        SetDiagnostics(buildDiagnostics);
        return true;
    }
    if (!matched && FileSystem::FileExists(resolved))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Imported source has no registered asset processor.");
        buildDiagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(buildDiagnostics);
        return true;
    }
#endif
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::Refresh(ImportAssetOptions options)
{
#if USE_EDITOR
    Array<String> sources;
    if (FileSystem::DirectoryGetFiles(sources, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
        return true;
    Array<AssetPipelineDiagnostic> preparationDiagnostics;
    for (const String& source : sources)
    {
        if (IsMetaPath(source) || IsV3MetadataExcluded(source))
            continue;
        AssetPipelineDiagnostic diagnostic;
        if (EnsureDefaultCanonicalMetadata(source, diagnostic))
            preparationDiagnostics.Add(MoveTemp(diagnostic));
    }
    if (preparationDiagnostics.HasItems())
    {
        SetDiagnostics(preparationDiagnostics);
        return true;
    }
    if (Scan(false))
        return true;
#if COMPILE_WITH_ASSETS_IMPORTER
    const bool force = EnumHasAnyFlags(options, ImportAssetOptions::ForceUpdate) ||
        EnumHasAnyFlags(options, ImportAssetOptions::ForceUncompressedImport);
    const bool synchronous = EnumHasAnyFlags(options, ImportAssetOptions::ForceSynchronousImport);
    Array<AssetPipelineDiagnostic> buildDiagnostics;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (!record.IsMainAsset())
            continue;
        AssetPipelineDiagnostic diagnostic;
        if (RequestGenericBuild(record, force, synchronous, diagnostic) == GenericBuildRequestResult::Failed)
            buildDiagnostics.Add(MoveTemp(diagnostic));
    }
    if (buildDiagnostics.HasItems())
    {
        SetDiagnostics(buildDiagnostics);
        return true;
    }
#endif
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::CleanLibrary()
{
#if USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    const bool failed = ArtifactStore::CleanEntireLibrary(diagnostic);
    Array<AssetPipelineDiagnostic> diagnostics;
    if (diagnostic.Code != AssetPipelineDiagnosticCode::None)
        diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
    return failed;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::CloneMetadata(const StringView& sourceMetaPath, const StringView& destinationMetaPath)
{
    AssetPipelineDiagnostic diagnostic;
    if (AssetDatabase::Get().IsHardCutEnabled())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = destinationMetaPath;
        diagnostic.Message = TEXT("Standalone metadata cloning is disabled after the Asset System v3 hard cut; copy the source pair through AssetMutationService.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    AssetMeta source;
    if (AssetMeta::Load(sourceMetaPath, source, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    const AssetMeta clone = source.CloneWithNewIdentities();
    if (AssetMeta::SaveAtomic(destinationMetaPath, clone, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    return false;
}

#if USE_EDITOR
Array<Guid> AssetDatabaseFacade::StageDefaultCanonicalMetadataBatch(const Array<String>& sourcePaths, const Array<String>& stagingPaths)
{
    Array<Guid> result;
    result.Resize(sourcePaths.Count());
    Array<AssetPipelineDiagnostic> diagnostics;
    if (sourcePaths.Count() != stagingPaths.Count())
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.Message = TEXT("Canonical metadata batch source and staging path counts do not match.");
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return result;
    }
    for (const String& stagingPath : stagingPaths)
    {
        if (AssetPathPolicy::IsSameOrChild(stagingPath, Globals::ProjectContentFolder))
        {
            AssetPipelineDiagnostic diagnostic;
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.SourcePath = stagingPath;
            diagnostic.Message = TEXT("Metadata preparation staging cannot write inside a canonical source root.");
            diagnostics.Add(MoveTemp(diagnostic));
        }
    }
    if (diagnostics.HasItems())
    {
        SetDiagnostics(diagnostics);
        return result;
    }

    Array<CanonicalBatchWork> work;
    work.Resize(sourcePaths.Count());
    for (int32 i = 0; i < work.Count(); i++)
    {
        work[i].SourcePath = sourcePaths[i];
        work[i].StagingPath = stagingPaths[i];
    }

    constexpr int32 MaxPreparationConcurrency = 4;
    for (int32 begin = 0; begin < work.Count(); begin += MaxPreparationConcurrency)
    {
        const int32 end = Math::Min(begin + MaxPreparationConcurrency, work.Count());
        std::vector<std::future<void>> tasks;
        tasks.reserve(end - begin);
        for (int32 i = begin; i < end; i++)
        {
            tasks.emplace_back(std::async(std::launch::async, [&work, i]
            {
                work[i].Failed = PrepareDefaultCanonicalMetadata(work[i]);
                if (work[i].Failed)
                {
                    if (work[i].Diagnostic.SourcePath.IsEmpty())
                        work[i].Diagnostic.SourcePath = work[i].SourcePath;
                    if (work[i].Diagnostic.AssetGuid == Guid::Empty)
                        work[i].Diagnostic.AssetGuid = work[i].Meta.ID;
                }
            }));
        }
        for (std::future<void>& task : tasks)
            task.get();
    }

    for (int32 i = 0; i < work.Count(); i++)
    {
        CanonicalBatchWork& item = work[i];
        if (!item.Failed && AssetMeta::SaveAtomic(item.StagingPath, item.Meta, item.Diagnostic))
            item.Failed = true;
        if (item.Failed)
        {
            diagnostics.Add(item.Diagnostic);
            continue;
        }
        result[i] = item.Meta.ID;
    }
    SetDiagnostics(diagnostics);
    return result;
}

bool AssetDatabaseFacade::PublishDefaultCanonicalMetadataBatch(const Array<Guid>& assetIDs, const Array<String>& sourcePaths)
{
    if (sourcePaths.Count() ? RefreshSources(sourcePaths) : Scan(false))
        return true;
#if COMPILE_WITH_ASSETS_IMPORTER
    Array<AssetPipelineDiagnostic> diagnostics;
    bool failed = false;
    for (const Guid& assetID : assetIDs)
    {
        AssetRecord record;
        AssetPipelineDiagnostic diagnostic;
        if (!assetID.IsValid() || !AssetDatabase::Get().TryGetRecord(assetID, record))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.AssetGuid = assetID;
            diagnostic.Message = TEXT("Staged canonical metadata was not published into the asset database.");
            diagnostics.Add(MoveTemp(diagnostic));
            failed = true;
            continue;
        }

#if COMPILE_WITH_TEXTURE_TOOL
        if (record.ProcessorID == TextureProcessorSettings::ProcessorID())
            failed = TexturePipelineService::RequestBuild(assetID, false, diagnostic);
        else
#endif
#if COMPILE_WITH_MODEL_TOOL
        if (record.ProcessorID == ModelProcessorSettings::ProcessorID())
            failed = ModelPipelineService::RequestBuild(assetID, false, diagnostic);
        else
#endif
        if (ImportedSourceProcessor::Owns(record.ProcessorID) || record.ProcessorID == GraphDocumentProcessor::ProcessorID())
            failed = GraphPipelineService::RequestBuild(assetID, true, diagnostic);
        else
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = assetID;
            diagnostic.SourcePath = record.SourcePath.Get();
            diagnostic.ProcessorId = record.ProcessorID;
            diagnostic.Message = TEXT("Published canonical metadata has no supported build pipeline.");
            failed = true;
        }
        if (failed)
            diagnostics.Add(MoveTemp(diagnostic));
    }
    if (diagnostics.HasItems())
        SetDiagnostics(diagnostics);
    return diagnostics.HasItems();
#else
    return false;
#endif
}
#endif

#if COMPILE_WITH_TEXTURE_TOOL
Guid AssetDatabaseFacade::CreateTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options)
{
#if !USE_EDITOR
    return Guid::Empty;
#else
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Texture metadata already exists.") : TEXT("Texture source does not exist.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
    if (!options.IsAtlas)
    {
        TextureData sourceData;
        if (!TextureTool::ImportTexture(sourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
            meta.AssetType = CubeTexture::TypeName;
    }
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TextureProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic) || RegisterExistingMetadata(sourcePath, meta, false, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    if (RefreshPath(sourcePath))
        return Guid::Empty;
#if COMPILE_WITH_ASSETS_IMPORTER
    if (TexturePipelineService::RequestBuild(meta.ID, false, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
#endif
    return meta.ID;
#endif
}

AssetMutationResultInfo AssetDatabaseFacade::PublishExternalTexture(const StringView& externalSourcePath,
    const StringView& destinationPath, const TextureTool::Options& options, bool replaceExisting)
{
    AssetPipelineDiagnostic diagnostic;
    TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
    AssetMutationResult result;
    if (settings.Validate(diagnostic))
    {
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return ToInfo(result);
    }

    AssetMeta meta;
    const String existingMetaPath = String(destinationPath) + TEXT(".meta");
    if (!replaceExisting || !FileSystem::FileExists(existingMetaPath) || AssetMeta::Load(existingMetaPath, meta, diagnostic))
        meta.ID = Guid::New();
    meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
    if (!options.IsAtlas)
    {
        TextureData sourceData;
        if (!TextureTool::ImportTexture(externalSourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
            meta.AssetType = CubeTexture::TypeName;
    }
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TextureProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic))
    {
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return ToInfo(result);
    }
    GetMutationService().PublishExternal(externalSourcePath, destinationPath, meta, replaceExisting, result);
#if COMPILE_WITH_ASSETS_IMPORTER
    if (result.Succeeded && TexturePipelineService::RequestBuild(meta.ID, false, diagnostic))
    {
        result.Succeeded = false;
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
    }
#endif
    return ToInfo(result);
}

Guid AssetDatabaseFacade::StageLegacyTextureMigration(const StringView& legacyPath, const StringView& extractedPath,
    const StringView& destinationPath, const StringView& backupPath, const TextureTool::Options& options)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    FlaxStorage::Entry root;
    {
        FlaxStorageReference storage = ContentStorageManager::GetStorage(legacyPath, true);
        if (!storage || storage->GetEntriesCount() < 1)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = legacyPath;
            diagnostic.Message = TEXT("Legacy texture header could not be read.");
            return fail();
        }
        storage->GetEntry(0, root);
    }
    TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
        return fail();
    AssetMeta meta;
    meta.ID = root.ID;
    meta.AssetType = root.TypeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TextureProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic) ||
        StageImportedFiles(legacyPath, extractedPath, destinationPath, backupPath, meta, diagnostic))
        return fail();
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return meta.ID;
}

bool AssetDatabaseFacade::LoadTextureMetadata(const StringView& sourcePath, TextureTool::Options& options)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    TextureProcessorSettings settings;
    if (TextureProcessorSettings::Parse(meta.Processor.SettingsJson, meta.Processor.SettingsVersion, settings, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    options = settings.ToImportOptions(StringAnsiView("windows"));
    return false;
}

bool AssetDatabaseFacade::ApplyTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(metaPath, meta, diagnostic))
        goto Failed;
    {
        const TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
        if (settings.Validate(diagnostic) || settings.ToJson(meta.Processor.SettingsJson, diagnostic))
            goto Failed;
        meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
        if (!options.IsAtlas)
        {
            TextureData sourceData;
            if (!TextureTool::ImportTexture(sourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
                meta.AssetType = CubeTexture::TypeName;
        }
        meta.Processor.ID = TextureProcessorSettings::ProcessorID();
        meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    }
    if (ReplaceMetadataTransactional(sourcePath, meta, diagnostic) || RefreshPath(sourcePath))
        goto Failed;
#if COMPILE_WITH_ASSETS_IMPORTER
    if (TexturePipelineService::RequestBuild(meta.ID, false, diagnostic))
        goto Failed;
#endif
    return false;

Failed:
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    return true;
}

bool AssetDatabaseFacade::RebuildTexture(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!TexturePipelineService::RequestBuild(assetID, true, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

bool AssetDatabaseFacade::BuildTexture(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!TexturePipelineService::RequestBuild(assetID, false, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

String AssetDatabaseFacade::GetTextureBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    switch (TexturePipelineService::GetStatus(assetID, diagnostic))
    {
    case AssetBuildJobStatus::Queued: return TEXT("Queued");
    case AssetBuildJobStatus::Building: return TEXT("Building");
    case AssetBuildJobStatus::Publishing: return TEXT("Publishing");
    case AssetBuildJobStatus::Succeeded: return TEXT("ReadyExact");
    case AssetBuildJobStatus::Failed: return TEXT("Failed");
    case AssetBuildJobStatus::Cancelled: return TEXT("Cancelled");
    default: break;
    }
#endif
    return TEXT("NotBuilt");
}

AssetPipelineDiagnostic AssetDatabaseFacade::GetTextureBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER
    TexturePipelineService::GetStatus(assetID, diagnostic);
#endif
    return diagnostic;
}

Texture* AssetDatabaseFacade::LoadTextureThumbnail(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    if (!ArtifactResolver::Get().IsConfigured())
        return nullptr;
    AssetPipelineDiagnostic diagnostic;
    const AssetBuildJobStatus thumbnailStatus = TexturePipelineService::GetThumbnailStatus(assetID, diagnostic);
    if (thumbnailStatus == AssetBuildJobStatus::Queued || thumbnailStatus == AssetBuildJobStatus::Building ||
        thumbnailStatus == AssetBuildJobStatus::Publishing || thumbnailStatus == AssetBuildJobStatus::Failed ||
        thumbnailStatus == AssetBuildJobStatus::Cancelled)
        return nullptr;
    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = TexturePipelineService::GetHostTarget();
    request.OutputKind = "thumbnail";
    request.RequiredCompatibility = "flax-texture-thumbnail-v2";
    request.Policy = ArtifactResolvePolicy::NoBuild;
    ResolvedArtifact artifact;
    if (ArtifactResolver::Get().Resolve(request, artifact, diagnostic))
    {
        TexturePipelineService::RequestThumbnailBuild(assetID, diagnostic);
        return nullptr;
    }
    const ArtifactLease lease = ArtifactLease::Acquire(artifact.StoragePath.Get());
    TextureData textureData;
    if (TextureTool::ImportTexture(artifact.StoragePath.Get(), textureData, false))
        return nullptr;
    if (PixelFormatExtensions::IsSRGB(textureData.Format))
        textureData.Format = PixelFormatExtensions::ToNonsRGB(textureData.Format);
    auto* texture = Content::CreateVirtualAsset<Texture>();
    auto* initData = New<TextureBase::InitData>();
    initData->FromTextureData(textureData, false);
    if (texture->Init(initData))
    {
        texture->DeleteObject();
        return nullptr;
    }
    return texture;
#else
    return nullptr;
#endif
}
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
bool AssetDatabaseFacade::LoadModelMetadata(const StringView& sourcePath, ModelTool::Options& options)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
        goto Failed;
    {
        ModelProcessorSettings settings;
        if (meta.Processor.ID != ModelProcessorSettings::ProcessorID())
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.ProcessorId = meta.Processor.ID;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Model metadata is not owned by the Flax.Model processor.");
            goto Failed;
        }
        if (ModelProcessorSettings::Parse(meta.Processor.SettingsJson, meta.Processor.SettingsVersion, settings, diagnostic))
            goto Failed;
        options = settings.Import;
    }
    return false;

Failed:
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    return true;
}

bool AssetDatabaseFacade::ApplyModelMetadata(const StringView& sourcePath, const ModelTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(metaPath, meta, diagnostic))
        goto Failed;
    {
        const ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
        if (settings.Validate(diagnostic) || settings.ToJson(meta.Processor.SettingsJson, diagnostic))
            goto Failed;
        meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel || options.Type == ModelTool::ModelType::Animation
            ? SkinnedModel::TypeName
            : Model::TypeName;
        meta.Processor.ID = ModelProcessorSettings::ProcessorID();
        meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
    }
    if (ReplaceMetadataTransactional(sourcePath, meta, diagnostic) || RefreshPath(sourcePath))
        goto Failed;
#if COMPILE_WITH_ASSETS_IMPORTER
    if (ModelPipelineService::RequestBuild(meta.ID, false, diagnostic))
        goto Failed;
#endif
    return false;

Failed:
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    return true;
}

bool AssetDatabaseFacade::ReconcileModel(const Guid& rootAssetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    Array<SubAssetReconcileChange> changes;
    AssetPipelineDiagnostic diagnostic;
    if (!ModelPipelineService::ReconcileMetadata(rootAssetID, changes, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

bool AssetDatabaseFacade::BuildModel(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!ModelPipelineService::RequestBuild(assetID, false, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

bool AssetDatabaseFacade::RebuildModel(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!ModelPipelineService::RequestBuild(assetID, true, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

String AssetDatabaseFacade::GetModelBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    switch (ModelPipelineService::GetStatus(assetID, diagnostic))
    {
    case AssetBuildJobStatus::Queued: return TEXT("Queued");
    case AssetBuildJobStatus::Building: return TEXT("Building");
    case AssetBuildJobStatus::Publishing: return TEXT("Publishing");
    case AssetBuildJobStatus::Succeeded: return TEXT("ReadyExact");
    case AssetBuildJobStatus::Failed: return TEXT("Failed");
    case AssetBuildJobStatus::Cancelled: return TEXT("Cancelled");
    default: break;
    }
#endif
    return TEXT("NotBuilt");
}

AssetPipelineDiagnostic AssetDatabaseFacade::GetModelBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER
    ModelPipelineService::GetStatus(assetID, diagnostic);
    if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
    {
        const Array<AssetPipelineDiagnostic> diagnostics = GetDiagnostics();
        if (diagnostics.HasItems())
            diagnostic = diagnostics[0];
    }
#endif
    return diagnostic;
}
#endif

Guid AssetDatabaseFacade::CreateGraphDocument(const StringView& outputPath, const StringView& typeName, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!GraphDocumentCodec::IsSupportedType(typeName) || outputPath.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Graph document type or path is invalid.");
        return fail();
    }
    GraphDocument document;
    if (GraphDocumentCodec::CreateStarter(typeName, document, diagnostic))
        return fail();
    Array<byte> surface;
    if (GraphDocumentCompiler::CompileDocument(document, surface, diagnostic))
        return fail();

    BytesContainer data;
    data.Link(ToSpan(surface));
    return CreateGraphDocumentFromSurface(outputPath, typeName, data, propertiesJson);
}

Guid AssetDatabaseFacade::CreateGraphDocumentFromSurface(const StringView& outputPath, const StringView& typeName, const BytesContainer& surface, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!GraphDocumentCodec::IsSupportedType(typeName) || outputPath.IsEmpty() || !surface.IsValid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Graph document type, path, or surface is invalid.");
        return fail();
    }
    GraphDocument document;
    StringAnsi json;
    if (GraphDocumentCodec::FromSurface(typeName, surface, document, diagnostic))
        return fail();
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic))
        return fail();

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = TEXT("Flax.GraphDocument");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    AssetMutationResult mutation;
    if (GetMutationService().CreateAsset(outputPath, StringAnsiView(json.Get(), json.Length()), meta, mutation))
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = outputPath;
        diagnostic.AssetGuid = meta.ID;
        diagnostic.Message = mutation.Message;
        return fail();
    }
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
        return fail();
#endif
    return meta.ID;
}

Guid AssetDatabaseFacade::CreateAuthoredDocument(const StringView& outputPath, const StringView& typeName)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    String processorID;
    if (typeName == MaterialInstance::TypeName)
        processorID = TEXT("Flax.MaterialInstance");
    else if (typeName == SkeletonMask::TypeName)
        processorID = TEXT("Flax.SkeletonMask");
    else if (typeName == SceneAnimation::TypeName)
        processorID = TEXT("Flax.SceneAnimation");
    else if (typeName == ParticleSystem::TypeName)
        processorID = TEXT("Flax.ParticleSystem");
    else if (typeName == CollisionData::TypeName)
        processorID = TEXT("Flax.CollisionData");
    else if (typeName == Animation::TypeName)
        processorID = TEXT("Flax.Animation");
    else if (typeName == GameplayGlobals::TypeName)
        processorID = TEXT("Flax.GameplayGlobals");
    if (processorID.IsEmpty() || outputPath.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Authored document type or path is invalid.");
        return fail();
    }

    StringAnsi source;
    AuthoredJsonDocument document;
    String error;
    bool serializeFailed = false;
    if (typeName == MaterialInstance::TypeName)
    {
        MemoryWriteStream stream(64);
        stream.Write(Guid::Empty);
        MaterialParams::Save(&stream, nullptr);
        serializeFailed = MaterialInstanceDocument::DecodeRuntime(ToSpan(stream), document, error);
    }
    else if (typeName == SkeletonMask::TypeName)
    {
        document.SetObject();
        auto& allocator = document.GetAllocator();
        const StringAnsi type(typeName);
        const StringAnsi skeleton = AuthoredGuidText(Guid::Empty);
        document.AddMember("documentVersion", 1, allocator);
        document.AddMember("type", AuthoredJsonValue(type.Get(), type.Length(), allocator), allocator);
        document.AddMember("skeleton", AuthoredJsonValue(skeleton.Get(), skeleton.Length(), allocator), allocator);
        document.AddMember("maskedNodes", AuthoredJsonValue(rapidjson::kArrayType), allocator);
    }
    else if (typeName == SceneAnimation::TypeName)
    {
        MemoryWriteStream stream(64);
        stream.WriteInt32(4);
        stream.WriteFloat(60.0f);
        stream.WriteInt32(5 * 60);
        stream.WriteInt32(0);
        serializeFailed = SceneAnimationDocument::DecodeRuntime(ToSpan(stream), document, error);
    }
    else if (typeName == ParticleSystem::TypeName)
    {
        MemoryWriteStream stream(64);
        stream.WriteInt32(4);
        stream.WriteFloat(60.0f);
        stream.WriteInt32(5 * 60);
        stream.WriteInt32(0);
        stream.WriteInt32(0);
        stream.WriteInt32(0);
        serializeFailed = ParticleSystemDocument::DecodeRuntime(ToSpan(stream), document, error);
    }
    else if (typeName == CollisionData::TypeName)
    {
        CollisionData::SerializedOptions options;
        Platform::MemoryClear(&options, sizeof(options));
        options.MaterialSlotsMask = MAX_uint32;
        options.ConvexVertexLimit = 255;
        serializeFailed = CollisionDataDocument::DecodeRuntime(options, document, error);
    }
    else
    {
        MemoryWriteStream stream(256);
        if (typeName == Animation::TypeName)
        {
            stream.Write(103);
            stream.Write(5 * 60.0);
            stream.Write(60.0);
            stream.Write(static_cast<byte>(0));
            stream.Write(StringView::Empty, 13);
            stream.WriteInt32(0);
            stream.WriteInt32(0);
            stream.WriteInt32(0);
        }
        else
        {
            stream.WriteInt32(0);
        }
        Array<Guid> references;
        serializeFailed = WriteRuntimePayloadSource(typeName, ToSpan(stream), references, source, diagnostic);
    }
    if (serializeFailed)
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            FailAuthoredSerialization(diagnostic, error);
        return fail();
    }
    if (source.IsEmpty() && WriteSemanticAuthoredSource(document, typeName, source, diagnostic))
        return fail();

    AssetMeta metadata;
    metadata.ID = Guid::New();
    metadata.AssetType = typeName;
    metadata.SourceKind = AssetSourceKind::TextDocument;
    metadata.Processor.ID = processorID;
    metadata.Processor.SettingsVersion = 1;
    metadata.Processor.SettingsJson = "{}\n";
    AssetMutationResult mutation;
    if (GetMutationService().CreateAsset(outputPath, StringAnsiView(source.Get(), source.Length()), metadata, mutation))
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = outputPath;
        diagnostic.AssetGuid = metadata.ID;
        diagnostic.Message = mutation.Message;
        return fail();
    }
    if (GraphPipelineService::RequestBuild(metadata.ID, true, diagnostic))
        return fail();
    return metadata.ID;
#else
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
    diagnostic.Message = TEXT("Authored documents require the editor importer.");
    return fail();
#endif
}

bool AssetDatabaseFacade::SaveAuthoredDocument(BinaryAsset* asset, const Guid& canonicalAssetID)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    AssetRecord record;
    if (!asset || !canonicalAssetID.IsValid() || !AssetDatabase::Get().TryGetRecord(canonicalAssetID, record) ||
        record.SourceKind != AssetSourceKind::TextDocument ||
        (record.ProcessorID != TEXT("Flax.MaterialInstance") && record.ProcessorID != TEXT("Flax.SkeletonMask") &&
            record.ProcessorID != TEXT("Flax.SceneAnimation") && record.ProcessorID != TEXT("Flax.ParticleSystem") &&
            record.ProcessorID != TEXT("Flax.Animation") && record.ProcessorID != TEXT("Flax.GameplayGlobals")))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = TEXT("The edited asset is not backed by a canonical authored document.");
        return fail();
    }

    StringAnsi source;
    AuthoredJsonDocument document;
    String error;
    bool serializeFailed = false;
    if (record.TypeName == MaterialInstance::TypeName)
    {
        auto* typed = ScriptingObject::Cast<MaterialInstance>(asset);
        serializeFailed = !typed || DecodeMaterialInstanceSource(typed, document, error);
    }
    else if (record.TypeName == SkeletonMask::TypeName)
    {
        auto* typed = ScriptingObject::Cast<SkeletonMask>(asset);
        serializeFailed = !typed;
        if (typed)
            DecodeSkeletonMaskSource(typed, document);
    }
    else if (record.TypeName == SceneAnimation::TypeName)
    {
        auto* typed = ScriptingObject::Cast<SceneAnimation>(asset);
        if (typed)
        {
            const BytesContainer& timeline = typed->LoadTimeline();
            serializeFailed = timeline.IsInvalid() || SceneAnimationDocument::DecodeRuntime(
                Span<byte>(timeline.Get(), timeline.Length()), document, error);
        }
        else
            serializeFailed = true;
    }
    else if (record.TypeName == ParticleSystem::TypeName)
    {
        auto* typed = ScriptingObject::Cast<ParticleSystem>(asset);
        if (typed)
        {
            const BytesContainer timeline = typed->LoadTimeline();
            serializeFailed = timeline.IsInvalid() || ParticleSystemDocument::DecodeRuntime(
                Span<byte>(timeline.Get(), timeline.Length()), document, error);
        }
        else
            serializeFailed = true;
    }
    else if (record.TypeName == Animation::TypeName)
    {
        auto* typed = ScriptingObject::Cast<Animation>(asset);
        serializeFailed = !typed;
        if (typed)
        {
            MemoryWriteStream stream(4096);
            SerializeAnimationChunk(typed, stream);
            Array<Guid> references;
            Array<String> referencedFiles;
            typed->GetReferences(references, referencedFiles);
            serializeFailed = WriteRuntimePayloadSource(record.TypeName, ToSpan(stream), references, source, diagnostic);
        }
    }
    else if (record.TypeName == GameplayGlobals::TypeName)
    {
        auto* typed = ScriptingObject::Cast<GameplayGlobals>(asset);
        serializeFailed = !typed;
        if (typed)
        {
            MemoryWriteStream stream(1024);
            SerializeGameplayGlobalsChunk(typed, stream);
            Array<Guid> references;
            Array<String> referencedFiles;
            typed->GetReferences(references, referencedFiles);
            serializeFailed = WriteRuntimePayloadSource(record.TypeName, ToSpan(stream), references, source, diagnostic);
        }
    }
    if (serializeFailed)
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            FailAuthoredSerialization(diagnostic, error.HasChars() ? StringView(error) : TEXT("The edited asset could not be serialized as a canonical source document."));
        diagnostic.AssetGuid = canonicalAssetID;
        return fail();
    }
    if (source.IsEmpty() && WriteSemanticAuthoredSource(document, record.TypeName, source, diagnostic))
        return fail();
    if (FileSystem::FileExists(record.SourcePath.Get()) && FileSystem::IsReadOnly(record.SourcePath.Get()))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The canonical authored document is read-only.");
        return fail();
    }
    AssetMutationResult mutation;
    if (GetMutationService().ReplaceContents(record.SourcePath.Get(), StringAnsiView(source.Get(), source.Length()), mutation))
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = mutation.Message;
        return fail();
    }
    if (GraphPipelineService::RequestBuild(canonicalAssetID, false, diagnostic))
        return fail();
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::SaveMaterialDocument(Material* asset, const Guid& canonicalAssetID)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER && COMPILE_WITH_MATERIAL_GRAPH
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    AssetRecord record;
    if (!asset || !canonicalAssetID.IsValid() || !AssetDatabase::Get().TryGetRecord(canonicalAssetID, record) ||
        record.SourceKind != AssetSourceKind::TextDocument || record.ProcessorID != TEXT("Flax.GraphDocument") ||
        record.TypeName != Material::TypeName)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = TEXT("The edited material is not backed by a canonical graph document.");
        return fail();
    }

    const BytesContainer existingSurface = asset->LoadSurface(true);
    if (existingSurface.IsInvalid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The edited material graph could not be loaded.");
        return fail();
    }

    MaterialGraph graph;
    MemoryReadStream readStream(existingSurface);
    if (graph.Load(&readStream, true))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The edited material graph could not be decoded.");
        return fail();
    }
    for (const MaterialParameter& materialParameter : asset->Params)
    {
        GraphParameter* graphParameter = graph.GetParameter(materialParameter.GetParameterID());
        if (!graphParameter)
            continue;

        graphParameter->Value = materialParameter.GetValue();
        if (graphParameter->Value.Type == VariantType::Object)
            graphParameter->Value = graphParameter->Value.AsObject ? graphParameter->Value.AsObject->GetID() : Guid::Empty;
        else if (graphParameter->Value.Type == VariantType::Asset)
            graphParameter->Value = graphParameter->Value.AsObject ? graphParameter->Value.AsObject->GetID() : Guid::Empty;
    }

    MemoryWriteStream writeStream(existingSurface.Length());
    if (graph.Save(&writeStream, true))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The edited material graph could not be encoded.");
        return fail();
    }
    BytesContainer surface;
    surface.Link(ToSpan(writeStream));
    return SaveGraphSurface(record.SourcePath.Get(), surface);
#else
    return true;
#endif
}

bool AssetDatabaseFacade::SaveCollisionDataDocument(const StringView& path, CollisionDataType type, const Guid& model, int32 modelLodIndex,
    uint32 materialSlotsMask, ConvexMeshGenerationFlags convexFlags, int32 convexVertexLimit)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    CollisionData::SerializedOptions options;
    Platform::MemoryClear(&options, sizeof(options));
    options.Type = type;
    options.Model = model;
    options.ModelLodIndex = modelLodIndex;
    options.MaterialSlotsMask = materialSlotsMask;
    options.ConvexFlags = convexFlags;
    options.ConvexVertexLimit = convexVertexLimit;
    rapidjson_flax::Document json;
    String error;
    if (CollisionDataDocument::DecodeLegacy(options, json, error))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = error;
        return fail();
    }
    StringAnsi text;
    CanonicalJsonError jsonError;
    Array<StringAnsi> order;
    order.Add("documentVersion");
    order.Add("type");
    order.Add("collisionType");
    order.Add("sourceModel");
    order.Add("modelLodIndex");
    order.Add("materialSlotsMask");
    order.Add("convexFlags");
    order.Add("convexVertexLimit");
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = jsonError.Message;
        return fail();
    }
    AssetMutationResult mutation;
    if (GetMutationService().ReplaceContents(path, StringAnsiView(text.Get(), text.Length()), mutation))
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = mutation.Message;
        return fail();
    }
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.CollisionData"))
        return fail();
    if (GraphPipelineService::RequestBuildAndWait(meta.ID, false, diagnostic))
        return fail();
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::SaveParticleSystemTimeline(const StringView& path, const BytesContainer& timeline)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    rapidjson_flax::Document json;
    String error;
    if (ParticleSystemDocument::DecodeLegacy(Span<byte>(timeline.Get(), timeline.Length()), json, error))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = error;
        return fail();
    }
    StringAnsi text;
    CanonicalJsonError jsonError;
    Array<StringAnsi> order;
    order.Add("documentVersion");
    order.Add("type");
    order.Add("framesPerSecond");
    order.Add("durationFrames");
    order.Add("tracks");
    order.Add("parameterOverrides");
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = jsonError.Message;
        return fail();
    }
    AssetMutationResult mutation;
    if (GetMutationService().ReplaceContents(path, StringAnsiView(text.Get(), text.Length()), mutation))
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = mutation.Message;
        return fail();
    }
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.ParticleSystem"))
        return fail();
    return GraphPipelineService::RequestBuild(meta.ID, false, diagnostic) ? fail() : false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::LoadCollisionDataDocument(const StringView& path, CollisionData::SerializedOptions& options)
{
    Array<byte> bytes;
    if (File::ReadAllBytes(path, bytes))
        return true;
    rapidjson_flax::Document json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    String error;
    return json.HasParseError() || CollisionDataDocument::Parse(json, options, error);
}

Guid AssetDatabaseFacade::CreateImportedSourceMetadata(const StringView& sourcePath, const StringView& typeName, const StringView& processorId)
{
#if !USE_EDITOR
    return Guid::Empty;
#else
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath) || typeName.IsEmpty() || processorId.IsEmpty())
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Imported source or processor identity is invalid, or metadata already exists.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = processorId == TEXT("Flax.Text") ? AssetSourceKind::TextDocument : AssetSourceKind::ImportedSource;
    meta.Processor.ID = processorId;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (RegisterExistingMetadata(sourcePath, meta, false, diagnostic) || RefreshPath(sourcePath))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
#endif
    return meta.ID;
#endif
}

Guid AssetDatabaseFacade::CreateImportedSourceBytes(const StringView& sourcePath, const BytesContainer& sourceContents,
    const StringView& typeName, const StringView& processorId)
{
#if !USE_EDITOR
    return Guid::Empty;
#else
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!AssetDatabase::Get().IsHardCutEnabled() || sourcePath.IsEmpty() || sourceContents.Length() == 0 ||
        typeName.IsEmpty() || processorId.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Journaled imported-source creation requires Asset System v3 and valid source bytes, type, processor, and Content path.");
        return fail();
    }

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = processorId == TEXT("Flax.Text") ? AssetSourceKind::TextDocument : AssetSourceKind::ImportedSource;
    meta.Processor.ID = processorId;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    AssetMutationResult mutation;
    GetMutationService().CreateAsset(sourcePath,
        StringAnsiView(reinterpret_cast<const char*>(sourceContents.Get()), sourceContents.Length()), meta, mutation);
    if (!mutation.Succeeded)
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = meta.ID;
        diagnostic.Message = mutation.Message;
        return fail();
    }
#if COMPILE_WITH_ASSETS_IMPORTER
    if (processorId != TEXT("Flax.Unsupported") && GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
        return fail();
#endif
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return meta.ID;
#endif
}

bool AssetDatabaseFacade::ReplaceCanonicalSource(const StringView& sourcePath, const StringAnsiView& sourceContents)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (!AssetDatabase::Get().IsHardCutEnabled() || sourcePath.IsEmpty() || sourceContents.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Canonical source replacement requires Asset System v3, source bytes, and a valid Content path.");
        return fail();
    }
    AssetMutationResult mutation;
    GetMutationService().ReplaceContents(sourcePath, sourceContents, mutation);
    if (!mutation.Succeeded)
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = mutation.AssetID;
        diagnostic.Message = mutation.Message;
        return fail();
    }
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return false;
}

#if COMPILE_WITH_AUDIO_TOOL && USE_EDITOR
Guid AssetDatabaseFacade::CreateAudioMetadata(const StringView& sourcePath, const AudioTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Audio metadata already exists.") : TEXT("Audio source does not exist.");
        return fail();
    }

    rapidjson_flax::StringBuffer settingsBuffer;
    CompactJsonWriter settingsWriter(settingsBuffer);
    settingsWriter.StartObject();
    AudioTool::Options serializedOptions = options;
    serializedOptions.Serialize(settingsWriter, nullptr);
    settingsWriter.EndObject();

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = TEXT("FlaxEngine.AudioClip");
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TEXT("Flax.Audio");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = StringAnsi(settingsBuffer.GetString(), static_cast<int32>(settingsBuffer.GetSize()));
    if (RegisterExistingMetadata(sourcePath, meta, false, diagnostic) || RefreshPath(sourcePath))
        return fail();
#if COMPILE_WITH_ASSETS_IMPORTER
    if (GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
        return fail();
#endif
    return meta.ID;
}

AssetMutationResultInfo AssetDatabaseFacade::PublishExternalAudio(const StringView& externalSourcePath,
    const StringView& destinationPath, const AudioTool::Options& options, bool replaceExisting)
{
    rapidjson_flax::StringBuffer settingsBuffer;
    CompactJsonWriter settingsWriter(settingsBuffer);
    settingsWriter.StartObject();
    AudioTool::Options serializedOptions = options;
    serializedOptions.Serialize(settingsWriter, nullptr);
    settingsWriter.EndObject();

    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    const String existingMetaPath = String(destinationPath) + TEXT(".meta");
    if (!replaceExisting || !FileSystem::FileExists(existingMetaPath) || AssetMeta::Load(existingMetaPath, meta, diagnostic))
        meta.ID = Guid::New();
    meta.AssetType = TEXT("FlaxEngine.AudioClip");
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TEXT("Flax.Audio");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = StringAnsi(settingsBuffer.GetString(), static_cast<int32>(settingsBuffer.GetSize()));
    AssetMutationResult result;
    GetMutationService().PublishExternal(externalSourcePath, destinationPath, meta, replaceExisting, result);
#if COMPILE_WITH_ASSETS_IMPORTER
    if (result.Succeeded && GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
    {
        result.Succeeded = false;
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
    }
#endif
    return ToInfo(result);
}

bool AssetDatabaseFacade::LoadAudioMetadata(const StringView& sourcePath, AudioTool::Options& options)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (AssetMeta::Load(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
        return fail();
    if (meta.Processor.ID != TEXT("Flax.Audio") || meta.Processor.SettingsVersion != 1)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = meta.Processor.ID;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Audio metadata is not owned by the supported Flax.Audio processor version.");
        return fail();
    }
    rapidjson_flax::Document settings;
    settings.Parse(meta.Processor.SettingsJson.Get(), meta.Processor.SettingsJson.Length());
    if (settings.HasParseError() || !settings.IsObject())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = meta.Processor.ID;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Audio processor settings are malformed.");
        return fail();
    }
    options.Deserialize(settings, nullptr);
    return false;
}
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
namespace
{
    Guid CreateModelMetadataInternal(const StringView& sourcePath, const ModelTool::Options& requestedOptions, bool inferRootType)
    {
        AssetPipelineDiagnostic diagnostic;
        auto fail = [&diagnostic]()
        {
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(diagnostic);
            SetDiagnostics(diagnostics);
            return Guid::Empty;
        };
        const String metaPath = String(sourcePath) + TEXT(".meta");
        if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
        {
            diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Model metadata already exists.") : TEXT("Model source does not exist.");
            return fail();
        }

        ModelTool::Options options = requestedOptions;
        ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
        if (settings.Validate(diagnostic))
            return fail();
        ModelSourceAnalysis analysis;
        if (ModelProcessor::AnalyzeSource(sourcePath, settings, analysis, diagnostic))
            return fail();
        if (inferRootType)
        {
            options.Type = analysis.SourceSkeletonBoneCount > 0 || analysis.SourceAnimationCount > 0
                ? ModelTool::ModelType::SkinnedModel
                : ModelTool::ModelType::Model;
            settings = ModelProcessorSettings::FromLegacyOptions(options);
        }

        AssetMeta meta;
        meta.ID = Guid::New();
        meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel || options.Type == ModelTool::ModelType::Animation
            ? SkinnedModel::TypeName
            : Model::TypeName;
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = ModelProcessorSettings::ProcessorID();
        meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
        if (settings.ToJson(meta.Processor.SettingsJson, diagnostic))
            return fail();
        SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, analysis.Candidates, true);
        if (reconciliation.RequiresUserReconciliation)
        {
            diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : diagnostic;
            diagnostic.AssetGuid = meta.ID;
            diagnostic.SourcePath = sourcePath;
            return fail();
        }
        meta.SubAssets = MoveTemp(reconciliation.Resolved);
        if (RegisterExistingMetadata(sourcePath, meta, false, diagnostic) || RefreshPath(sourcePath))
            return fail();
#if COMPILE_WITH_ASSETS_IMPORTER
        if (ModelPipelineService::RequestBuild(meta.ID, false, diagnostic))
            return fail();
#endif
        return meta.ID;
    }
}

Guid AssetDatabaseFacade::CreateDefaultModelMetadata(const StringView& sourcePath)
{
    ModelTool::Options options;
    return CreateModelMetadataInternal(sourcePath, options, true);
}

Guid AssetDatabaseFacade::CreateModelMetadata(const StringView& sourcePath, const ModelTool::Options& options)
{
    return CreateModelMetadataInternal(sourcePath, options, false);
}

AssetMutationResultInfo AssetDatabaseFacade::PublishExternalModel(const StringView& externalSourcePath,
    const StringView& destinationPath, const ModelTool::Options& options, bool replaceExisting)
{
    AssetPipelineDiagnostic diagnostic;
    ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
    AssetMutationResult result;
    if (settings.Validate(diagnostic))
    {
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return ToInfo(result);
    }
    ModelSourceAnalysis analysis;
    if (ModelProcessor::AnalyzeSource(externalSourcePath, settings, analysis, diagnostic))
    {
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return ToInfo(result);
    }

    AssetMeta meta;
    const String existingMetaPath = String(destinationPath) + TEXT(".meta");
    if (!replaceExisting || !FileSystem::FileExists(existingMetaPath) || AssetMeta::Load(existingMetaPath, meta, diagnostic))
        meta.ID = Guid::New();
    meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel || options.Type == ModelTool::ModelType::Animation
        ? SkinnedModel::TypeName
        : Model::TypeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = ModelProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic))
    {
        result.Message = diagnostic.Message;
        return ToInfo(result);
    }
    SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, analysis.Candidates, true);
    if (reconciliation.RequiresUserReconciliation)
    {
        diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : diagnostic;
        diagnostic.AssetGuid = meta.ID;
        diagnostic.SourcePath = externalSourcePath;
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return ToInfo(result);
    }
    meta.SubAssets = MoveTemp(reconciliation.Resolved);
    GetMutationService().PublishExternal(externalSourcePath, destinationPath, meta, replaceExisting, result);
#if COMPILE_WITH_ASSETS_IMPORTER
    if (result.Succeeded && ModelPipelineService::RequestBuild(meta.ID, false, diagnostic))
    {
        result.Succeeded = false;
        result.Message = diagnostic.Message;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
    }
#endif
    return ToInfo(result);
}

Guid AssetDatabaseFacade::StageLegacyModelMigration(const StringView& legacyPath, const StringView& extractedPath,
    const StringView& destinationPath, const StringView& backupPath, const ModelTool::Options& options)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
        return fail();
    AssetMeta meta;
    meta.Processor.ID = ModelProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic) || LegacyAssetMigrator::SeedModelSubAssets(legacyPath, meta, diagnostic))
        return fail();

    ModelTool::Options probeOptions = options;
    probeOptions.Type = ModelTool::ModelType::Prefab;
    probeOptions.ImportTypes = ImportDataTypes::Geometry | ImportDataTypes::Skeleton | ImportDataTypes::Animations |
                               ImportDataTypes::Nodes | ImportDataTypes::Materials | ImportDataTypes::Textures;
    ModelData sourceData;
    String importError;
    if (ModelTool::ImportData(String(extractedPath), sourceData, probeOptions, importError))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = extractedPath;
        diagnostic.Message = importError.IsEmpty() ? TEXT("Extracted GLB could not be parsed.") : importError;
        return fail();
    }
    Array<ModelSubAssetInfo> infos;
    Array<SubAssetCandidate> candidates;
    if (ModelSubAssetKeys::Enumerate(sourceData, infos, candidates, diagnostic))
        return fail();
    const Dictionary<String, SubAssetMeta> legacyMappings = meta.SubAssets;
    Dictionary<String, SubAssetMeta> mapped;
    HashSet<int64> used;
    for (const SubAssetCandidate& candidate : candidates)
    {
        const SubAssetMeta* selected = nullptr;
        for (const auto& existing : legacyMappings)
        {
            if (!used.Contains(existing.Value.LocalId) && existing.Value.TypeName == candidate.TypeName && existing.Value.DisplayName == candidate.DisplayName)
            {
                selected = &existing.Value;
                break;
            }
        }
        if (!selected)
        {
            for (const auto& existing : legacyMappings)
            {
                if (!used.Contains(existing.Value.LocalId) && existing.Value.TypeName == candidate.TypeName)
                {
                    selected = &existing.Value;
                    break;
                }
            }
        }
        if (selected)
        {
            SubAssetMeta value = *selected;
            value.DisplayName = candidate.DisplayName;
            value.Removed = false;
            mapped.Add(candidate.StableKey, MoveTemp(value));
            used.Add(selected->LocalId);
        }
    }
    for (const auto& existing : legacyMappings)
    {
        if (used.Contains(existing.Value.LocalId))
            continue;
        SubAssetMeta tombstone = existing.Value;
        tombstone.Removed = true;
        mapped.Add(String::Format(TEXT("legacy:{0}"), tombstone.LocalId), MoveTemp(tombstone));
    }
    meta.SubAssets = MoveTemp(mapped);
    if (StageImportedFiles(legacyPath, extractedPath, destinationPath, backupPath, meta, diagnostic))
        return fail();
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return meta.ID;
}
#endif

BytesContainer AssetDatabaseFacade::LoadGraphSurface(const StringView& path)
{
    BytesContainer result;
    AssetPipelineDiagnostic diagnostic;
    GraphDocumentSession session;
    if (session.Open(path, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return result;
    }
    Array<byte> surface;
    if (GraphDocumentCompiler::CompileDocument(session.Document, surface, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return result;
    }
    result.Copy(ToSpan(surface));
    return result;
}

bool AssetDatabaseFacade::SaveGraphSurface(const StringView& path, const BytesContainer& surface, bool allowOverwriteConflict, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    String typeName;
    const String extension = FileSystem::GetExtension(path);
    if (GraphDocumentCodec::TypeForExtension(extension, typeName))
    {
        AssetMeta meta;
        if (!AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic))
            typeName = meta.AssetType;
    }
    GraphDocument document;
    if (GraphDocumentCodec::FromSurface(typeName, surface, document, diagnostic))
        return fail();
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    else if (FileSystem::FileExists(path))
    {
        GraphDocumentSession session;
        AssetPipelineDiagnostic ignored;
        if (!session.Open(path, ignored))
            document.PropertiesJson = session.Document.PropertiesJson;
    }
    StringAnsi json;
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic))
        return fail();
    AssetMutationResult mutation;
    if (GetMutationService().ReplaceContents(path, StringAnsiView(json.Get(), json.Length()), mutation))
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = mutation.Message;
        return fail();
    }
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetMeta meta;
    if (!AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) && GraphPipelineService::RequestBuild(meta.ID, false, diagnostic))
        return fail();
#endif
    return false;
}

bool AssetDatabaseFacade::BuildGraph(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(assetID, false, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::BuildAsset(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = assetID;
        diagnostic.Message = TEXT("Canonical asset is not registered.");
    }
#if COMPILE_WITH_TEXTURE_TOOL
    else if (record.ProcessorID == TextureProcessorSettings::ProcessorID())
    {
        if (!TexturePipelineService::RequestBuild(assetID, false, diagnostic))
            return false;
    }
#endif
#if COMPILE_WITH_MODEL_TOOL
    else if (record.ProcessorID == ModelProcessorSettings::ProcessorID())
    {
        if (!ModelPipelineService::RequestBuild(assetID, false, diagnostic))
            return false;
    }
#endif
    else if (GraphPipelineService::OwnsProcessor(record.ProcessorID))
    {
        if (!GraphPipelineService::RequestBuild(assetID, false, diagnostic))
            return false;
    }
    else
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.ProcessorId = record.ProcessorID;
        diagnostic.Message = TEXT("Canonical asset has no supported build pipeline.");
    }
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
    return true;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::RebuildGraph(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(assetID, true, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    return false;
#else
    return true;
#endif
}

String AssetDatabaseFacade::GetGraphBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    switch (GraphPipelineService::GetStatus(assetID, diagnostic))
    {
    case AssetBuildJobStatus::Queued: return TEXT("Queued");
    case AssetBuildJobStatus::Building: return TEXT("Building");
    case AssetBuildJobStatus::Publishing: return TEXT("Publishing");
    case AssetBuildJobStatus::Succeeded: return TEXT("ReadyExact");
    case AssetBuildJobStatus::Failed: return TEXT("Failed");
    case AssetBuildJobStatus::Cancelled: return TEXT("Cancelled");
    default: break;
    }
#endif
    return TEXT("NotBuilt");
}

AssetPipelineDiagnostic AssetDatabaseFacade::GetGraphBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    GraphPipelineService::GetStatus(assetID, diagnostic);
#endif
    return diagnostic;
}

String AssetDatabaseFacade::GetMigrationInventoryJson()
{
    LoadOrScan(false);
    Array<MigrationInventoryEntry> entries;
    MigrationInventory::Build(AssetDatabase::Get().GetSnapshot().Records, entries);
    StringAnsi json;
    AssetPipelineDiagnostic diagnostic;
    if (MigrationInventory::WriteCanonicalJson(entries, json, diagnostic))
        return String::Empty;
    return String(json);
}

bool AssetDatabaseFacade::MigrateLegacyAsset(const StringView& sourcePath)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (RejectPostCutoverLegacyMutation(sourcePath, diagnostic))
        return fail();
    if (sourcePath.IsEmpty() || EnsureDatabaseLoaded())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Legacy migration requires a source path and database snapshot.");
        return fail();
    }

    AssetRecord record;
    bool found = false;
    for (const AssetRecord& candidate : AssetDatabase::Get().GetSnapshot().Records)
    {
        if (candidate.IsMainAsset() && FileSystem::AreFilePathsEqual(candidate.SourcePath.Get(), sourcePath))
        {
            record = candidate;
            found = true;
            break;
        }
    }
    if (!found)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("The requested root asset is not registered.");
        return fail();
    }
    const Guid assetID = record.ID;
    String reason, destination;
    if (MigrationInventory::Classify(record, reason, destination) != MigrationEligibility::ReadyToMigrate)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = reason;
        return fail();
    }
    const String destinationMeta = destination + TEXT(".meta");
    if (FileSystem::FileExists(destination) || FileSystem::FileExists(destinationMeta))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destination;
        diagnostic.Message = TEXT("The canonical migration destination already exists.");
        return fail();
    }
    if (FileSystem::IsReadOnly(record.SourcePath.Get()))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The legacy asset is read-only.");
        return fail();
    }

    const String token = Guid::New().ToString(Guid::FormatType::N);
    const String stagedDocument = destination + TEXT(".migration-") + token;
    const String stagedMeta = stagedDocument + TEXT(".meta");
    SCOPE_EXIT
    {
        FileSystem::DeleteFile(stagedDocument);
        FileSystem::DeleteFile(stagedMeta);
    };
    if (LegacyAssetMigrator::ConvertFlax(record.SourcePath.Get(), stagedDocument, assetID, record.TypeName, diagnostic))
        return fail();
    if (FileSystem::MoveFile(destination, stagedDocument, false))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destination;
        diagnostic.Message = TEXT("The canonical migration document could not be committed.");
        return fail();
    }
    if (FileSystem::MoveFile(destinationMeta, stagedMeta, false))
    {
        FileSystem::MoveFile(stagedDocument, destination, false);
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destinationMeta;
        diagnostic.Message = TEXT("The canonical migration sidecar could not be committed.");
        return fail();
    }

    ContentStorageManager::EnsureAccess(record.SourcePath.Get());
    if (FileSystem::DeleteFile(record.SourcePath.Get()))
    {
        FileSystem::DeleteFile(destinationMeta);
        FileSystem::DeleteFile(destination);
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The legacy binary could not be removed; canonical outputs were rolled back.");
        return fail();
    }
    SetDiagnostics(Array<AssetPipelineDiagnostic>());

    // The database is loaded once per migration run, so this conversion has to be folded back in
    // here or the next asset would see a stale snapshot.
    Array<String> refreshPaths;
    refreshPaths.Add(record.SourcePath.Get());
    refreshPaths.Add(destination);
    refreshPaths.Add(destinationMeta);
    if (RefreshSources(refreshPaths))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destination;
        diagnostic.Message = TEXT("The asset database could not be updated after migration.");
        return fail();
    }
    return false;
}

bool AssetDatabaseFacade::FinalizeLegacyImportedMigration(const StringView& backupPath)
{
    AssetPipelineDiagnostic diagnostic;
    if (RejectPostCutoverLegacyMutation(backupPath, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    if (backupPath.IsEmpty() || !FileSystem::FileExists(backupPath))
        return true;
    ContentStorageManager::EnsureAccess(backupPath);
    return FileSystem::DeleteFile(backupPath);
}

bool AssetDatabaseFacade::RollbackLegacyImportedMigration(const StringView& legacyPath, const StringView& destinationPath, const StringView& backupPath)
{
    AssetPipelineDiagnostic diagnostic;
    if (RejectPostCutoverLegacyMutation(legacyPath, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    const String metaPath = String(destinationPath) + TEXT(".meta");
    ContentStorageManager::EnsureAccess(destinationPath);
    ContentStorageManager::EnsureAccess(backupPath);
    Array<String> refreshPaths;
    refreshPaths.Add(metaPath);
    refreshPaths.Add(String(destinationPath));
    refreshPaths.Add(String(legacyPath));
    refreshPaths.Add(String(backupPath));
    const bool failed = FileSystem::DeleteFile(metaPath) || FileSystem::DeleteFile(destinationPath) ||
                        FileSystem::MoveFile(legacyPath, backupPath, false) || RefreshSources(refreshPaths);
    if (failed)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = legacyPath;
        diagnostic.Message = TEXT("Staged imported migration rollback failed.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    else
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>());
    }
    return failed;
}

Guid AssetDatabaseFacade::CreateExistingJsonMetadata(const StringView& sourcePath)
{
#if !USE_EDITOR
    return Guid::Empty;
#else
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    Guid sourceHeaderID;
    String typeName;
    if (!JsonStorageProxy::GetAssetInfo(sourcePath, sourceHeaderID, typeName) || !sourceHeaderID.IsValid() || typeName.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("JSON asset is missing a valid ID and TypeName header.");
        return fail();
    }
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::ExistingJson;
    meta.Processor.ID = TEXT("Flax.ExistingJson");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (RegisterExistingMetadata(sourcePath, meta, false, diagnostic))
        return fail();
    return meta.ID;
#endif
}

bool AssetDatabaseFacade::SaveExistingJsonSource(const StringView& sourcePath, const StringAnsiView& sourceContents,
    const Guid& sourceID, const StringView& typeName)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (!AssetDatabase::Get().IsHardCutEnabled() || sourcePath.IsEmpty() || sourceContents.Length() == 0 ||
        !sourceID.IsValid() || typeName.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = sourceID;
        diagnostic.Message = TEXT("Journaled existing-JSON publication requires Asset System v3 and a valid identity, type, path, and source payload.");
        return fail();
    }

    AssetMutationResult mutation;
    const String metaPath = String(sourcePath) + TEXT(".meta");
    if (FileSystem::FileExists(sourcePath))
    {
        AssetMeta existing;
        if (!FileSystem::FileExists(metaPath) || AssetMeta::Load(metaPath, existing, diagnostic) ||
            existing.ID != sourceID || existing.AssetType != typeName || existing.SourceKind != AssetSourceKind::ExistingJson)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                diagnostic.SourcePath = sourcePath;
                diagnostic.AssetGuid = sourceID;
                diagnostic.Message = TEXT("Existing JSON metadata is missing or does not match the document identity and type.");
            }
            return fail();
        }
        GetMutationService().ReplaceContents(sourcePath, sourceContents, mutation);
    }
    else
    {
        if (FileSystem::FileExists(metaPath))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.SourcePath = metaPath;
            diagnostic.AssetGuid = sourceID;
            diagnostic.Message = TEXT("Existing JSON metadata is orphaned at the requested creation path.");
            return fail();
        }
        AssetMeta metadata;
        metadata.ID = sourceID;
        metadata.AssetType = typeName;
        metadata.SourceKind = AssetSourceKind::ExistingJson;
        metadata.Processor.ID = TEXT("Flax.ExistingJson");
        metadata.Processor.SettingsVersion = 1;
        metadata.Processor.SettingsJson = "{}\n";
        GetMutationService().CreateAsset(sourcePath, sourceContents, metadata, mutation);
    }
    if (!mutation.Succeeded)
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = sourceID;
        diagnostic.Message = mutation.Message;
        return fail();
    }
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return false;
}

bool AssetDatabaseFacade::SaveExistingJsonSourceWithExternalActors(const StringView& sourcePath,
    const StringAnsiView& sourceContents, const Guid& sourceID, const StringView& typeName,
    const Array<AssetMutationSidecar>& sidecars)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (!AssetDatabase::Get().IsHardCutEnabled() || sourcePath.IsEmpty() || sourceContents.Length() == 0 ||
        !sourceID.IsValid() || typeName != TEXT("FlaxEngine.SceneAsset"))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = sourceID;
        diagnostic.Message = TEXT("Journaled external-actor scene publication requires Asset System v3 and a valid scene identity, path, and source payload.");
        return fail();
    }

    AssetMeta metadata;
    const String metaPath = String(sourcePath) + TEXT(".meta");
    if (FileSystem::FileExists(sourcePath))
    {
        if (!FileSystem::FileExists(metaPath) || AssetMeta::Load(metaPath, metadata, diagnostic) ||
            metadata.ID != sourceID || metadata.AssetType != typeName || metadata.SourceKind != AssetSourceKind::ExistingJson)
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                diagnostic.SourcePath = sourcePath;
                diagnostic.AssetGuid = sourceID;
                diagnostic.Message = TEXT("Existing scene metadata is missing or does not match the document identity and type.");
            }
            return fail();
        }
    }
    else
    {
        if (FileSystem::FileExists(metaPath))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
            diagnostic.SourcePath = metaPath;
            diagnostic.AssetGuid = sourceID;
            diagnostic.Message = TEXT("Existing scene metadata is orphaned at the requested creation path.");
            return fail();
        }
        metadata.ID = sourceID;
        metadata.AssetType = typeName;
        metadata.SourceKind = AssetSourceKind::ExistingJson;
        metadata.Processor.ID = TEXT("Flax.ExistingJson");
        metadata.Processor.SettingsVersion = 1;
        metadata.Processor.SettingsJson = "{}\n";
    }

    AssetMutationResult mutation;
    GetMutationService().SaveExternalActors(sourcePath, sourceContents, metadata, sidecars, mutation);
    if (!mutation.Succeeded)
    {
        diagnostic.Code = mutation.RequiresRecovery ? AssetPipelineDiagnosticCode::RecoveryRequired : AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = sourcePath;
        diagnostic.AssetGuid = sourceID;
        diagnostic.Message = mutation.Message;
        return fail();
    }
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return false;
}

bool AssetDatabaseFacade::SaveExistingJsonSourceBytes(const StringView& sourcePath, const BytesContainer& sourceContents,
    const Guid& sourceID, const StringView& typeName)
{
    return SaveExistingJsonSource(sourcePath,
        StringAnsiView(reinterpret_cast<const char*>(sourceContents.Get()), sourceContents.Length()), sourceID, typeName);
}

bool AssetDatabaseFacade::CommitMetadata(const StringView& sourcePath, const AssetMeta& metadata,
    bool replaceExisting, AssetPipelineDiagnostic& diagnostic)
{
#if USE_EDITOR
    ScopeLock mutationLock(MetadataMutationLocker);
    return metadata.FolderAsset
        ? RegisterExistingMetadata(sourcePath, metadata, replaceExisting, diagnostic)
        : replaceExisting
            ? ReplaceMetadataTransactional(sourcePath, metadata, diagnostic)
            : RegisterExistingMetadata(sourcePath, metadata, false, diagnostic);
#else
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
    diagnostic.SourcePath = sourcePath;
    diagnostic.AssetGuid = metadata.ID;
    diagnostic.Message = TEXT("Metadata mutation is unavailable outside the editor.");
    return true;
#endif
}

bool AssetDatabaseFacade::EnsureExistingJsonSidecars()
{
#if USE_EDITOR
    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
        return true;
    bool failed = false;
    for (const String& path : files)
    {
        const String extension = FileSystem::GetExtension(path).ToLower();
        if (extension != TEXT("scene") && extension != TEXT("prefab") && extension != TEXT("json"))
            continue;
        const String metaPath = path + TEXT(".meta");
        if (FileSystem::FileExists(metaPath))
            continue;
        if (!CreateExistingJsonMetadata(path).IsValid())
            failed = true;
    }
    return failed;
#else
    return true;
#endif
}
