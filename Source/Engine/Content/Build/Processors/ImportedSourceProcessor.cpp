// Copyright (c) Wojciech Figat. All rights reserved.

#include "ImportedSourceProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Render2D/FontAsset.h"
#include "Engine/Serialization/Json.h"
#if COMPILE_WITH_AUDIO_TOOL
#include "Engine/Audio/AudioClip.h"
#include "Engine/ContentImporters/ImportAudio.h"
#endif
#include "Engine/ContentImporters/ImportFont.h"
#include "Engine/ContentImporters/ImportShader.h"
#include "Engine/ContentImporters/Types.h"

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = TEXT("Flax.ImportedSource");
        diagnostic.Message = message;
        return true;
    }
}

bool ImportedSourceProcessor::Owns(const StringView& processorID)
{
    return
#if COMPILE_WITH_AUDIO_TOOL
        processorID == AudioID() ||
#endif
        processorID == FontID() || processorID == ShaderID() || processorID == VideoID();
}

const String& ImportedSourceProcessor::AudioID()
{
    static const String value(TEXT("Flax.Audio"));
    return value;
}

const String& ImportedSourceProcessor::FontID()
{
    static const String value(TEXT("Flax.Font"));
    return value;
}

const String& ImportedSourceProcessor::ShaderID()
{
    static const String value(TEXT("Flax.ShaderSource"));
    return value;
}

const String& ImportedSourceProcessor::VideoID()
{
    static const String value(TEXT("Flax.Video"));
    return value;
}

