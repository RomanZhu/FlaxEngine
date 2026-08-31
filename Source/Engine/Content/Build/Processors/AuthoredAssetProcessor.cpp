// Copyright (c) Wojciech Figat. All rights reserved.

#include "AuthoredAssetProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Documents/MaterialInstanceDocument.h"
#include "Engine/Content/Documents/SceneAnimationDocument.h"
#include "Engine/Content/Documents/ParticleSystemDocument.h"
#include "Engine/Content/Documents/CollisionDataDocument.h"
#include "Engine/Content/Documents/AuthoredSourceDocument.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "FlaxEngine.Gen.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Content/Assets/SkeletonMask.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Engine/GameplayGlobals.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Physics/CollisionCooking.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/ModelBase.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Serialization/Json.h"

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
        diagnostic.ProcessorId = TEXT("Flax.AuthoredDocument");
        diagnostic.Message = message;
        return true;
    }

    bool ReadString(const JsonDocument& json, const char* key, StringAnsiView& value)
    {
        const auto member = json.FindMember(key);
        if (member == json.MemberEnd() || !member->value.IsString())
            return true;
        value = StringAnsiView(member->value.GetString(), member->value.GetStringLength());
        return false;
    }

    bool WriteFlax(const StringView& path, const Guid& id, const StringView& typeName, uint32 version, const Array<byte>& chunk0)
    {
        FlaxChunk chunk;
        if (chunk0.Count())
            chunk.Data.Copy(chunk0.Get(), chunk0.Count());
        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = typeName;
        data.SerializedVersion = version;
        data.Header.Chunks[0] = &chunk;
        return FlaxStorage::Create(path, data);
    }

    bool DecodeRuntimeChunk(const JsonDocument& json, Array<byte>& chunk, String& error)
    {
        StringAnsiView encoding;
        StringAnsiView text;
        if (ReadString(json, "payloadEncoding", encoding) || encoding != StringAnsiView("hex") ||
            ReadString(json, "runtimeChunk", text) || (text.Length() & 1) != 0)
        {
            error = TEXT("Authored runtime payload must contain an even-length hexadecimal runtimeChunk.");
            return true;
        }
        chunk.Resize(text.Length() / 2);
        for (int32 i = 0; i < chunk.Count(); i++)
        {
            const char high = text[i * 2];
            const char low = text[i * 2 + 1];
            const int32 highValue = high >= '0' && high <= '9' ? high - '0' : high >= 'a' && high <= 'f' ? high - 'a' + 10 : high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
            const int32 lowValue = low >= '0' && low <= '9' ? low - '0' : low >= 'a' && low <= 'f' ? low - 'a' + 10 : low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;
            if (highValue < 0 || lowValue < 0)
            {
                error = TEXT("Authored runtimeChunk contains a non-hexadecimal character.");
                return true;
            }
            chunk[i] = static_cast<byte>((highValue << 4) | lowValue);
        }
        if (chunk.IsEmpty())
        {
            error = TEXT("Authored runtimeChunk cannot be empty.");
            return true;
        }
        return false;
    }

    bool BuildRuntimePayload(const JsonDocument& json, const Guid& id, const StringView& typeName, uint32 version,
        const StringView& scratchPath, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> chunk;
        String error;
        if (DecodeRuntimeChunk(json, chunk, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, error);
        if (WriteFlax(scratchPath, id, typeName, version, chunk))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Authored runtime payload could not be written."));
        return false;
    }

    bool BuildMaterialInstance(const JsonDocument& json, const Guid& id, const StringView& scratchPath, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> chunk;
        String error;
        if (MaterialInstanceDocument::Compile(json, chunk, nullptr, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, error);
        if (WriteFlax(scratchPath, id, MaterialInstance::TypeName, 4, chunk))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Material instance flax could not be written."));
        return false;
    }

    bool BuildSkeletonMask(const JsonDocument& json, const Guid& id, const StringView& scratchPath, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsiView skeletonText;
        if (ReadString(json, "skeleton", skeletonText))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Skeleton mask document is missing skeleton."));
        Guid skeletonId;
        if (Guid::Parse(skeletonText, skeletonId))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Skeleton mask skeleton is not a GUID."));
        const auto nodes = json.FindMember("maskedNodes");
        if (nodes == json.MemberEnd() || !nodes->value.IsArray())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Skeleton mask document is missing maskedNodes."));
        MemoryWriteStream stream(256);
        stream.Write(skeletonId);
        stream.WriteInt32(static_cast<int32>(nodes->value.Size()));
        for (const auto& node : nodes->value.GetArray())
        {
            if (!node.IsString())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Skeleton mask node names must be strings."));
            stream.Write(String(StringAnsiView(node.GetString(), node.GetStringLength())), -13);
        }
        Array<byte> chunk;
        chunk.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
        if (WriteFlax(scratchPath, id, SkeletonMask::TypeName, 2, chunk))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Skeleton mask flax could not be written."));
        return false;
    }

    bool BuildSceneAnimation(const JsonDocument& json, const Guid& id, const StringView& scratchPath, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> chunk;
        String error;
        if (SceneAnimationDocument::Compile(json, chunk, nullptr, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, error);
        if (WriteFlax(scratchPath, id, SceneAnimation::TypeName, 1, chunk))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Scene animation flax could not be written."));
        return false;
    }

    bool BuildParticleSystem(const JsonDocument& json, const Guid& id, const StringView& scratchPath, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> chunk;
        String error;
        if (ParticleSystemDocument::Compile(json, chunk, nullptr, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, error);
        if (WriteFlax(scratchPath, id, ParticleSystem::TypeName, ParticleSystem::SerializedVersion, chunk))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Particle system flax could not be written."));
        return false;
    }

    bool BuildCollisionData(const JsonDocument& json, const Guid& id, const StringView& scratchPath, AssetPipelineDiagnostic& diagnostic)
    {
        CollisionData::SerializedOptions recipe;
        String error;
        if (CollisionDataDocument::Parse(json, recipe, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, scratchPath, error);
        CollisionData::SerializedOptions cooked = recipe;
        BytesContainer cookedBytes;
#if COMPILE_WITH_PHYSICS_COOKING
        if (recipe.Type != CollisionDataType::None)
        {
            auto model = Content::LoadAsync<ModelBase>(recipe.Model);
            if (!model || model->WaitForLoaded())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Collision source model artifact could not be loaded."));
            CollisionCooking::Argument argument;
            argument.Type = recipe.Type;
            argument.Model = model;
            argument.ModelLodIndex = recipe.ModelLodIndex;
            argument.MaterialSlotsMask = recipe.MaterialSlotsMask;
            argument.ConvexFlags = recipe.ConvexFlags;
            argument.ConvexVertexLimit = recipe.ConvexVertexLimit;
            if (CollisionCooking::CookCollision(argument, cooked, cookedBytes))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Collision source model could not be cooked."));
        }
#else
        if (recipe.Type != CollisionDataType::None)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Collision cooking is unavailable for this editor build."));
#endif
        Array<byte> chunk;
        chunk.Resize(sizeof(cooked) + cookedBytes.Length());
        Platform::MemoryCopy(chunk.Get(), &cooked, sizeof(cooked));
        if (cookedBytes.Length())
            Platform::MemoryCopy(chunk.Get() + sizeof(cooked), cookedBytes.Get(), cookedBytes.Length());
        if (WriteFlax(scratchPath, id, CollisionData::TypeName, CollisionData::SerializedVersion, chunk))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, id, scratchPath, TEXT("Collision data flax could not be written."));
        return false;
    }

    bool BuildGenericObject(const Array<byte>& sourceBytes, const Guid& id, int64 localId, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
    {
        AuthoredSourceDocument sourceDocument;
        String error;
        if (AuthoredSourceDocument::Parse(StringAnsiView((const char*)sourceBytes.Get(), sourceBytes.Count()), sourceDocument, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, StringView::Empty, error);
        const AuthoredSourceObject* object = sourceDocument.FindObject(localId);
        if (!object)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, StringView::Empty,
                TEXT("Authored object disappeared after preparation."));

        JsonDocument document;
        document.SetObject();
        auto& allocator = document.GetAllocator();
        const StringAnsi idText(id.ToString(Guid::FormatType::N).ToLower());
        const StringAnsi typeText(object->TypeName);
        document.AddMember("ID", JsonValue(idText.Get(), idText.Length(), allocator), allocator);
        document.AddMember("TypeName", JsonValue(typeText.Get(), typeText.Length(), allocator), allocator);
        document.AddMember("EngineBuild", FLAXENGINE_VERSION_BUILD, allocator);
        JsonDocument data;
        data.Parse(object->DataJson.Get(), object->DataJson.Length());
        if (data.HasParseError())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, StringView::Empty,
                TEXT("Authored object payload is malformed."));
        JsonValue dataValue;
        dataValue.CopyFrom(data, allocator);
        document.AddMember("Data", dataValue.Move(), allocator);
        Array<StringAnsi> order;
        order.Add("ID"); order.Add("TypeName"); order.Add("EngineBuild"); order.Add("Data");
        CanonicalJsonError canonicalError;
        if (CanonicalJsonWriter::Write(document, output, canonicalError, &order))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, id, StringView::Empty, canonicalError.Message);
        return false;
    }

    bool HashCollisionModelInputs(PrepareAssetContext& context, const Guid& modelID, const AssetDependencyOrigin& origin, AssetPipelineDiagnostic& diagnostic)
    {
        if (!modelID.IsValid())
            return false;
        AssetRecord modelRecord;
        String modelPath;
        const bool hasModelRecord = AssetDatabase::Get().TryGetRecord(modelID, modelRecord);
        if (hasModelRecord)
        {
            modelPath = modelRecord.SourcePath.Get();
        }
        else
        {
            AssetInfo modelInfo;
            if (!Content::GetAssetInfo(modelID, modelInfo) || modelInfo.Path.IsEmpty())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
                    context.GetRecord().ID, context.GetRecord().SourcePath.Get(), TEXT("Collision source model is unavailable."));
            modelPath = modelInfo.Path;
        }
        Array<byte> bytes;
        if (File::ReadAllBytes(modelPath, bytes))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
                context.GetRecord().ID, modelPath, TEXT("Collision source model file is missing."));
        ContentHasher hasher;
        hasher.Update(bytes.Get(), bytes.Count());
        bytes.Clear();
        if (hasModelRecord && FileSystem::FileExists(modelRecord.MetaPath.Get()))
        {
            if (File::ReadAllBytes(modelRecord.MetaPath.Get(), bytes))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
                    context.GetRecord().ID, modelRecord.MetaPath.Get(), TEXT("Collision source model metadata is unreadable."));
            hasher.Update(bytes.Get(), bytes.Count());
        }
        return context.DeclareToolchain(TEXT("collision-model-input"), hasher.Finalize(), origin, diagnostic);
    }
}

