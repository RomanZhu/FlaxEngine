// Copyright (c) Wojciech Figat. All rights reserved.

#include "ExistingJsonProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"
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
        diagnostic.ProcessorId = ExistingJsonProcessor::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    bool ValidateSource(const JsonDocument& json, const AssetRecord& record, AssetPipelineDiagnostic& diagnostic)
    {
        if (!json.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), TEXT("Existing JSON source root must be an object."));
        const auto id = json.FindMember("ID");
        const auto type = json.FindMember("TypeName");
        const auto data = json.FindMember("Data");
        Guid sourceID;
        if (id == json.MemberEnd() || !id->value.IsString() || Guid::Parse(id->value.GetStringAnsiView(), sourceID) || !sourceID.IsValid() ||
            type == json.MemberEnd() || !type->value.IsString() || String(type->value.GetStringAnsiView()) != record.TypeName || data == json.MemberEnd())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), TEXT("Existing JSON ID, type, or Data payload does not match its canonical record."));
        return false;
    }

    void RemapSelfReferences(JsonValue& value, const Guid& sourceID, const StringAnsiView& runtimeID, JsonDocument::AllocatorType& allocator)
    {
        if (value.IsObject())
        {
            for (auto i = value.MemberBegin(); i != value.MemberEnd(); ++i)
                RemapSelfReferences(i->value, sourceID, runtimeID, allocator);
        }
        else if (value.IsArray())
        {
            for (JsonValue& item : value.GetArray())
                RemapSelfReferences(item, sourceID, runtimeID, allocator);
        }
        else if (value.IsString())
        {
            Guid referenced;
            if (!Guid::Parse(value.GetStringAnsiView(), referenced) && referenced == sourceID)
                value.SetString(runtimeID.Get(), runtimeID.Length(), allocator);
        }
    }

    void FindRuntimeReferences(const JsonValue& value, const Guid& self, HashSet<Guid>& references)
    {
        if (value.IsObject())
        {
            for (auto i = value.MemberBegin(); i != value.MemberEnd(); ++i)
                FindRuntimeReferences(i->value, self, references);
        }
        else if (value.IsArray())
        {
            for (const JsonValue& item : value.GetArray())
                FindRuntimeReferences(item, self, references);
        }
        else if (value.IsString() && value.GetStringLength() == 32)
        {
            Guid id;
            AssetRecord referenced;
            if (!Guid::Parse(value.GetStringAnsiView(), id) && id != self && AssetDatabase::Get().TryGetRecord(id, referenced))
                references.Add(id);
        }
    }

    bool MakeRuntimeJson(const Array<byte>& sourceBytes, const PreparedAsset& prepared, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
    {
        JsonDocument source;
        source.Parse(reinterpret_cast<const char*>(sourceBytes.Get()), sourceBytes.Count());
        if (source.HasParseError() || !source.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, prepared.SourcePath, TEXT("Snapshotted Existing JSON source is malformed."));
        const auto data = source.FindMember("Data");
        const auto sourceIdMember = source.FindMember("ID");
        if (data == source.MemberEnd())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, prepared.SourcePath, TEXT("Snapshotted Existing JSON source has no Data payload."));

        JsonDocument runtime;
        runtime.SetObject();
        auto& allocator = runtime.GetAllocator();
        const StringAnsi id(prepared.AssetID.ToString(Guid::FormatType::N));
        const StringAnsi type(prepared.OutputType);
        runtime.AddMember("ID", JsonValue(id.Get(), id.Length(), allocator), allocator);
        runtime.AddMember("TypeName", JsonValue(type.Get(), type.Length(), allocator), allocator);
        runtime.AddMember("EngineBuild", FLAXENGINE_VERSION_BUILD, allocator);
        JsonValue runtimeData;
        runtimeData.CopyFrom(data->value, allocator);
        Guid sourceID;
        if (sourceIdMember != source.MemberEnd() && sourceIdMember->value.IsString() &&
            !Guid::Parse(sourceIdMember->value.GetStringAnsiView(), sourceID) && sourceID != prepared.AssetID)
            RemapSelfReferences(runtimeData, sourceID, id, allocator);
        runtime.AddMember("Data", runtimeData.Move(), allocator);
        Array<StringAnsi> rootOrder;
        rootOrder.Add("ID");
        rootOrder.Add("TypeName");
        rootOrder.Add("EngineBuild");
        rootOrder.Add("Data");
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(runtime, output, error, &rootOrder))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, prepared.SourcePath, error.Message);
        return false;
    }
}

