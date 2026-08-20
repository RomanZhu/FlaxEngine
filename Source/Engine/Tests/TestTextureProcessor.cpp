// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && COMPILE_WITH_TEXTURE_TOOL

#include "Engine/Content/Build/Processors/TextureProcessor.h"
#include "Engine/Content/Build/Processors/TextureArtifactValidator.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include "Engine/Content/Artifacts/ResolvedArtifact.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Factories/IAssetFactory.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Graphics/Textures/Types.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    void AddU32LE(Array<byte>& data, uint32 value)
    {
        data.Add(static_cast<byte>(value));
        data.Add(static_cast<byte>(value >> 8));
        data.Add(static_cast<byte>(value >> 16));
        data.Add(static_cast<byte>(value >> 24));
    }

    void AddAscii(Array<byte>& data, const char* value)
    {
        while (*value)
            data.Add(static_cast<byte>(*value++));
        data.Add(0);
    }

    void AddExrAttribute(Array<byte>& data, const char* name, const char* type, const Array<byte>& value)
    {
        AddAscii(data, name);
        AddAscii(data, type);
        AddU32LE(data, value.Count());
        data.Add(value.Get(), value.Count());
    }

    Array<byte> MakePng(uint32 width, uint32 height)
    {
        Array<byte> result;
        const byte header[] =
        {
            137, 80, 78, 71, 13, 10, 26, 10,
            0, 0, 0, 13, 'I', 'H', 'D', 'R',
            static_cast<byte>(width >> 24), static_cast<byte>(width >> 16), static_cast<byte>(width >> 8), static_cast<byte>(width),
            static_cast<byte>(height >> 24), static_cast<byte>(height >> 16), static_cast<byte>(height >> 8), static_cast<byte>(height),
            8, 6, 0, 0, 0, 0, 0, 0, 0
        };
        result.Add(header, ARRAY_COUNT(header));
        return result;
    }

    Array<byte> MakeValidPng()
    {
        static const byte bytes[] =
        {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
            0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc, 0xff, 0x1f, 0x00, 0x02, 0xeb,
            0x01, 0xf5, 0x8f, 0x59, 0x97, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
        };
        Array<byte> result;
        result.Add(bytes, ARRAY_COUNT(bytes));
        return result;
    }

    Array<byte> MakeTga(uint16 width, uint16 height)
    {
        Array<byte> result;
        result.Resize(18);
        Platform::MemoryClear(result.Get(), result.Count());
        result[2] = 2;
        result[12] = static_cast<byte>(width);
        result[13] = static_cast<byte>(width >> 8);
        result[14] = static_cast<byte>(height);
        result[15] = static_cast<byte>(height >> 8);
        result[16] = 32;
        result[17] = 8;
        return result;
    }

    Array<byte> MakeExr(int32 width, int32 height)
    {
        Array<byte> result;
        AddU32LE(result, 0x762f3101);
        AddU32LE(result, 2);

        Array<byte> channels;
        AddAscii(channels, "R");
        AddU32LE(channels, 1);
        AddU32LE(channels, 0);
        AddU32LE(channels, 1);
        AddU32LE(channels, 1);
        channels.Add(0);
        AddExrAttribute(result, "channels", "chlist", channels);

        Array<byte> compression;
        compression.Add(0);
        AddExrAttribute(result, "compression", "compression", compression);

        Array<byte> dataWindow;
        AddU32LE(dataWindow, 0);
        AddU32LE(dataWindow, 0);
        AddU32LE(dataWindow, width - 1);
        AddU32LE(dataWindow, height - 1);
        AddExrAttribute(result, "dataWindow", "box2i", dataWindow);
        result.Add(0);
        return result;
    }

    AssetRecord MakeTextureRecord(const String& path)
    {
        AssetRecord record;
        record.ID = Guid::New();
        record.SourceAssetID = record.ID;
        record.TypeName = TEXT("FlaxEngine.Texture");
        record.CanonicalPath = CanonicalAssetPath(path);
        record.SourcePath = SourceFilePath(path);
        record.ProcessorID = TextureProcessorSettings::ProcessorID();
        record.SourceKind = AssetSourceKind::ImportedSource;
        record.Status = AssetRecordStatus::Ready;
        record.DatabaseRevision = 1;
        return record;
    }
}

