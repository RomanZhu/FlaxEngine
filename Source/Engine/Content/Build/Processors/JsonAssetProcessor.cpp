// Copyright (c) Wojciech Figat. All rights reserved.

#include "JsonAssetProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/AssetDatabase/Identity/GlobalAssetObjectId.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Level/ScenePartitionDocument.h"
#include "Engine/Level/ScenePrefabDocument.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include "FlaxEngine.Gen.h"

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = JsonAssetProcessor::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    const JsonValue* ValidateSourceDocument(const JsonDocument& json, const StringView& expectedType)
    {
        if (!json.IsObject())
            return nullptr;
        const char* versionName;
        const char* payloadName;
        if (expectedType == TEXT("FlaxEngine.SceneAsset"))
        {
            versionName = "sceneVersion";
            payloadName = "objects";
        }
        else if (expectedType == TEXT("FlaxEngine.Prefab"))
        {
            versionName = "prefabVersion";
            payloadName = "objects";
        }
        else
        {
            versionName = "documentVersion";
            payloadName = "data";
            const auto typeMember = json.FindMember("type");
            if (typeMember == json.MemberEnd() || !typeMember->value.IsString() ||
                String(StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength())) != expectedType)
                return nullptr;
        }
        const auto versionMember = json.FindMember(versionName);
        const auto payloadMember = json.FindMember(payloadName);
        const bool isSceneOrPrefab = expectedType == TEXT("FlaxEngine.SceneAsset") || expectedType == TEXT("FlaxEngine.Prefab");
        if (versionMember == json.MemberEnd() || !versionMember->value.IsUint() || versionMember->value.GetUint() < 1 ||
            payloadMember == json.MemberEnd() || (isSceneOrPrefab ? !payloadMember->value.IsArray() :
                (!payloadMember->value.IsObject() && !payloadMember->value.IsArray())))
            return nullptr;
        if (isSceneOrPrefab)
        {
            String error;
            if (ScenePrefabDocument::ValidateObjects(payloadMember->value,
                expectedType == TEXT("FlaxEngine.SceneAsset"), error))
                return nullptr;
        }
        return &payloadMember->value;
    }

    bool CollectReferences(const JsonValue& value, const Guid& currentSource, HashSet<AssetObjectId>& references)
    {
        if (value.IsArray())
        {
            for (const JsonValue& item : value.GetArray())
            {
                if (CollectReferences(item, currentSource, references))
                    return true;
            }
            return false;
        }
        if (!value.IsObject())
            return false;

        const auto guidMember = value.FindMember("guid");
        const auto fileIDMember = value.FindMember("fileId");
        if (guidMember != value.MemberEnd())
        {
            Guid guid;
            if (guidMember == value.MemberEnd() || fileIDMember == value.MemberEnd() || !guidMember->value.IsString() ||
                Guid::Parse(StringAnsiView(guidMember->value.GetString(), guidMember->value.GetStringLength()), guid) ||
                !fileIDMember->value.IsInt64())
                return true;
            const int64 fileID = fileIDMember->value.GetInt64();
            if (!guid.IsValid() && fileID == 0)
                return false;
            AssetObjectId objectID(AssetGuid(guid), fileID);
            if (!objectID.IsValid())
                return true;
            const auto kindMember = value.FindMember("kind");
            if (kindMember != value.MemberEnd())
            {
                const auto instanceMember = value.FindMember("prefabInstanceFileId");
                if (!kindMember->value.IsInt() || (instanceMember != value.MemberEnd() && !instanceMember->value.IsInt64()) ||
                    kindMember->value.GetInt() < static_cast<int32>(GlobalObjectKind::ImportedAssetObject) ||
                    kindMember->value.GetInt() > static_cast<int32>(GlobalObjectKind::BuiltinObject))
                    return true;
                const auto kind = static_cast<GlobalObjectKind>(kindMember->value.GetInt());
                if (kind == GlobalObjectKind::SceneObject || kind == GlobalObjectKind::PrefabObject)
                {
                    if (guid == currentSource)
                        objectID = AssetObjectId();
                    else
                        objectID = AssetObjectId::Main(AssetGuid(guid));
                }
            }
            if (!objectID.IsValid())
                return false;
            references.Add(objectID);
        }
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
        {
            if (CollectReferences(member->value, currentSource, references))
                return true;
        }
        return false;
    }

    bool BuildRuntimeEnvelope(const JsonDocument& source, const Guid& id, const StringView& typeName,
        const JsonAssetPreparedPayload* payload, StringAnsi& output)
    {
        const JsonValue* dataValue = ValidateSourceDocument(source, typeName);
        if (!dataValue)
            return true;
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(id);
        writer.JKEY("TypeName");
        const StringAnsi typeAnsi(typeName);
        writer.String(typeAnsi.Get(), typeAnsi.Length());
        writer.JKEY("EngineBuild");
        writer.Int(FLAXENGINE_VERSION_BUILD);
        const bool isScene = typeName == TEXT("FlaxEngine.SceneAsset");
        const bool isPrefab = typeName == TEXT("FlaxEngine.Prefab");
        if (isScene)
        {
            const auto external = source.FindMember("externalActors");
            if (external != source.MemberEnd() && (!payload || !payload->SceneUsesPartitions))
            {
                if (!external->value.IsBool())
                    return true;
                writer.JKEY("ExternalActors");
                writer.Bool(external->value.GetBool());
            }
        }
        writer.JKEY("Data");
        if (isScene || isPrefab)
        {
            JsonDocument runtime;
            runtime.SetArray();
            JsonValue objects;
            String error;
            if (ScenePrefabDocument::ToRuntimeObjects(*dataValue, objects, runtime.GetAllocator(), isScene, error))
                return true;
            if (isScene && payload && payload->ScenePartitions.HasItems())
            {
                HashSet<int64> objectIds;
                for (const JsonValue& object : objects.GetArray())
                {
                    const auto fileId = object.FindMember("FileId");
                    if (fileId == object.MemberEnd() || !fileId->value.IsInt64() || !objectIds.Add(fileId->value.GetInt64()))
                        return true;
                }
                for (const JsonAssetPreparedPartition& partition : payload->ScenePartitions)
                {
                    const rapidjson::SizeType previousCount = objects.Size();
                    JsonDocument chunk;
                    chunk.Parse(partition.SourceJson.Get(), partition.SourceJson.Length());
                    String error;
                    if (chunk.HasParseError() || ScenePartitionDocument::AppendRuntimeObjects(chunk, partition.RootFileId,
                        objects, runtime.GetAllocator(), error))
                        return true;
                    for (rapidjson::SizeType i = previousCount; i < objects.Size(); i++)
                    {
                        const auto fileId = objects[i].FindMember("FileId");
                        if (fileId == objects[i].MemberEnd() || !fileId->value.IsInt64() || !objectIds.Add(fileId->value.GetInt64()))
                            return true;
                    }
                }
                for (const JsonValue& object : objects.GetArray())
                {
                    const auto parent = object.FindMember("ParentFileId");
                    if (parent != object.MemberEnd() && (!parent->value.IsInt64() || !objectIds.Contains(parent->value.GetInt64())))
                        return true;
                }
            }
            objects.Accept(writer.GetWriter());
        }
        else
        {
            dataValue->Accept(writer.GetWriter());
        }
        writer.EndObject();
        output.Set(buffer.GetString(), (int32)buffer.GetSize());
        return false;
    }

    bool WriteRuntimeStorage(const StringView& path, const Guid& id, const StringView& typeName, const StringAnsiView& json)
    {
        FlaxChunk chunk;
        chunk.Flags = FlaxChunkFlags::CompressedLZ4;
        chunk.Data.Copy(reinterpret_cast<const byte*>(json.Get()), json.Length());
        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = typeName;
        data.SerializedVersion = JsonAssetProcessor::RuntimeFormatVersion;
        data.Header.Chunks[0] = &chunk;
        return FlaxStorage::Create(path, data);
    }
}