AssetProcessorDescriptor ImportedSourceProcessor::CreateDescriptor(const StringView& processorID)
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = processorID;
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceKinds.Add(AssetSourceKind::ImportedSource);
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "imported-source";
    descriptor.MemoryEstimate = 256ull * 1024ull * 1024ull;
    descriptor.Prepare = &ImportedSourceProcessor::Prepare;
    descriptor.Build = &ImportedSourceProcessor::Build;
    if (processorID == AudioID())
    {
#if COMPILE_WITH_AUDIO_TOOL
        descriptor.SourceExtensions.Add(TEXT(".wav"));
        descriptor.SourceExtensions.Add(TEXT(".mp3"));
        descriptor.SourceExtensions.Add(TEXT(".ogg"));
        descriptor.DocumentTypes.Add(AudioClip::TypeName);
        descriptor.MainOutputType = AudioClip::TypeName;
#endif
    }
    else if (processorID == FontID())
    {
        descriptor.SourceExtensions.Add(TEXT(".ttf"));
        descriptor.SourceExtensions.Add(TEXT(".otf"));
        descriptor.DocumentTypes.Add(FontAsset::TypeName);
        descriptor.MainOutputType = FontAsset::TypeName;
    }
    else if (processorID == ShaderID())
    {
        descriptor.SourceExtensions.Add(TEXT(".shader"));
        descriptor.DocumentTypes.Add(Shader::TypeName);
        descriptor.MainOutputType = Shader::TypeName;
    }
    else
    {
        descriptor.SourceExtensions.Add(TEXT(".mp4"));
        descriptor.SourceExtensions.Add(TEXT(".webm"));
        descriptor.SourceExtensions.Add(TEXT(".mov"));
        descriptor.SourceExtensions.Add(TEXT(".mkv"));
        descriptor.DocumentTypes.Add(TEXT("FlaxEngine.Video"));
        descriptor.MainOutputType = TEXT("FlaxEngine.Video");
    }
    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = processorID == VideoID() ? ".bin" : ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
    runtime.CompatibilityTag = "flax-imported-source-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool ImportedSourceProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    const AssetRecord& record = context.GetRecord();
    if (!Owns(record.ProcessorID))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Imported source processor does not own this record."));
    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    if (bytes.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Imported source file is empty."));
    if (record.ProcessorID == ShaderID() && bytes.Count() < 10)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Shader source is too short."));
    static const char ToolIdentity[] = "flax-imported-source-tool-v1";
    if (context.DeclareToolchain(TEXT("imported-source"), ContentHash::Compute(ToolIdentity, ARRAY_COUNT(ToolIdentity) - 1), origin, diagnostic) ||
        context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    auto payload = std::make_shared<ImportedSourcePreparedPayload>();
    payload->SourceHash = sourceHash;
    payload->SettingsHash = ContentHash::Compute(context.GetSettings().Get(), context.GetSettings().Length());
    payload->SourceExtension = TEXT(".") + FileSystem::GetExtension(record.SourcePath.Get()).ToLower();
    if (record.ProcessorID == AudioID())
    {
#if COMPILE_WITH_AUDIO_TOOL
        rapidjson_flax::Document settings;
        settings.Parse(context.GetSettings().Get(), context.GetSettings().Length());
        if (settings.HasParseError() || !settings.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.SourcePath.Get(), TEXT("Audio processor settings are malformed."));
        payload->AudioOptions.Deserialize(settings, nullptr);
        payload->HasAudioOptions = true;
#else
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Audio importing is not compiled for this target."));
#endif
    }
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(ImportedSourcePreparedPayload) + bytes.Count();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ImportedSourceProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const ImportedSourcePreparedPayload*>(prepared.Payload.get());
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("Imported source output key requires prepared state and the runtime output."));
    ArtifactKeyBuilder builder(StringAnsiView("flax-imported-source-output-v2"));
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddString(StringAnsiView("output-type"), prepared.OutputType);
    builder.AddHash(StringAnsiView("source"), payload->SourceHash);
    builder.AddHash(StringAnsiView("settings"), payload->SettingsHash);
    builder.AddString(StringAnsiView("extension"), payload->SourceExtension);
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

bool ImportedSourceProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const auto* payload = static_cast<const ImportedSourcePreparedPayload*>(prepared.Payload.get());
    if (!payload)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, StringView::Empty, TEXT("Imported source prepared payload is missing."));
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
            prepared.AssetID, StringView::Empty, TEXT("Imported source build has no declared source."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    const bool video = prepared.OutputType == TEXT("FlaxEngine.Video");
    String scratchPath;
    if (context.CreateScratchFilePath(video ? payload->SourceExtension : String(TEXT(".flax")), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (video)
    {
        if (File::WriteAllBytes(scratchPath, sourceBytes.Get(), sourceBytes.Count()))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, scratchPath, TEXT("Video source snapshot could not be written."));
    }
    else
    {
        String verifiedSourcePath;
        if (context.CreateScratchFilePath(payload->SourceExtension, verifiedSourcePath, diagnostic) ||
            File::WriteAllBytes(verifiedSourcePath, sourceBytes.Get(), sourceBytes.Count()))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, verifiedSourcePath, TEXT("Imported source snapshot could not be created."));
        SCOPE_EXIT
        {
            ContentStorageManager::EnsureAccess(verifiedSourcePath);
            FileSystem::DeleteFile(verifiedSourcePath);
        };
        void* importerOptions = nullptr;
#if COMPILE_WITH_AUDIO_TOOL
        importerOptions = payload->HasAudioOptions ? const_cast<AudioTool::Options*>(&payload->AudioOptions) : nullptr;
#endif
        CreateAssetContext importerContext(verifiedSourcePath, scratchPath, prepared.AssetID, importerOptions, true, prepared.OutputType);
        CreateAssetFunction callback;
#if COMPILE_WITH_AUDIO_TOOL
        if (payload->SourceExtension == TEXT(".wav"))
            callback.Bind(&ImportAudio::ImportWav);
        else if (payload->SourceExtension == TEXT(".mp3"))
            callback.Bind(&ImportAudio::ImportMp3);
#if COMPILE_WITH_OGG_VORBIS
        else if (payload->SourceExtension == TEXT(".ogg"))
            callback.Bind(&ImportAudio::ImportOgg);
#endif
        else
#endif
        if (payload->SourceExtension == TEXT(".ttf") || payload->SourceExtension == TEXT(".otf"))
            callback.Bind(&ImportFont::Import);
        else if (payload->SourceExtension == TEXT(".shader"))
            callback.Bind(&ImportShader::Import);
        if (!callback.IsBinded())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, verifiedSourcePath, TEXT("No importer is registered for this source extension."));
        const CreateAssetResult importResult = importerContext.Run(callback);
        if (importResult != CreateAssetResult::Ok)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, verifiedSourcePath, TEXT("Imported source compatibility importer failed."));
    }
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Imported source artifact is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(video ? TEXT("video.bin") : TEXT("imported.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
