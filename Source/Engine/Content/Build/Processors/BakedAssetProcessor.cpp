// Copyright (c) Wojciech Figat. All rights reserved.

#include "BakedAssetProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Utilities/Encryption.h"

namespace
{
    constexpr int32 MaxEncodedPartBytes = 1024 * 1024 * 1024;

    struct BakedChunk
    {
        int32 Index = -1;
        FlaxChunkFlags Flags = FlaxChunkFlags::None;
        Array<byte> Data;
    };

    struct BakedDocument
    {
        String TypeName;
        uint32 SerializedVersion = 0;
        Array<byte> CustomData;
        Array<BakedChunk> Chunks;
        Array<AssetObjectId> RuntimeReferences;
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = TEXT("Flax.BakedAsset");
        diagnostic.Message = message;
        return true;
    }

    bool Decode(const rapidjson_flax::Value& value, Array<byte>& output)
    {
        if (!value.IsString())
            return true;
        const int32 encodedLength = static_cast<int32>(value.GetStringLength());
        const int32 decodedLength = Encryption::Base64DecodeLength(value.GetString(), encodedLength);
        if (decodedLength < 0 || decodedLength > MaxEncodedPartBytes)
            return true;
        Encryption::Base64Decode(value.GetString(), encodedLength, output);
        return output.Count() != decodedLength;
    }

    bool ParseDocument(const Array<byte>& source, const StringView& expectedType, BakedDocument& output, String& error)
    {
        rapidjson_flax::Document json;
        json.Parse(reinterpret_cast<const char*>(source.Get()), source.Count());
        if (json.HasParseError() || !json.IsObject())
        {
            error = TEXT("Baked asset source JSON is malformed.");
            return true;
        }
        const auto sourceVersion = json.FindMember("flaxSourceVersion");
        const auto documentType = json.FindMember("documentType");
        const auto type = json.FindMember("type");
        const auto serializedVersion = json.FindMember("serializedVersion");
        const auto customData = json.FindMember("customData");
        const auto chunks = json.FindMember("chunks");
        if (sourceVersion == json.MemberEnd() || !sourceVersion->value.IsUint() || sourceVersion->value.GetUint() != 1 ||
            documentType == json.MemberEnd() || !documentType->value.IsString() || documentType->value.GetStringAnsiView() != StringAnsiView("Flax.BakedAsset") ||
            type == json.MemberEnd() || !type->value.IsString() || serializedVersion == json.MemberEnd() || !serializedVersion->value.IsUint() ||
            customData == json.MemberEnd() || chunks == json.MemberEnd() || !chunks->value.IsArray())
        {
            error = TEXT("Baked asset source envelope is invalid or unsupported.");
            return true;
        }
        output.TypeName = String(type->value.GetStringAnsiView());
        output.SerializedVersion = serializedVersion->value.GetUint();
        if (output.TypeName.IsEmpty() || (expectedType.HasChars() && output.TypeName != expectedType) || Decode(customData->value, output.CustomData))
        {
            error = TEXT("Baked asset type or custom data does not match its metadata.");
            return true;
        }
        if (chunks->value.Size() > ASSET_FILE_DATA_CHUNKS)
        {
            error = TEXT("Baked asset source declares too many chunks.");
            return true;
        }
        bool used[ASSET_FILE_DATA_CHUNKS] = {};
        for (const auto& item : chunks->value.GetArray())
        {
            if (!item.IsObject())
            {
                error = TEXT("Baked asset chunk entry is not an object.");
                return true;
            }
            const auto index = item.FindMember("index");
            const auto flags = item.FindMember("flags");
            const auto data = item.FindMember("data");
            if (index == item.MemberEnd() || !index->value.IsInt() || index->value.GetInt() < 0 || index->value.GetInt() >= ASSET_FILE_DATA_CHUNKS ||
                flags == item.MemberEnd() || !flags->value.IsUint() || (flags->value.GetUint() & ~static_cast<uint32>(FlaxChunkFlags::CompressedLZ4)) != 0 ||
                data == item.MemberEnd() || used[index->value.GetInt()])
            {
                error = TEXT("Baked asset chunk index or flags are invalid.");
                return true;
            }
            BakedChunk chunk;
            chunk.Index = index->value.GetInt();
            chunk.Flags = static_cast<FlaxChunkFlags>(flags->value.GetUint());
            if (Decode(data->value, chunk.Data) || chunk.Data.IsEmpty())
            {
                error = TEXT("Baked asset chunk payload is invalid.");
                return true;
            }
            used[chunk.Index] = true;
            output.Chunks.Add(MoveTemp(chunk));
        }
        if (output.Chunks.IsEmpty())
        {
            error = TEXT("Baked asset source contains no runtime payload.");
            return true;
        }
        const auto references = json.FindMember("runtimeReferences");
        if (references != json.MemberEnd())
        {
            if (!references->value.IsArray())
            {
                error = TEXT("Baked asset runtime references are malformed.");
                return true;
            }
            for (const auto& item : references->value.GetArray())
            {
                if (!item.IsObject())
                {
                    error = TEXT("Baked asset runtime reference is malformed.");
                    return true;
                }
                const auto guid = item.FindMember("guid");
                const auto localId = item.FindMember("localId");
                Guid parsedGuid;
                if (guid == item.MemberEnd() || !guid->value.IsString() || Guid::Parse(guid->value.GetStringAnsiView(), parsedGuid) || !parsedGuid.IsValid() ||
                    localId == item.MemberEnd() || !localId->value.IsInt64() || localId->value.GetInt64() == 0)
                {
                    error = TEXT("Baked asset runtime reference identity is invalid.");
                    return true;
                }
                output.RuntimeReferences.Add(AssetObjectId(parsedGuid, localId->value.GetInt64()));
            }
        }
        return false;
    }
}