const String& JsonAssetProcessor::ProcessorID()
{
    static const String value(TEXT("Flax.JsonDocument"));
    return value;
}

AssetProcessorDescriptor JsonAssetProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.SourceExtensions.Add(TEXT(".scene"));
    descriptor.SourceExtensions.Add(TEXT(".prefab"));
    descriptor.SourceExtensions.Add(TEXT(".json"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.SceneAsset"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.Prefab"));
    descriptor.MainOutputType = TEXT("FlaxEngine.JsonAsset");
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "json-document";
    descriptor.MemoryEstimate = 64ull * 1024ull * 1024ull;
    descriptor.Prepare = &JsonAssetProcessor::Prepare;
    descriptor.Build = &JsonAssetProcessor::Build;
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::None;
    runtime.CompatibilityTag = "flax-json-source-v3";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool JsonAssetProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    const AssetRecord& record = context.GetRecord();
    if (record.ProcessorID != ProcessorID() || record.TypeName.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("JSON document metadata has an invalid importer or runtime type."));

    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    const JsonValue* data = json.HasParseError() ? nullptr : ValidateSourceDocument(json, record.TypeName);
    if (!data)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("JSON source document does not match the schema selected by its metadata type."));

    HashSet<AssetObjectId> references;
    if (CollectReferences(*data, record.SourceAssetID, references))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("JSON source contains a malformed structured {guid,fileId} reference."));
    for (const auto& entry : references)
    {
        const AssetObjectId& reference = entry.Item;
        if (context.DeclareRuntimeReference(TEXT("json-reference:") + reference.ToString(), reference, origin, diagnostic))
            return true;
    }

    auto payload = std::make_shared<JsonAssetPreparedPayload>();
    payload->SourceHash = sourceHash;
    uint64 partitionBytes = 0;
    if (record.TypeName == TEXT("FlaxEngine.SceneAsset"))
    {
        Array<ScenePartitionReference> partitions;
        String partitionError;
        if (ScenePartitionDocument::ReadSceneReferences(json, partitions, partitionError))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), partitionError);
        const auto external = json.FindMember("externalActors");
        payload->SceneUsesPartitions = external != json.MemberEnd() && external->value.GetBool();
        const String partitionRoot = String(record.SourcePath.Get()) + TEXT("-data");
        for (const ScenePartitionReference& partition : partitions)
        {
            AssetRecord partitionRecord;
            if (!AssetDatabase::Get().TryGetRecord(partition.Object.Asset.Value, partitionRecord) ||
                !partitionRecord.IsMainAsset() || partitionRecord.ProcessorID != TEXT("Flax.SceneChunk") ||
                partitionRecord.Status != AssetRecordStatus::Ready ||
                !AssetPathPolicy::IsSameOrChild(partitionRecord.SourcePath.Get(), partitionRoot))
            {
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
                    record.ID, record.SourcePath.Get(), TEXT("Scene partition reference does not resolve to a ready .scenechunk source inside the companion scene-data directory."));
            }
            Array<byte> partitionSource;
            ContentHash partitionHash;
            AssetDependencyOrigin partitionOrigin;
            partitionOrigin.Path = partitionRecord.SourcePath.Get();
            if (context.ReadSourceFile(partitionRecord.SourcePath.Get(), partitionSource, partitionHash, partitionOrigin, diagnostic))
                return true;
            JsonDocument chunk;
            chunk.Parse(reinterpret_cast<const char*>(partitionSource.Get()), partitionSource.Count());
            int64 chunkRoot;
            const JsonValue* chunkObjects;
            if (chunk.HasParseError() || ScenePartitionDocument::ReadChunk(chunk, chunkRoot, chunkObjects, partitionError) ||
                chunkRoot != partition.RootFileId)
            {
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                    record.ID, partitionRecord.SourcePath.Get(), TEXT("Scene partition source is malformed or its rootFileId does not match the owner scene."));
            }
            AssetSemanticInterface semantic;
            semantic.Version = 1;
            semantic.Hash = partitionHash;
            if (context.DeclareBuildInput(TEXT("scene-partition:") + partition.Object.ToString(), partition.Object,
                ArtifactKey(), semantic, partitionOrigin, diagnostic))
                return true;
            JsonAssetPreparedPartition preparedPartition;
            preparedPartition.Object = partition.Object;
            preparedPartition.RootFileId = partition.RootFileId;
            preparedPartition.SourceHash = partitionHash;
            preparedPartition.SourceJson.Set(reinterpret_cast<const char*>(partitionSource.Get()), partitionSource.Count());
            partitionBytes += partitionSource.Count();
            payload->ScenePartitions.Add(MoveTemp(preparedPartition));
        }
    }

    static const char CompilerIdentity[] = "flax-json-source-compiler-v3";
    if (context.DeclareToolchain(TEXT("json-document-compiler"), ContentHash::Compute(CompilerIdentity, ARRAY_COUNT(CompilerIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(JsonAssetPreparedPayload) + bytes.Count() + partitionBytes;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool JsonAssetProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const JsonAssetPreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("JSON document output key requires prepared runtime state."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-json-source-output-v3"));
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddString(StringAnsiView("output-type"), prepared.OutputType);
    builder.AddHash(StringAnsiView("source"), payload->SourceHash);
    for (int32 i = 0; i < prepared.Dependencies.Count(); i++)
    {
        if (prepared.Dependencies[i].AffectsBuildKey())
            prepared.Dependencies[i].AppendKeyComponents(builder, i);
    }
    builder.AddTarget(target, ArtifactTargetDimension::None);
    key = builder.Finalize();
    components = builder.GetComponents();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool JsonAssetProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceFile)
        {
            sourceDependency = &dependency;
            break;
        }
    }
    if (!sourceDependency)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, StringView::Empty, TEXT("JSON document build has no declared source."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(sourceBytes.Get()), sourceBytes.Count());
    StringAnsi runtimeJson;
    const auto* payload = static_cast<const JsonAssetPreparedPayload*>(prepared.Payload.get());
    if (json.HasParseError() || !ValidateSourceDocument(json, prepared.OutputType) ||
        BuildRuntimeEnvelope(json, prepared.AssetID, prepared.OutputType, payload, runtimeJson))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("JSON document changed after preparation."));

    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (WriteRuntimeStorage(scratchPath, prepared.AssetID, prepared.OutputType, runtimeJson))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("JSON runtime storage could not be created."));
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("JSON runtime storage is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("document.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
