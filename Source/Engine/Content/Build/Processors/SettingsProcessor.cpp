// Copyright (c) Wojciech Figat. All rights reserved.

#include "SettingsProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
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
        diagnostic.ProcessorId = SettingsProcessor::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    const JsonValue* ValidateSourceDocument(const JsonDocument& json, String& dataType)
    {
        if (!json.IsObject())
            return nullptr;
        const auto versionMember = json.FindMember("settingsVersion");
        const auto typeMember = json.FindMember("type");
        const auto dataMember = json.FindMember("data");
        if (versionMember == json.MemberEnd() || !versionMember->value.IsUint() || versionMember->value.GetUint() != 1 ||
            typeMember == json.MemberEnd() || !typeMember->value.IsString() || typeMember->value.GetStringLength() == 0 ||
            dataMember == json.MemberEnd() || !dataMember->value.IsObject())
            return nullptr;
        dataType = String(StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength()));
        return &dataMember->value;
    }

    bool CollectReferences(const JsonValue& value, HashSet<AssetObjectId>& references)
    {
        if (value.IsArray())
        {
            for (const JsonValue& item : value.GetArray())
            {
                if (CollectReferences(item, references))
                    return true;
            }
            return false;
        }
        if (!value.IsObject())
            return false;

        const auto guidMember = value.FindMember("guid");
        const auto fileIDMember = value.FindMember("fileId");
        if (guidMember != value.MemberEnd() || fileIDMember != value.MemberEnd())
        {
            Guid guid;
            int64 fileID;
            if (guidMember == value.MemberEnd() || fileIDMember == value.MemberEnd() || !guidMember->value.IsString() ||
                Guid::Parse(StringAnsiView(guidMember->value.GetString(), guidMember->value.GetStringLength()), guid) ||
                !fileIDMember->value.IsInt64())
                return true;
            fileID = fileIDMember->value.GetInt64();
            if (!guid.IsValid() && fileID == 0)
                return false;
            const AssetObjectId objectID(AssetGuid(guid), fileID);
            if (!objectID.IsValid())
                return true;
            references.Add(objectID);
        }
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
        {
            if (CollectReferences(member->value, references))
                return true;
        }
        return false;
    }

    bool BuildRuntimeEnvelope(const JsonDocument& source, const Guid& id, StringAnsi& output)
    {
        String dataType;
        const JsonValue* dataValue = ValidateSourceDocument(source, dataType);
        if (!dataValue)
            return true;
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("ID");
        writer.Guid(id);
        writer.JKEY("TypeName");
        const StringAnsi typeAnsi(dataType);
        writer.String(typeAnsi.Get(), typeAnsi.Length());
        writer.JKEY("EngineBuild");
        writer.Int(FLAXENGINE_VERSION_BUILD);
        writer.JKEY("Data");
        dataValue->Accept(writer.GetWriter());
        writer.EndObject();
        output.Set(buffer.GetString(), (int32)buffer.GetSize());
        return false;
    }

    bool WriteFlax(const StringView& path, const Guid& id, const StringAnsiView& json)
    {
        FlaxChunk chunk;
        chunk.Data.Copy(reinterpret_cast<const byte*>(json.Get()), json.Length());
        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = JsonAsset::TypeName;
        data.SerializedVersion = SettingsProcessor::RuntimeFormatVersion;
        data.Header.Chunks[0] = &chunk;
        return FlaxStorage::Create(path, data);
    }
}

const String& SettingsProcessor::ProcessorID()
{
    static const String value(TEXT("Flax.Settings"));
    return value;
}

AssetProcessorDescriptor SettingsProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.SourceExtensions.Add(TEXT(".settings"));
    descriptor.DocumentTypes.Add(JsonAsset::TypeName);
    descriptor.MainOutputType = JsonAsset::TypeName;
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "settings-document";
    descriptor.MemoryEstimate = 4ull * 1024ull * 1024ull;
    descriptor.Prepare = &SettingsProcessor::Prepare;
    descriptor.Build = &SettingsProcessor::Build;
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::None;
    runtime.CompatibilityTag = "flax-settings-source-v2";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool SettingsProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    const AssetRecord& record = context.GetRecord();
    if (record.ProcessorID != ProcessorID() || record.TypeName != JsonAsset::TypeName)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Settings metadata must use Flax.Settings and the JsonAsset runtime type."));

    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    String dataType;
    const JsonValue* data = json.HasParseError() ? nullptr : ValidateSourceDocument(json, dataType);
    if (!data)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Settings source must contain settingsVersion, type, and object data fields."));

    HashSet<AssetObjectId> references;
    if (CollectReferences(*data, references))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Settings source contains a malformed structured {guid,fileId} reference."));
    for (const auto& entry : references)
    {
        const AssetObjectId& reference = entry.Item;
        const String identity = TEXT("settings-reference:") + reference.ToString();
        if (context.DeclareRuntimeReference(identity, reference, origin, diagnostic))
            return true;
    }

    static const char CompilerIdentity[] = "flax-settings-source-compiler-v2";
    if (context.DeclareToolchain(TEXT("settings-compiler"), ContentHash::Compute(CompilerIdentity, ARRAY_COUNT(CompilerIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    auto payload = std::make_shared<SettingsPreparedPayload>();
    payload->SourceHash = sourceHash;
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(SettingsPreparedPayload) + bytes.Count();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SettingsProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget&, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const SettingsPreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("Settings output key requires prepared state and the runtime output."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-settings-source-output-v2"));
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddString(StringAnsiView("output-type"), prepared.OutputType);
    builder.AddHash(StringAnsiView("source"), payload->SourceHash);
    for (int32 i = 0; i < prepared.Dependencies.Count(); i++)
    {
        if (prepared.Dependencies[i].AffectsBuildKey())
            prepared.Dependencies[i].AppendKeyComponents(builder, i);
    }
    key = builder.Finalize();
    components = builder.GetComponents();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SettingsProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
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
            prepared.AssetID, StringView::Empty, TEXT("Settings build has no declared source."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    JsonDocument json;
    json.Parse(reinterpret_cast<const char*>(sourceBytes.Get()), sourceBytes.Count());
    String dataType;
    StringAnsi runtimeJson;
    if (json.HasParseError() || !ValidateSourceDocument(json, dataType) || BuildRuntimeEnvelope(json, prepared.AssetID, runtimeJson))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("Settings source changed after preparation."));

    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (WriteFlax(scratchPath, prepared.AssetID, runtimeJson))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Settings JsonAsset artifact could not be written."));
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Settings JsonAsset artifact is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("settings.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