const String& BakedAssetProcessor::ProcessorID()
{
    static const String value(TEXT("Flax.BakedAsset"));
    return value;
}

AssetProcessorDescriptor BakedAssetProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceExtensions.Add(TEXT(".bakedasset"));
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.DocumentTypes.Add(TEXT("Flax.BakedAsset"));
    descriptor.MainOutputType = TEXT("FlaxEngine.Asset");
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "authored-bake";
    descriptor.MemoryEstimate = 64ull * 1024ull * 1024ull;
    descriptor.Prepare = &BakedAssetProcessor::Prepare;
    descriptor.Build = &BakedAssetProcessor::Build;
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
    runtime.CompatibilityTag = "flax-baked-asset-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool BakedAssetProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    context.SetSourceSerializerVersion(RuntimeFormatVersion);
    const AssetRecord& record = context.GetRecord();
    if (record.ProcessorID != ProcessorID())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Baked asset processor does not own this source."));
    Array<byte> source;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), source, sourceHash, origin, diagnostic))
        return true;
    BakedDocument document;
    String error;
    if (ParseDocument(source, record.TypeName, document, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), error);
    for (const AssetObjectId& reference : document.RuntimeReferences)
    {
        const String identity = String::Format(TEXT("baked-reference:{0}:{1}"), reference.Guid, reference.LocalId);
        if (context.DeclareRuntimeReference(identity, reference, origin, diagnostic))
            return true;
    }
    static const char CompilerIdentity[] = "flax-baked-asset-compiler-v1";
    if (context.DeclareToolchain(TEXT("baked-asset-compiler"), ContentHash::Compute(CompilerIdentity, ARRAY_COUNT(CompilerIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    auto payload = std::make_shared<BakedAssetPreparedPayload>();
    payload->SourceHash = sourceHash;
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(BakedAssetPreparedPayload) + source.Count();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool BakedAssetProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const BakedAssetPreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("Baked asset output key requires prepared runtime state."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-baked-asset-output-v1"));
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

bool BakedAssetProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
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
            prepared.AssetID, StringView::Empty, TEXT("Baked asset build has no declared source."));
    Array<byte> source;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, source, sourceHash, diagnostic))
        return true;
    BakedDocument document;
    String error;
    if (ParseDocument(source, prepared.OutputType, document, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, error);
    FlaxChunk chunks[ASSET_FILE_DATA_CHUNKS];
    AssetInitData data;
    data.Header.ID = prepared.AssetID;
    data.Header.TypeName = document.TypeName;
    data.SerializedVersion = document.SerializedVersion;
    if (document.CustomData.HasItems())
        data.CustomData.Copy(document.CustomData.Get(), document.CustomData.Count());
    for (const BakedChunk& sourceChunk : document.Chunks)
    {
        FlaxChunk& chunk = chunks[sourceChunk.Index];
        chunk.Flags = sourceChunk.Flags;
        chunk.Data.Copy(sourceChunk.Data.Get(), sourceChunk.Data.Count());
        data.Header.Chunks[sourceChunk.Index] = &chunk;
    }
    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (FlaxStorage::Create(scratchPath, data))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Baked asset runtime storage could not be written."));
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Baked asset runtime artifact is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("baked.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