TEST_CASE("Texture processor Prepare probes PNG TGA and EXR deterministically without writing outputs")
{
    const String root = Globals::TemporaryFolder / (TEXT("TexturePrepare-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    AssetProcessorRegistry registry;
    AssetProcessorRegistration registration;
    REQUIRE_FALSE(registry.Register(TextureProcessor::CreateDescriptor(), registration, diagnostic));
    AssetProcessorLease lease;
    REQUIRE_FALSE(registry.TryAcquire(TextureProcessorSettings::ProcessorID(), AssetProcessorInvocationStage::Prepare, lease, diagnostic));
    REQUIRE(lease.Get().Outputs.Count() == 2);

    StringAnsi settingsJson;
    REQUIRE_FALSE(TextureProcessorSettings::Defaults().ToJson(settingsJson, diagnostic));
    SourceHashCache hashCache;
    struct Fixture
    {
        const Char* Name;
        Array<byte> Bytes;
        TextureSourceFormat Format;
        int32 Width;
        int32 Height;
    };
    Array<Fixture> fixtures;
    fixtures.Add({ TEXT("probe.png"), MakePng(64, 32), TextureSourceFormat::Png, 64, 32 });
    fixtures.Add({ TEXT("probe.tga"), MakeTga(33, 17), TextureSourceFormat::Tga, 33, 17 });
    fixtures.Add({ TEXT("probe.exr"), MakeExr(19, 11), TextureSourceFormat::Exr, 19, 11 });

    for (const Fixture& fixture : fixtures)
    {
        const String sourcePath = content / fixture.Name;
        REQUIRE_FALSE(File::WriteAllBytes(sourcePath, fixture.Bytes.Get(), fixture.Bytes.Count()));
        const AssetRecord record = MakeTextureRecord(sourcePath);
        AssetCancellationSource cancellation;
        PreparedAsset first;
        PrepareAssetContext firstContext(root, content, library, record, lease.Get(), settingsJson, hashCache, cancellation.GetToken());
        REQUIRE_FALSE(lease.Get().Prepare(firstContext, first, diagnostic));
        REQUIRE_FALSE(firstContext.Finalize(record.DatabaseRevision, first, diagnostic));
        REQUIRE(first.Payload);
        const auto* payload = static_cast<const TexturePreparedPayload*>(first.Payload.get());
        CHECK(payload->SourceFormat == fixture.Format);
        CHECK(payload->Width == fixture.Width);
        CHECK(payload->Height == fixture.Height);
        CHECK(payload->EstimatedDecodedBytes > 0);
        CHECK(payload->EstimatedOutputBytes > 0);
        CHECK(first.MemoryEstimate >= payload->EstimatedDecodedBytes);
        REQUIRE(first.Dependencies.Count() == 3);
        CHECK(first.Dependencies[0].Kind == AssetDependencyKind::SourceFile);
        CHECK(first.Outputs.Count() == 2);

        PreparedAsset second;
        PrepareAssetContext secondContext(root, content, library, record, lease.Get(), settingsJson, hashCache, cancellation.GetToken());
        REQUIRE_FALSE(lease.Get().Prepare(secondContext, second, diagnostic));
        REQUIRE_FALSE(secondContext.Finalize(record.DatabaseRevision, second, diagnostic));
        CHECK(second.InputFingerprint == first.InputFingerprint);
        CHECK(second.MemoryEstimate == first.MemoryEstimate);
    }

    Array<String> generatedFiles;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(generatedFiles, library, TEXT("*"), DirectorySearchOption::AllDirectories));
    CHECK(generatedFiles.IsEmpty());
}

TEST_CASE("Texture processor Prepare rejects malformed oversized invalid-settings and cancelled input")
{
    const String root = Globals::TemporaryFolder / (TEXT("TexturePrepareFailure-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    const AssetProcessorDescriptor descriptor = TextureProcessor::CreateDescriptor();
    StringAnsi settingsJson;
    REQUIRE_FALSE(TextureProcessorSettings::Defaults().ToJson(settingsJson, diagnostic));
    SourceHashCache hashCache;
    AssetCancellationSource cancellation;

    const String malformedPath = content / TEXT("malformed.png");
    const byte malformed[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    REQUIRE_FALSE(File::WriteAllBytes(malformedPath, malformed, ARRAY_COUNT(malformed)));
    AssetRecord record = MakeTextureRecord(malformedPath);
    PreparedAsset prepared;
    PrepareAssetContext malformedContext(root, content, library, record, descriptor, settingsJson, hashCache, cancellation.GetToken());
    CHECK(TextureProcessor::Prepare(malformedContext, prepared, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
    CHECK(diagnostic.Stage == AssetPipelineDiagnosticStage::Prepare);
    CHECK(diagnostic.SourcePath == malformedPath);

    const String oversizedPath = content / TEXT("oversized.png");
    const Array<byte> oversized = MakePng(TextureProcessor::MaximumDimension, TextureProcessor::MaximumDimension);
    REQUIRE_FALSE(File::WriteAllBytes(oversizedPath, oversized.Get(), oversized.Count()));
    record = MakeTextureRecord(oversizedPath);
    PrepareAssetContext oversizedContext(root, content, library, record, descriptor, settingsJson, hashCache, cancellation.GetToken());
    CHECK(TextureProcessor::Prepare(oversizedContext, prepared, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);

    const String validPath = content / TEXT("valid.png");
    const Array<byte> valid = MakePng(8, 8);
    REQUIRE_FALSE(File::WriteAllBytes(validPath, valid.Get(), valid.Count()));
    record = MakeTextureRecord(validPath);
    PrepareAssetContext settingsContext(root, content, library, record, descriptor, StringAnsiView("{\"type\":\"NoSuchType\"}"), hashCache, cancellation.GetToken());
    CHECK(TextureProcessor::Prepare(settingsContext, prepared, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    AssetCancellationSource cancelled;
    cancelled.Cancel();
    PrepareAssetContext cancelledContext(root, content, library, record, descriptor, settingsJson, hashCache, cancelled.GetToken());
    CHECK(TextureProcessor::Prepare(cancelledContext, prepared, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCancelled);
}

#if COMPILE_WITH_ASSETS_IMPORTER

TEST_CASE("Texture processor Build writes load-compatible runtime and thumbnail artifacts only to Library staging")
{
    const String root = Globals::TemporaryFolder / (TEXT("TextureBuild-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String sourcePath = content / TEXT("source.png");
    const Array<byte> sourceBytes = MakeValidPng();
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, sourceBytes.Get(), sourceBytes.Count()));
    const AssetRecord record = MakeTextureRecord(sourcePath);
    const AssetProcessorDescriptor descriptor = TextureProcessor::CreateDescriptor();
    StringAnsi settingsJson;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(TextureProcessorSettings::Defaults().ToJson(settingsJson, diagnostic));
    SourceHashCache hashCache;
    AssetCancellationSource cancellation;
    PreparedAsset prepared;
    PrepareAssetContext prepareContext(root, content, library, record, descriptor, settingsJson, hashCache, cancellation.GetToken());
    REQUIRE_FALSE(descriptor.Prepare(prepareContext, prepared, diagnostic));
    REQUIRE_FALSE(prepareContext.Finalize(record.DatabaseRevision, prepared, diagnostic));

    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceFile)
        {
            sourceDependency = &dependency;
            break;
        }
    }
    REQUIRE(sourceDependency);
    ArtifactBuildInput input;
    input.StableIdentity = sourceDependency->StableIdentity;
    input.Path = sourcePath;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    ArtifactTarget target;
    target.Platform = "Windows";
    target.Architecture = "x64";
    target.Graphics = "DirectX12";
    target.Configuration = "Development";
    target.TextureCompression = "Desktop";
    target.Role = "Editor";
    ArtifactBuildContext buildContext(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken(), target);
    REQUIRE_FALSE(buildContext.Initialize(diagnostic));
    REQUIRE_FALSE(descriptor.Build(buildContext, diagnostic));
    REQUIRE_FALSE(buildContext.Close(diagnostic));
    REQUIRE(buildContext.GetFiles().Count() == 2);

    String runtimePath;
    String thumbnailPath;
    for (const StagedArtifactFile& file : buildContext.GetFiles())
    {
        if (file.OutputKind == "runtime")
            runtimePath = file.AbsolutePath;
        else if (file.OutputKind == "thumbnail")
            thumbnailPath = file.AbsolutePath;
    }
    REQUIRE(runtimePath.HasChars());
    REQUIRE(thumbnailPath.HasChars());
    Array<String> contentBinaries;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(contentBinaries, content, TEXT("*.flax"), DirectorySearchOption::AllDirectories));
    CHECK(contentBinaries.IsEmpty());

    auto storage = ContentStorageManager::GetStorage(runtimePath);
    REQUIRE(storage);
    Array<FlaxStorage::Entry> entries;
    storage->GetEntries(entries);
    REQUIRE(entries.Count() == 1);
    CHECK(entries[0].ID == record.ID);
    CHECK(entries[0].TypeName == Texture::TypeName);
    AssetInitData initData;
    REQUIRE_FALSE(storage->LoadAssetHeader(record.ID, initData));
    CHECK(initData.SerializedVersion == Texture::SerializedVersion);
    CHECK(initData.Metadata.IsInvalid());
    REQUIRE(initData.CustomData.Length() == sizeof(TextureHeader));
    TextureHeader header;
    Platform::MemoryCopy(&header, initData.CustomData.Get(), sizeof(TextureHeader));
    CHECK(header.Width == 1);
    CHECK(header.Height == 1);
    CHECK(header.MipLevels >= 1);
    CHECK(initData.Header.Chunks[0] != nullptr);

    AssetLoadLocation location;
    location.Info = AssetInfo(record.ID, Texture::TypeName, sourcePath);
    location.Artifact.AssetID = record.ID;
    location.Artifact.TypeName = Texture::TypeName;
    location.Artifact.StoragePath = ArtifactStoragePath(runtimePath);
    location.Artifact.OutputKind = TEXT("runtime");
    location.Artifact.Key = TEXT("texture-compatibility-test");
    location.Artifact.StorageKind = ArtifactStorageKind::Generated;
    location.Artifact.IsExact = true;
    IAssetFactory* factory = Content::GetAssetFactory(location.Info);
    REQUIRE(factory);
    Asset* asset = factory->New(location);
    REQUIRE(asset);
    CHECK(asset->Is<Texture>());
    CHECK(asset->GetPath() == sourcePath);
    CHECK(static_cast<Texture*>(asset)->GetStoragePath() == runtimePath);
    Delete(asset);

    Array<byte> thumbnailBytes;
    REQUIRE_FALSE(File::ReadAllBytes(thumbnailPath, thumbnailBytes));
    static const byte pngSignature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    REQUIRE(thumbnailBytes.Count() >= ARRAY_COUNT(pngSignature));
    CHECK(Platform::MemoryCompare(thumbnailBytes.Get(), pngSignature, ARRAY_COUNT(pngSignature)) == 0);

    ArtifactManifestOutput runtimeOutput;
    runtimeOutput.Kind = "runtime";
    runtimeOutput.FormatVersion = TextureProcessor::RuntimeFormatVersion;
    runtimeOutput.Size = FileSystem::GetFileSize(runtimePath);
    runtimeOutput.Compatibility = "flax-texture-v4";
    REQUIRE_FALSE(TextureArtifactValidator::ValidateRuntime(runtimePath, runtimeOutput, record.ID, diagnostic));
    CHECK(TextureArtifactValidator::ValidateRuntime(runtimePath, runtimeOutput, Guid::New(), diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
    ArtifactManifestOutput thumbnailOutput;
    thumbnailOutput.Kind = "thumbnail";
    thumbnailOutput.FormatVersion = TextureProcessor::ThumbnailFormatVersion;
    thumbnailOutput.Size = FileSystem::GetFileSize(thumbnailPath);
    thumbnailOutput.Compatibility = "flax-texture-thumbnail-v1";
    REQUIRE_FALSE(TextureArtifactValidator::ValidateThumbnail(thumbnailPath, thumbnailOutput, diagnostic));
    storage = nullptr;
    ContentStorageManager::EnsureAccess(runtimePath);
}

TEST_CASE("Texture processor output keys isolate target overrides and thumbnail dimensions")
{
    const String root = Globals::TemporaryFolder / (TEXT("TextureKeys-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String sourcePath = content / TEXT("source.png");
    const Array<byte> sourceBytes = MakeValidPng();
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, sourceBytes.Get(), sourceBytes.Count()));
    const AssetRecord record = MakeTextureRecord(sourcePath);
    const AssetProcessorDescriptor descriptor = TextureProcessor::CreateDescriptor();
    AssetPipelineDiagnostic diagnostic;
    StringAnsi settingsJson;
    REQUIRE_FALSE(TextureProcessorSettings::Defaults().ToJson(settingsJson, diagnostic));
    SourceHashCache hashCache;
    AssetCancellationSource cancellation;
    PreparedAsset prepared;
    PrepareAssetContext context(root, content, library, record, descriptor, settingsJson, hashCache, cancellation.GetToken());
    REQUIRE_FALSE(descriptor.Prepare(context, prepared, diagnostic));
    REQUIRE_FALSE(context.Finalize(record.DatabaseRevision, prepared, diagnostic));

    ArtifactTarget windows;
    windows.Platform = "Windows";
    windows.Architecture = "x64";
    windows.Graphics = "DirectX12";
    windows.TextureCompression = "Desktop";
    ArtifactKey windowsRuntime;
    ArtifactKey windowsThumbnail;
    Array<ArtifactKeyComponent> runtimeComponents;
    Array<ArtifactKeyComponent> thumbnailComponents;
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(prepared, windows, StringAnsiView("runtime"), windowsRuntime, runtimeComponents, diagnostic));
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(prepared, windows, StringAnsiView("thumbnail"), windowsThumbnail, thumbnailComponents, diagnostic));

    ArtifactTarget android = windows;
    android.Platform = "Android";
    android.Architecture = "arm64";
    android.Graphics = "Vulkan";
    android.TextureCompression = "Mobile";
    ArtifactKey androidRuntime;
    ArtifactKey androidThumbnail;
    Array<ArtifactKeyComponent> ignored;
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(prepared, android, StringAnsiView("runtime"), androidRuntime, ignored, diagnostic));
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(prepared, android, StringAnsiView("thumbnail"), androidThumbnail, ignored, diagnostic));
    CHECK(androidRuntime != windowsRuntime);
    CHECK(androidThumbnail == windowsThumbnail);

    TextureProcessorSettings withAndroidOverride = TextureProcessorSettings::Defaults();
    TextureProcessorPlatformOverride overrideSettings;
    overrideSettings.HasCompression = true;
    overrideSettings.Compress = false;
    overrideSettings.MaxSize = 1024;
    withAndroidOverride.PlatformOverrides.Add("android", overrideSettings);
    REQUIRE_FALSE(withAndroidOverride.ToJson(settingsJson, diagnostic));
    PreparedAsset overridePrepared;
    PrepareAssetContext overrideContext(root, content, library, record, descriptor, settingsJson, hashCache, cancellation.GetToken());
    REQUIRE_FALSE(descriptor.Prepare(overrideContext, overridePrepared, diagnostic));
    REQUIRE_FALSE(overrideContext.Finalize(record.DatabaseRevision, overridePrepared, diagnostic));
    ArtifactKey unchangedWindowsRuntime;
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(overridePrepared, windows, StringAnsiView("runtime"), unchangedWindowsRuntime, ignored, diagnostic));
    CHECK(unchangedWindowsRuntime == windowsRuntime);

    bool hasEffectiveSettings = false;
    bool hasGraphicsAbi = false;
    for (const ArtifactKeyComponent& component : runtimeComponents)
    {
        hasEffectiveSettings |= component.Name == "effective-settings";
        hasGraphicsAbi |= component.Name == "graphics-runtime-abi";
    }
    CHECK(hasEffectiveSettings);
    CHECK(hasGraphicsAbi);
}

TEST_CASE("Texture processor Build fails safely for decoder errors and cancellation")
{
    const String root = Globals::TemporaryFolder / (TEXT("TextureBuildFailure-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String sourcePath = content / TEXT("header-only.png");
    const Array<byte> sourceBytes = MakePng(8, 8);
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, sourceBytes.Get(), sourceBytes.Count()));
    const AssetRecord record = MakeTextureRecord(sourcePath);
    const AssetProcessorDescriptor descriptor = TextureProcessor::CreateDescriptor();
    StringAnsi settingsJson;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(TextureProcessorSettings::Defaults().ToJson(settingsJson, diagnostic));
    SourceHashCache hashCache;
    AssetCancellationSource cancellation;
    PreparedAsset prepared;
    PrepareAssetContext prepareContext(root, content, library, record, descriptor, settingsJson, hashCache, cancellation.GetToken());
    REQUIRE_FALSE(descriptor.Prepare(prepareContext, prepared, diagnostic));
    REQUIRE_FALSE(prepareContext.Finalize(record.DatabaseRevision, prepared, diagnostic));
    ArtifactBuildInput input;
    input.Path = sourcePath;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceFile)
            input.StableIdentity = dependency.StableIdentity;
    }
    REQUIRE(input.StableIdentity.HasChars());
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    ArtifactTarget target;
    target.Platform = "Windows";

    ArtifactBuildContext decoderFailure(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken(), target);
    REQUIRE_FALSE(decoderFailure.Initialize(diagnostic));
    CHECK(descriptor.Build(decoderFailure, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildFailed);
    CHECK(decoderFailure.GetFiles().IsEmpty());
    const String failedStaging = decoderFailure.GetStagingPath();
    decoderFailure.Cancel();
    CHECK_FALSE(FileSystem::DirectoryExists(failedStaging));

    AssetCancellationSource cancelled;
    ArtifactBuildContext cancelledBuild(root, content, library, Guid::New(), prepared, inputs, cancelled.GetToken(), target);
    REQUIRE_FALSE(cancelledBuild.Initialize(diagnostic));
    const String cancelledStaging = cancelledBuild.GetStagingPath();
    cancelled.Cancel();
    CHECK(descriptor.Build(cancelledBuild, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCancelled);
    cancelledBuild.Cancel();
    CHECK_FALSE(FileSystem::DirectoryExists(cancelledStaging));

    Array<String> contentBinaries;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(contentBinaries, content, TEXT("*.flax"), DirectorySearchOption::AllDirectories));
    CHECK(contentBinaries.IsEmpty());
}

#endif

#endif
