// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneChunkProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Level/ScenePartitionDocument.h"
#include "Engine/Level/ScenePrefabDocument.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include "FlaxEngine.Gen.h"
#include <memory>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetId, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetId;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = SceneChunkProcessor::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    bool ParseChunk(const Array<byte>& bytes, int64& rootFileId, const JsonValue*& objects,
        JsonDocument& document, String& error)
    {
        document.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
        if (document.HasParseError())
        {
            error = TEXT("Scene chunk source contains malformed JSON.");
            return true;
        }
        return ScenePartitionDocument::ReadChunk(document, rootFileId, objects, error);
    }

    bool WriteRuntimeStorage(const StringView& path, const Guid& id, const JsonDocument& source)
    {
        int64 rootFileId;
        const JsonValue* sourceObjects;
        String error;
        if (ScenePartitionDocument::ReadChunk(source, rootFileId, sourceObjects, error))
            return true;
        JsonDocument converted;
        converted.SetArray();
        JsonValue runtimeObjects;
        if (ScenePrefabDocument::ToRuntimeObjects(*sourceObjects, runtimeObjects, converted.GetAllocator(), false, error))
            return true;

        rapidjson_flax::StringBuffer buffer;
        CompactJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(id);
        writer.JKEY("TypeName");
        writer.String("FlaxEngine.JsonAsset", ARRAY_COUNT("FlaxEngine.JsonAsset") - 1);
        writer.JKEY("EngineBuild");
        writer.Int(FLAXENGINE_VERSION_BUILD);
        writer.JKEY("Data");
        writer.StartObject();
        writer.JKEY("RootFileId");
        writer.Int64(rootFileId);
        writer.JKEY("Objects");
        runtimeObjects.Accept(writer.GetWriter());
        writer.EndObject();
        writer.EndObject();

        FlaxChunk chunk;
        chunk.Flags = FlaxChunkFlags::CompressedLZ4;
        chunk.Data.Copy(reinterpret_cast<const byte*>(buffer.GetString()), static_cast<int32>(buffer.GetSize()));
        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = JsonAsset::TypeName;
        data.SerializedVersion = SceneChunkProcessor::RuntimeFormatVersion;
        data.Header.Chunks[0] = &chunk;
        return FlaxStorage::Create(path, data);
    }
}

const String& SceneChunkProcessor::ProcessorID()
{
    static const String value(TEXT("Flax.SceneChunk"));
    return value;
}

AssetProcessorDescriptor SceneChunkProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.SourceExtensions.Add(TEXT(".scenechunk"));
    descriptor.DocumentTypes.Add(JsonAsset::TypeName);
    descriptor.MainOutputType = JsonAsset::TypeName;
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "scene-chunk";
    descriptor.MemoryEstimate = 64ull * 1024ull * 1024ull;
    descriptor.Prepare = &SceneChunkProcessor::Prepare;
    descriptor.Build = &SceneChunkProcessor::Build;
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::None;
    runtime.CompatibilityTag = "flax-scene-chunk-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool SceneChunkProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    const AssetRecord& record = context.GetRecord();
    if (record.ProcessorID != ProcessorID() || record.TypeName != JsonAsset::TypeName)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Scene chunk metadata has an invalid importer or runtime type."));
    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    JsonDocument source;
    const JsonValue* objects;
    int64 rootFileId;
    String error;
    if (ParseChunk(bytes, rootFileId, objects, source, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), error);
    static const char compilerIdentity[] = "flax-scene-chunk-compiler-v1";
    if (context.DeclareToolchain(TEXT("scene-chunk-compiler"), ContentHash::Compute(compilerIdentity, ARRAY_COUNT(compilerIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    auto payload = std::make_shared<SceneChunkPreparedPayload>();
    payload->SourceHash = sourceHash;
    payload->RootFileId = rootFileId;
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(SceneChunkPreparedPayload) + bytes.Count();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SceneChunkProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target,
    const StringAnsiView& outputKind, ArtifactKey& key, Array<ArtifactKeyComponent>& components,
    AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const SceneChunkPreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("Scene chunk output key requires prepared runtime state."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-scene-chunk-output-v1"));
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddHash(StringAnsiView("source"), payload->SourceHash);
    builder.AddUInt64(StringAnsiView("root-file-id"), static_cast<uint64>(payload->RootFileId));
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

bool SceneChunkProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
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
            prepared.AssetID, StringView::Empty, TEXT("Scene chunk build has no declared source."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    JsonDocument source;
    source.Parse(reinterpret_cast<const char*>(sourceBytes.Get()), sourceBytes.Count());
    int64 rootFileId;
    const JsonValue* objects;
    String error;
    if (source.HasParseError() || ScenePartitionDocument::ReadChunk(source, rootFileId, objects, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("Scene chunk changed after preparation."));

    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (WriteRuntimeStorage(scratchPath, prepared.AssetID, source))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Scene chunk runtime storage could not be created."));
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Scene chunk runtime storage is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("scenechunk.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
