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
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Tools/TextureTool/TextureTool.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    void AddU16LE(Array<byte>& data, uint16 value)
    {
        data.Add(static_cast<byte>(value));
        data.Add(static_cast<byte>(value >> 8));
    }

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

    bool WriteMidtonePng(const StringView& path)
    {
        TextureData data;
        data.Width = 2;
        data.Height = 1;
        data.Depth = 1;
        data.Format = PixelFormat::R8G8B8A8_UNorm;
        data.Items.Resize(1);
        data.Items[0].Mips.Resize(1);
        TextureMipData* mip = data.GetData(0, 0);
        mip->RowPitch = data.Width * 4;
        mip->DepthPitch = mip->RowPitch;
        mip->Lines = data.Height;
        mip->Data.Allocate(mip->DepthPitch);
        for (int32 i = 0; i < data.Width; i++)
        {
            mip->Data[i * 4 + 0] = 64;
            mip->Data[i * 4 + 1] = 64;
            mip->Data[i * 4 + 2] = 64;
            mip->Data[i * 4 + 3] = 255;
        }
        return TextureTool::ExportTexture(path, data);
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

    Array<byte> MakeBmp(int32 width, int32 height)
    {
        Array<byte> result;
        result.Resize(54);
        Platform::MemoryClear(result.Get(), result.Count());
        result[0] = 'B';
        result[1] = 'M';
        result[14] = 40;
        result[18] = static_cast<byte>(width);
        result[19] = static_cast<byte>(width >> 8);
        result[22] = static_cast<byte>(height);
        result[23] = static_cast<byte>(height >> 8);
        result[26] = 1;
        result[28] = 32;
        return result;
    }

    Array<byte> MakeGif(uint16 width, uint16 height)
    {
        Array<byte> result;
        const char signature[] = "GIF89a";
        result.Add(reinterpret_cast<const byte*>(signature), 6);
        AddU16LE(result, width);
        AddU16LE(result, height);
        return result;
    }

    Array<byte> MakeJpeg(uint16 width, uint16 height)
    {
        Array<byte> result;
        const byte bytes[] = {
            0xff, 0xd8, 0xff, 0xc0, 0x00, 0x08, 0x08,
            static_cast<byte>(height >> 8), static_cast<byte>(height),
            static_cast<byte>(width >> 8), static_cast<byte>(width), 0x01
        };
        result.Add(bytes, ARRAY_COUNT(bytes));
        return result;
    }

    void AddTiffEntry(Array<byte>& data, uint16 tag, uint16 type, uint32 value)
    {
        AddU16LE(data, tag);
        AddU16LE(data, type);
        AddU32LE(data, 1);
        if (type == 3)
        {
            AddU16LE(data, static_cast<uint16>(value));
            AddU16LE(data, 0);
        }
        else
        {
            AddU32LE(data, value);
        }
    }

    Array<byte> MakeTiff(uint32 width, uint32 height)
    {
        Array<byte> result;
        result.Add('I');
        result.Add('I');
        AddU16LE(result, 42);
        AddU32LE(result, 8);
        AddU16LE(result, 4);
        AddTiffEntry(result, 256, 4, width);
        AddTiffEntry(result, 257, 4, height);
        AddTiffEntry(result, 258, 3, 8);
        AddTiffEntry(result, 277, 3, 4);
        AddU32LE(result, 0);
        return result;
    }

    Array<byte> MakeDds(uint32 width, uint32 height)
    {
        Array<byte> result;
        result.Resize(128);
        Platform::MemoryClear(result.Get(), result.Count());
        Platform::MemoryCopy(result.Get(), "DDS ", 4);
        result[4] = 124;
        result[12] = static_cast<byte>(height);
        result[13] = static_cast<byte>(height >> 8);
        result[16] = static_cast<byte>(width);
        result[17] = static_cast<byte>(width >> 8);
        result[76] = 32;
        return result;
    }

    Array<byte> MakeHdr(int32 width, int32 height)
    {
        const StringAnsi text = StringAnsi::Format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {0} +X {1}\n", height, width);
        Array<byte> result;
        result.Add(reinterpret_cast<const byte*>(text.Get()), text.Length());
        return result;
    }

    Array<byte> MakeRaw(int32 size)
    {
        Array<byte> result;
        result.Resize(size * size * 2);
        Platform::MemoryClear(result.Get(), result.Count());
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
        record.LocalId = 1;
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

TEST_CASE("Texture processor Prepare probes every supported source format deterministically without writing outputs")
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
    REQUIRE(lease.Get().Outputs.Count() == 1);
    CHECK(lease.Get().Outputs[0].Kind == StringAnsiView("runtime"));

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
    fixtures.Add({ TEXT("probe.bmp"), MakeBmp(41, 23), TextureSourceFormat::Bmp, 41, 23 });
    fixtures.Add({ TEXT("probe.gif"), MakeGif(29, 13), TextureSourceFormat::Gif, 29, 13 });
    fixtures.Add({ TEXT("probe.tiff"), MakeTiff(37, 21), TextureSourceFormat::Tiff, 37, 21 });
    fixtures.Add({ TEXT("probe.jpg"), MakeJpeg(31, 15), TextureSourceFormat::Jpeg, 31, 15 });
    fixtures.Add({ TEXT("probe.dds"), MakeDds(48, 24), TextureSourceFormat::Dds, 48, 24 });
    fixtures.Add({ TEXT("probe.hdr"), MakeHdr(43, 27), TextureSourceFormat::Hdr, 43, 27 });
    fixtures.Add({ TEXT("probe.raw"), MakeRaw(16), TextureSourceFormat::Raw, 16, 16 });

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
        REQUIRE(first.Outputs.Count() == 1);
        CHECK(first.Outputs[0].Kind == StringAnsiView("runtime"));

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

TEST_CASE("Texture processor Build writes a load-compatible runtime artifact only to Library staging")
{
    const String root = Globals::TemporaryFolder / (TEXT("TextureBuild-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String sourcePath = content / TEXT("source.png");
    REQUIRE_FALSE(WriteMidtonePng(sourcePath));
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
    REQUIRE(buildContext.GetFiles().Count() == 1);

    String runtimePath;
    for (const StagedArtifactFile& file : buildContext.GetFiles())
    {
        if (file.OutputKind == "runtime")
            runtimePath = file.AbsolutePath;
    }
    REQUIRE(runtimePath.HasChars());
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
    CHECK(header.Width == 2);
    CHECK(header.Height == 1);
    CHECK(header.MipLevels >= 1);
    CHECK(initData.Header.Chunks[0] != nullptr);

    AssetLoadLocation location;
    location.Info = AssetInfo(record.ID, Texture::TypeName, sourcePath);
    location.Artifact.ObjectID = AssetObjectId::Main(AssetGuid(location.Info.ObjectID));
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

    ArtifactManifestOutput runtimeOutput;
    runtimeOutput.Kind = "runtime";
    runtimeOutput.FormatVersion = TextureProcessor::RuntimeFormatVersion;
    runtimeOutput.Size = FileSystem::GetFileSize(runtimePath);
    runtimeOutput.Compatibility = "flax-texture-v4";
    REQUIRE_FALSE(TextureArtifactValidator::ValidateRuntime(runtimePath, runtimeOutput, record.ID, Texture::TypeName, diagnostic));
    CHECK(TextureArtifactValidator::ValidateRuntime(runtimePath, runtimeOutput, Guid::New(), Texture::TypeName, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
    storage = nullptr;
    ContentStorageManager::EnsureAccess(runtimePath);
}

TEST_CASE("Texture processor runtime output keys isolate target overrides")
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
    Array<ArtifactKeyComponent> runtimeComponents;
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(prepared, windows, StringAnsiView("runtime"), windowsRuntime, runtimeComponents, diagnostic));
    ArtifactKey unsupportedOutput;
    Array<ArtifactKeyComponent> unsupportedComponents;
    CHECK(TextureProcessor::BuildOutputKey(prepared, windows, StringAnsiView("thumbnail"), unsupportedOutput, unsupportedComponents, diagnostic));

    ArtifactTarget android = windows;
    android.Platform = "Android";
    android.Architecture = "arm64";
    android.Graphics = "Vulkan";
    android.TextureCompression = "Mobile";
    ArtifactKey androidRuntime;
    Array<ArtifactKeyComponent> ignored;
    REQUIRE_FALSE(TextureProcessor::BuildOutputKey(prepared, android, StringAnsiView("runtime"), androidRuntime, ignored, diagnostic));
    CHECK(androidRuntime != windowsRuntime);

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