bool AuthoredAssetProcessor::Owns(const StringView& processorID)
{
    return processorID == MaterialInstanceID() || processorID == SkeletonMaskID() || processorID == SceneAnimationID() ||
        processorID == ParticleSystemID() || processorID == CollisionDataID() || processorID == AnimationID() ||
        processorID == GameplayGlobalsID() || processorID == GenericObjectID();
}

const String& AuthoredAssetProcessor::MaterialInstanceID()
{
    static const String value(TEXT("Flax.MaterialInstance"));
    return value;
}

const String& AuthoredAssetProcessor::SkeletonMaskID()
{
    static const String value(TEXT("Flax.SkeletonMask"));
    return value;
}

const String& AuthoredAssetProcessor::SceneAnimationID()
{
    static const String value(TEXT("Flax.SceneAnimation"));
    return value;
}

const String& AuthoredAssetProcessor::ParticleSystemID()
{
    static const String value(TEXT("Flax.ParticleSystem"));
    return value;
}

const String& AuthoredAssetProcessor::CollisionDataID()
{
    static const String value(TEXT("Flax.CollisionData"));
    return value;
}

const String& AuthoredAssetProcessor::AnimationID()
{
    static const String value(TEXT("Flax.Animation"));
    return value;
}

const String& AuthoredAssetProcessor::GameplayGlobalsID()
{
    static const String value(TEXT("Flax.GameplayGlobals"));
    return value;
}