const String& ExistingJsonProcessor::ProcessorID()
{
    static const String value(TEXT("Flax.ExistingJson"));
    return value;
}

AssetProcessorDescriptor ExistingJsonProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceKinds.Add(AssetSourceKind::ExistingJson);
    descriptor.SourceExtensions.Add(TEXT(".json"));
    descriptor.SourceExtensions.Add(TEXT(".scene"));
    descriptor.SourceExtensions.Add(TEXT(".prefab"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.JsonAsset"));
    descriptor.MainOutputType = TEXT("FlaxEngine.JsonAsset");
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "existing-json";
    descriptor.MemoryEstimate = 128ull * 1024ull * 1024ull;
    descriptor.Prepare = &ExistingJsonProcessor::Prepare;
    descriptor.Build = &ExistingJsonProcessor::Build;
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
    runtime.CompatibilityTag = "flax-existing-json-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool ExistingJsonProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    context.SetSourceSerializerVersion(RuntimeFormatVersion);
    const AssetRecord& record = context.GetRecord();
    if (record.ProcessorID != ProcessorID() || record.SourceKind != AssetSourceKind::ExistingJson)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Existing JSON processor does not own this canonical record."));
    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    if (json.HasParseError() || ValidateSource(json, record, diagnostic))
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), TEXT("Existing JSON source is malformed."));
        return true;
    }
    if (record.RuntimeObjectReferences.HasItems())
    {
        for (const AssetObjectId& reference : record.RuntimeObjectReferences)
        {
            const String identity = String::Format(TEXT("existing-json-reference:{0}:{1}"), reference.Guid, reference.LocalId);
            if (context.DeclareRuntimeReference(identity, reference, origin, diagnostic))
                return true;
        }
    }
    else
    {
        HashSet<Guid> runtimeReferences;
        FindRuntimeReferences(json, record.ID, runtimeReferences);
        for (const Guid& reference : record.RuntimeReferences)
            runtimeReferences.Add(reference);
        for (const auto& referenceEntry : runtimeReferences)
        {
            const Guid& reference = referenceEntry.Item;
            const String identity = String::Format(TEXT("existing-json-reference:{0}"), reference);
            if (context.DeclareRuntimeReference(identity, reference, origin, diagnostic))
                return true;
        }
    }
    static const char CompilerIdentity[] = "flax-existing-json-compiler-v2";
    if (context.DeclareToolchain(TEXT("existing-json-compiler"), ContentHash::Compute(CompilerIdentity, ARRAY_COUNT(CompilerIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    auto payload = std::make_shared<ExistingJsonPreparedPayload>();
    payload->SourceHash = sourceHash;
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(ExistingJsonPreparedPayload) + bytes.Count() * 2ull;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ExistingJsonProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const ExistingJsonPreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, prepared.SourcePath, TEXT("Existing JSON output key requires prepared state and the runtime output."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-existing-json-output-v1"));
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddString(StringAnsiView("output-type"), prepared.OutputType);
    builder.AddHash(StringAnsiView("source"), payload->SourceHash);
    for (int32 i = 0; i < prepared.Dependencies.Count(); i++)
    {
        if (prepared.Dependencies[i].AffectsBuildKey())
            prepared.Dependencies[i].AppendKeyComponents(builder, i);
    }
    builder.AddTarget(target, ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture);
    key = builder.Finalize();
    components = builder.GetComponents();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ExistingJsonProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceAsset)
        {
            sourceDependency = &dependency;
            break;
        }
    }
    if (!sourceDependency)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, prepared.SourcePath, TEXT("Existing JSON build has no declared source snapshot."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    StringAnsi runtimeJson;
    if (MakeRuntimeJson(sourceBytes, prepared, runtimeJson, diagnostic))
        return true;

    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    FlaxChunk chunk;
    chunk.Flags = FlaxChunkFlags::CompressedLZ4;
    chunk.Data.Copy(reinterpret_cast<const byte*>(runtimeJson.Get()), runtimeJson.Length());
    AssetInitData data;
    data.Header.ID = prepared.AssetID;
    data.Header.TypeName = prepared.OutputType;
    data.Header.Chunks[0] = &chunk;
    data.SerializedVersion = RuntimeFormatVersion;
    if (FlaxStorage::Create(scratchPath, data))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Existing JSON runtime Flax artifact could not be written."));
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Existing JSON runtime artifact is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("existing-json.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