const String& AuthoredAssetProcessor::GenericObjectID()
{
    static const String value(TEXT("Flax.AuthoredObject"));
    return value;
}

AssetProcessorDescriptor AuthoredAssetProcessor::CreateDescriptor(const StringView& processorID)
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = processorID;
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "authored-document";
    descriptor.MemoryEstimate = 32ull * 1024ull * 1024ull;
    descriptor.Prepare = &AuthoredAssetProcessor::Prepare;
    descriptor.Build = &AuthoredAssetProcessor::Build;
    if (processorID == GenericObjectID())
    {
        descriptor.SourceExtensions.Add(TEXT(".asset"));
        descriptor.DocumentTypes.Add(TEXT("Flax.AuthoredObject"));
        descriptor.MainOutputType = TEXT("FlaxEngine.JsonAsset");
    }
    else if (processorID == MaterialInstanceID())
    {
        descriptor.SourceExtensions.Add(TEXT(".materialinstance"));
        descriptor.DocumentTypes.Add(MaterialInstance::TypeName);
        descriptor.MainOutputType = MaterialInstance::TypeName;
    }
    else if (processorID == SkeletonMaskID())
    {
        descriptor.SourceExtensions.Add(TEXT(".skeletonmask"));
        descriptor.DocumentTypes.Add(SkeletonMask::TypeName);
        descriptor.MainOutputType = SkeletonMask::TypeName;
    }
    else if (processorID == SceneAnimationID())
    {
        descriptor.SourceExtensions.Add(TEXT(".sceneanimation"));
        descriptor.DocumentTypes.Add(SceneAnimation::TypeName);
        descriptor.MainOutputType = SceneAnimation::TypeName;
    }
    else if (processorID == ParticleSystemID())
    {
        descriptor.SourceExtensions.Add(TEXT(".particlesystem"));
        descriptor.DocumentTypes.Add(ParticleSystem::TypeName);
        descriptor.MainOutputType = ParticleSystem::TypeName;
    }
    else if (processorID == AnimationID())
    {
        descriptor.SourceExtensions.Add(TEXT(".animation"));
        descriptor.DocumentTypes.Add(Animation::TypeName);
        descriptor.MainOutputType = Animation::TypeName;
    }
    else if (processorID == GameplayGlobalsID())
    {
        descriptor.SourceExtensions.Add(TEXT(".gameplayglobals"));
        descriptor.DocumentTypes.Add(GameplayGlobals::TypeName);
        descriptor.MainOutputType = GameplayGlobals::TypeName;
    }
    else
    {
        descriptor.SourceExtensions.Add(TEXT(".collisiondata"));
        descriptor.DocumentTypes.Add(CollisionData::TypeName);
        descriptor.MainOutputType = CollisionData::TypeName;
    }
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = processorID == GenericObjectID() ? ".json" : ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
    runtime.CompatibilityTag = processorID == GenericObjectID() ? "flax-authored-object-v1" : "flax-authored-document-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool AuthoredAssetProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    context.SetSourceSerializerVersion(RuntimeFormatVersion);
    const AssetRecord& record = context.GetRecord();
    if (!Owns(record.ProcessorID))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Authored document processor does not own this record."));
    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    if (json.HasParseError() || !json.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Authored document JSON is malformed."));
    const bool genericObjectDocument = record.ProcessorID == GenericObjectID();
    int64 genericObjectLocalId = 0;
    if (genericObjectDocument)
    {
        AuthoredSourceDocument sourceDocument;
        String error;
        if (AuthoredSourceDocument::Parse(StringAnsiView((const char*)bytes.Get(), bytes.Count()), sourceDocument, error))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), error);
        genericObjectLocalId = record.LocalId;
        const AuthoredSourceObject* object = sourceDocument.FindObject(genericObjectLocalId);
        if (!object || object->TypeName != record.TypeName)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), TEXT("Authored object inventory does not match adjacent metadata."));
    }
    else
    {
        StringAnsiView type;
        if (ReadString(json, "type", type) || String(type) != record.TypeName)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), TEXT("Authored document type does not match its metadata sidecar."));
        if (record.ProcessorID == AnimationID() || record.ProcessorID == GameplayGlobalsID())
        {
            Array<byte> runtimeChunk;
            String error;
            if (DecodeRuntimeChunk(json, runtimeChunk, error))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                    record.ID, record.SourcePath.Get(), error);
        }
    }
    if (record.ProcessorID == MaterialInstanceID() || record.ProcessorID == SceneAnimationID() ||
        record.ProcessorID == ParticleSystemID() || record.ProcessorID == CollisionDataID() ||
        record.ProcessorID == AnimationID() || record.ProcessorID == GameplayGlobalsID())
    {
        Array<byte> runtimeChunk;
        Array<Guid> references;
        String error;
        bool invalid = false;
        if (record.ProcessorID == MaterialInstanceID())
            invalid = MaterialInstanceDocument::Compile(json, runtimeChunk, &references, error);
        else if (record.ProcessorID == SceneAnimationID())
            invalid = SceneAnimationDocument::Compile(json, runtimeChunk, &references, error);
        else if (record.ProcessorID == ParticleSystemID())
            invalid = ParticleSystemDocument::Compile(json, runtimeChunk, &references, error);
        else if (record.ProcessorID == CollisionDataID())
        {
            CollisionData::SerializedOptions options;
            invalid = CollisionDataDocument::Parse(json, options, error);
            if (!invalid && options.Model.IsValid())
                references.Add(options.Model);
        }
        if (invalid)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), error);
        if (record.RuntimeObjectReferences.HasItems())
        {
            for (const AssetObjectId& reference : record.RuntimeObjectReferences)
            {
                const String identity = String::Format(TEXT("authored-reference:{0}:{1}"), reference.Guid, reference.LocalId);
                if (context.DeclareRuntimeReference(identity, reference, origin, diagnostic))
                    return true;
            }
        }
        else
        {
            for (int32 i = 0; i < references.Count(); i++)
            {
                const String identity = String::Format(TEXT("authored-reference:{0}"), references[i]);
                if (context.DeclareRuntimeReference(identity, references[i], origin, diagnostic))
                    return true;
            }
        }
        if (record.ProcessorID == CollisionDataID() && references.HasItems() && HashCollisionModelInputs(context, references[0], origin, diagnostic))
            return true;
    }
    static const char CompilerIdentity[] = "flax-authored-document-compiler-v2";
    if (context.DeclareToolchain(TEXT("authored-compiler"), ContentHash::Compute(CompilerIdentity, ARRAY_COUNT(CompilerIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    auto payload = std::make_shared<AuthoredAssetPreparedPayload>();
    payload->SourceHash = sourceHash;
    payload->GenericObjectDocument = genericObjectDocument;
    payload->ObjectLocalId = genericObjectLocalId;
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(AuthoredAssetPreparedPayload) + bytes.Count();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AuthoredAssetProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const AuthoredAssetPreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("Authored output key requires prepared state and the runtime output."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-authored-document-output-v2"));
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

bool AuthoredAssetProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
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
            prepared.AssetID, StringView::Empty, TEXT("Authored build has no declared source."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    const auto* payload = static_cast<const AuthoredAssetPreparedPayload*>(prepared.Payload.get());
    if (!payload)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("Authored build payload is missing."));
    if (payload->GenericObjectDocument)
    {
        StringAnsi artifact;
        if (BuildGenericObject(sourceBytes, prepared.AssetID, payload->ObjectLocalId, artifact, diagnostic))
            return true;
        ArtifactWriter writer;
        if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
            writer.WriteFile(TEXT("authored.json"), artifact.Get(), artifact.Length(), diagnostic))
            return true;
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(sourceBytes.Get()), sourceBytes.Count());
    if (json.HasParseError() || !json.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("Authored document JSON changed after preparation."));
    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (prepared.OutputType == MaterialInstance::TypeName)
    {
        if (BuildMaterialInstance(json, prepared.AssetID, scratchPath, diagnostic))
            return true;
    }
    else if (prepared.OutputType == SkeletonMask::TypeName)
    {
        if (BuildSkeletonMask(json, prepared.AssetID, scratchPath, diagnostic))
            return true;
    }
    else if (prepared.OutputType == SceneAnimation::TypeName)
    {
        if (BuildSceneAnimation(json, prepared.AssetID, scratchPath, diagnostic))
            return true;
    }
    else if (prepared.OutputType == ParticleSystem::TypeName)
    {
        if (BuildParticleSystem(json, prepared.AssetID, scratchPath, diagnostic))
            return true;
    }
    else if (prepared.OutputType == CollisionData::TypeName)
    {
        if (BuildCollisionData(json, prepared.AssetID, scratchPath, diagnostic))
            return true;
    }
    else if (prepared.OutputType == Animation::TypeName)
    {
        if (BuildRuntimePayload(json, prepared.AssetID, Animation::TypeName, Animation::SerializedVersion, scratchPath, diagnostic))
            return true;
    }
    else if (prepared.OutputType == GameplayGlobals::TypeName)
    {
        if (BuildRuntimePayload(json, prepared.AssetID, GameplayGlobals::TypeName, GameplayGlobals::SerializedVersion, scratchPath, diagnostic))
            return true;
    }
    else
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("Authored document type is not rebuildable."));
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Authored compatibility artifact is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("authored.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
