// Copyright (c) Wojciech Figat. All rights reserved.

#include "CookAssetsStep.h"
#include "Editor/Cooker/PlatformTools.h"
#include "Engine/Core/DeleteMe.h"
#include "Engine/Core/Utilities.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Asset.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/Build/AssetBuildSnapshot.h"
#include "Engine/Content/Build/RuntimeAssetCatalog.h"
#include "Engine/Content/Build/RuntimeDependencyClosure.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Render2D/SpriteAtlas.h"
#include "Engine/Content/Storage/FlaxFile.h"
#include "Engine/Particles/ParticleEmitter.h"
#include "Engine/Utilities/Encryption.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/FileWriteStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Core/Config/PlatformSettings.h"
#include "Engine/Core/Config/GameSettings.h"
#include "Engine/Core/Config/BuildSettings.h"
#include "Engine/Streaming/StreamingSettings.h"
#include "Engine/ShadersCompilation/ShadersCompilation.h"
#include "Engine/Graphics/RenderTools.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Graphics/Materials/MaterialShader.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
#include "Engine/Level/Level.h"
#include "Engine/Particles/Graph/GPU/ParticleEmitterGraph.GPU.h"
#include "Engine/Engine/Base/GameBase.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Tools/TextureTool/TextureTool.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Scripting/Enums.h"
#include "Engine/Platform/File.h"
#include "Engine/Core/Types/DataContainer.h"
#if PLATFORM_TOOLS_WINDOWS
#include "Engine/Platform/Windows/WindowsPlatformSettings.h"
#endif
#if PLATFORM_TOOLS_UWP
#include "Engine/Platform/UWP/UWPPlatformSettings.h"
#endif
#if PLATFORM_TOOLS_LINUX
#include "Engine/Platform/Linux/LinuxPlatformSettings.h"
#endif
#include "FlaxEngine.Gen.h"
#include <algorithm>

Dictionary<String, CookAssetsStep::ProcessAssetFunc> CookAssetsStep::AssetProcessors;

namespace
{
    bool LessGuid(const Guid& a, const Guid& b)
    {
        if (a.A != b.A)
            return a.A < b.A;
        if (a.B != b.B)
            return a.B < b.B;
        if (a.C != b.C)
            return a.C < b.C;
        return a.D < b.D;
    }

    bool LessAssetObjectId(const AssetObjectId& a, const AssetObjectId& b)
    {
        if (a.Asset.Value != b.Asset.Value)
            return LessGuid(a.Asset.Value, b.Asset.Value);
        return a.LocalId < b.LocalId;
    }

    ArtifactTarget GetCookArtifactTarget(const CookingData& data)
    {
        ArtifactTarget target;
        switch (data.Platform)
        {
        case BuildPlatform::Windows32:
            target.Platform = "Windows";
            target.Architecture = "x86";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::Windows64:
            target.Platform = "Windows";
            target.Architecture = "x64";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::WindowsARM64:
            target.Platform = "Windows";
            target.Architecture = "ARM64";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::UWPx86:
            target.Platform = "UWP";
            target.Architecture = "x86";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::UWPx64:
            target.Platform = "UWP";
            target.Architecture = "x64";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::XboxOne:
            target.Platform = "XboxOne";
            target.Architecture = "x64";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::XboxScarlett:
            target.Platform = "XboxScarlett";
            target.Architecture = "x64";
            target.Graphics = "DirectX12";
            break;
        case BuildPlatform::LinuxX64:
            target.Platform = "Linux";
            target.Architecture = "x64";
            target.Graphics = "Vulkan";
            break;
        case BuildPlatform::AndroidARM64:
            target.Platform = "Android";
            target.Architecture = "ARM64";
            target.Graphics = "Vulkan";
            break;
        case BuildPlatform::MacOSx64:
            target.Platform = "Mac";
            target.Architecture = "x64";
            target.Graphics = "Metal";
            break;
        case BuildPlatform::MacOSARM64:
            target.Platform = "Mac";
            target.Architecture = "ARM64";
            target.Graphics = "Metal";
            break;
        case BuildPlatform::iOSARM64:
            target.Platform = "iOS";
            target.Architecture = "ARM64";
            target.Graphics = "Metal";
            break;
        case BuildPlatform::PS4:
            target.Platform = "PS4";
            target.Architecture = "x64";
            target.Graphics = "GNM";
            break;
        case BuildPlatform::PS5:
            target.Platform = "PS5";
            target.Architecture = "x64";
            target.Graphics = "GNM";
            break;
        case BuildPlatform::Switch:
            target.Platform = "Switch";
            target.Architecture = "ARM64";
            target.Graphics = "NVN";
            break;
        case BuildPlatform::Web:
            target.Platform = "Web";
            target.Architecture = "x86";
            target.Graphics = "WebGPU";
            break;
        default:
            break;
        }
        target.Configuration = data.Configuration == BuildConfiguration::Debug ? "Debug" :
            data.Configuration == BuildConfiguration::Development ? "Development" : "Release";
        target.Quality = "Default";
        target.TextureCompression = data.Platform == BuildPlatform::AndroidARM64 || data.Platform == BuildPlatform::iOSARM64 || data.Platform == BuildPlatform::Switch
            ? "Mobile" : data.Platform == BuildPlatform::Web ? "Web" : "Desktop";
        target.Role = "Runtime";
        return target;
    }

    bool HashCookedFile(const StringView& path, ContentHash& result)
    {
        BytesContainer bytes;
        if (File::ReadAllBytes(path, bytes))
        {
            result = ContentHash();
            return true;
        }
        result = ContentHash::Compute(bytes.Get(), bytes.Length());
        return result.IsZero();
    }

    ContentHash BuildProjectSettingsHash(const BuildSettings& buildSettings, const GameSettings& gameSettings, int32 contentKey)
    {
        ArtifactKeyBuilder builder(StringAnsiView("flax-cook-project-settings-v1"));
        builder.AddString(StringAnsiView("product"), gameSettings.ProductName);
        builder.AddString(StringAnsiView("company"), gameSettings.CompanyName);
        builder.AddBool(StringAnsiView("no-splash"), gameSettings.NoSplashScreen);
        builder.AddGuid(StringAnsiView("splash-guid"), gameSettings.SplashScreen.Asset.Value);
        builder.AddUInt64(StringAnsiView("splash-file-id"), static_cast<uint64>(gameSettings.SplashScreen.LocalId));
        builder.AddGuid(StringAnsiView("streaming-guid"), gameSettings.Streaming.Asset.Value);
        builder.AddUInt64(StringAnsiView("streaming-file-id"), static_cast<uint64>(gameSettings.Streaming.LocalId));
        builder.AddUInt32(StringAnsiView("content-key"), static_cast<uint32>(contentKey));
        builder.AddUInt32(StringAnsiView("max-assets-per-package"), static_cast<uint32>(buildSettings.MaxAssetsPerPackage));
        builder.AddUInt32(StringAnsiView("max-package-size"), static_cast<uint32>(buildSettings.MaxPackageSizeMB));
        builder.AddBool(StringAnsiView("shader-optimization-disabled"), buildSettings.ShadersNoOptimize);
        builder.AddBool(StringAnsiView("shader-debug-data"), buildSettings.ShadersGenerateDebugData);
        builder.AddBool(StringAnsiView("skip-default-fonts"), buildSettings.SkipDefaultFonts);
        return builder.Finalize().Digest;
    }
}

void IBuildCache::InvalidateCacheShaders()
{
    InvalidateCachePerType<Shader>();
    InvalidateCachePerType<Material>();
    InvalidateCachePerType<ParticleEmitter>();
}

void IBuildCache::InvalidateCacheTextures()
{
    InvalidateCachePerType<Texture>();
    InvalidateCachePerType<CubeTexture>();
    InvalidateCachePerType<SpriteAtlas>();
}

bool CookAssetsStep::CacheEntry::IsValid(bool withDependencies)
{
    AssetInfo assetInfo;
    if (Content::GetRuntimeAssetInfo(ID, assetInfo))
    {
        if (TypeName == assetInfo.TypeName)
        {
            if (FileSystem::GetFileLastEditTime(assetInfo.Path) <= FileModified)
            {
                bool isValid = true;
                if (withDependencies)
                {
                    for (auto& f : FileDependencies)
                    {
                        if (FileSystem::GetFileLastEditTime(f.First) > f.Second)
                        {
                            isValid = false;
                            break;
                        }
                    }
                }
                if (isValid)
                    return true;
            }
        }
    }
    return false;
}

CookAssetsStep::CacheEntry& CookAssetsStep::CacheData::CreateEntry(const JsonAssetBase* asset, String& cachedFilePath)
{
    ASSERT(asset->DataTypeName.HasChars());
    auto& entry = Entries[asset->GetID()];
    entry.ID = asset->GetID();
    entry.TypeName = asset->DataTypeName;
    entry.FileModified = FileSystem::GetFileLastEditTime(asset->GetPath());
    cachedFilePath = CacheFolder / entry.ID.ToString(Guid::FormatType::N);
    return entry;
}

CookAssetsStep::CacheEntry& CookAssetsStep::CacheData::CreateEntry(const Asset* asset, String& cachedFilePath)
{
    auto& entry = Entries[asset->GetID()];
    entry.ID = asset->GetID();
    entry.TypeName = asset->GetTypeName();
    entry.FileModified = FileSystem::GetFileLastEditTime(asset->GetPath());
    cachedFilePath = CacheFolder / entry.ID.ToString(Guid::FormatType::N);
    return entry;
}

void CookAssetsStep::CacheData::InvalidateCachePerType(const StringView& typeName)
{
    LOG(Info, "Invalidating cooker cache for {0} assets.", typeName);
    for (auto e = Entries.Begin(); e.IsNotEnd(); ++e)
    {
        if (e->Value.TypeName == typeName)
        {
            Entries.Remove(e);
        }
    }
}

void CookAssetsStep::CacheData::Load(CookingData& data)
{
    PROFILE_CPU();
    HeaderFilePath = data.CacheDirectory / String::Format(TEXT("CookedHeader_{0}.bin"), FLAXENGINE_VERSION_BUILD);
    CacheFolder = data.CacheDirectory / TEXT("Cooked");
    Entries.Clear();

    if (!FileSystem::DirectoryExists(CacheFolder))
        FileSystem::CreateDirectory(CacheFolder);
    if (!FileSystem::FileExists(HeaderFilePath))
        return;

    auto file = FileReadStream::Open(HeaderFilePath);
    if (file == nullptr)
        return;
    DeleteMe<FileReadStream> deleteFile(file);

    int32 buildNum;
    file->ReadInt32(&buildNum);
    if (buildNum != FLAXENGINE_VERSION_BUILD)
        return;
    int32 entriesCount;
    file->ReadInt32(&entriesCount);
    if (Math::IsNotInRange(entriesCount, 0, 1000000))
        return;

    LOG(Info, "Loading incremental build cooking cache (entries count: {0})", entriesCount);
    file->ReadBytes(&Settings, sizeof(Settings));
    Entries.EnsureCapacity(entriesCount);

    Array<Pair<String, DateTime>> fileDependencies;
    for (int32 i = 0; i < entriesCount; i++)
    {
        Guid id;
        file->Read(id);
        String typeName;
        file->Read(typeName);
        DateTime fileModified;
        file->Read(fileModified);
        int32 fileDependenciesCount;
        file->ReadInt32(&fileDependenciesCount);
        fileDependencies.Clear();
        fileDependencies.Resize(fileDependenciesCount);
        for (int32 j = 0; j < fileDependenciesCount; j++)
        {
            Pair<String, DateTime>& f = fileDependencies[j];
            file->Read(f.First, 10);
            file->Read(f.Second);
        }

        // Skip missing entries
        if (!FileSystem::FileExists(CacheFolder / id.ToString(Guid::FormatType::N)))
            continue;

        auto& e = Entries[id];
        e.ID = id;
        e.TypeName = typeName;
        e.FileModified = fileModified;
        e.FileDependencies = fileDependencies;
    }

    Array<byte> platformCache;
    file->Read(platformCache);

    int32 checkChar;
    file->ReadInt32(&checkChar);
    if (checkChar != 13)
    {
        LOG(Warning, "Corrupted cooking cache header file.");
        Entries.Clear();
    }

    // Per-platform custom data loading (eg. to invalidate textures/shaders options)
    data.Tools->LoadCache(data, this, ToSpan(platformCache));

    const auto buildSettings = BuildSettings::Get();
    const auto gameSettings = GameSettings::Get();

    // Invalidate shaders and assets with shaders if need to rebuild them
    bool invalidateShaders = false;
    if (GPU_SHADER_CACHE_VERSION != Settings.Global.ShadersVersion)
    {
        LOG(Info, "{0} option has been modified.", TEXT("ShadersVersion"));
        invalidateShaders = true;
    }
    if (MATERIAL_GRAPH_VERSION != Settings.Global.MaterialGraphVersion)
    {
        LOG(Info, "{0} option has been modified.", TEXT("MaterialGraphVersion"));
        InvalidateCachePerType<Material>();
    }
    if (PARTICLE_GPU_GRAPH_VERSION != Settings.Global.ParticleGraphVersion)
    {
        LOG(Info, "{0} option has been modified.", TEXT("ParticleGraphVersion"));
        InvalidateCachePerType<ParticleEmitter>();
    }
    if (buildSettings->ShadersNoOptimize != Settings.Global.ShadersNoOptimize)
    {
        LOG(Info, "{0} option has been modified.", TEXT("ShadersNoOptimize"));
        invalidateShaders = true;
    }
    if (buildSettings->ShadersGenerateDebugData != Settings.Global.ShadersGenerateDebugData)
    {
        LOG(Info, "{0} option has been modified.", TEXT("ShadersGenerateDebugData"));
        invalidateShaders = true;
    }
#if PLATFORM_TOOLS_WINDOWS
    if (data.Platform == BuildPlatform::Windows32 || data.Platform == BuildPlatform::Windows64)
    {
        const auto settings = WindowsPlatformSettings::Get();
        const bool modified =
                Settings.Windows.SupportDX12 != settings->SupportDX12 ||
                Settings.Windows.SupportDX11 != settings->SupportDX11 ||
                Settings.Windows.SupportDX10 != settings->SupportDX10 ||
                Settings.Windows.SupportVulkan != settings->SupportVulkan;
        if (modified)
        {
            LOG(Info, "{0} option has been modified.", TEXT("Platform graphics backend"));
            invalidateShaders = true;
        }
    }
#endif
#if PLATFORM_TOOLS_UWP
    if (data.Platform == BuildPlatform::UWPx86 || data.Platform == BuildPlatform::UWPx64)
    {
        const auto settings = UWPPlatformSettings::Get();
        const bool modified =
                Settings.UWP.SupportDX11 != settings->SupportDX11 ||
                Settings.UWP.SupportDX10 != settings->SupportDX10;
        if (modified)
        {
            LOG(Info, "{0} option has been modified.", TEXT("Platform graphics backend"));
            invalidateShaders = true;
        }
    }
#endif
#if PLATFORM_TOOLS_LINUX
    if (data.Platform == BuildPlatform::LinuxX64)
    {
        const auto settings = LinuxPlatformSettings::Get();
        const bool modified =
                Settings.Linux.SupportVulkan != settings->SupportVulkan;
        if (modified)
        {
            LOG(Info, "{0} option has been modified.", TEXT("Platform graphics backend"));
            invalidateShaders = true;
        }
    }
#endif
    if (invalidateShaders)
    {
        InvalidateCachePerType<Shader>();
        InvalidateCachePerType<Material>();
        InvalidateCachePerType<ParticleEmitter>();
    }

    // Invalidate textures if streaming settings gets modified
    const Guid streamingSettingsRuntimeID = gameSettings->Streaming.IsValid() ? gameSettings->Streaming.ToRuntimeObjectGuid() : Guid::Empty;
    if (Settings.Global.StreamingSettingsAssetId != streamingSettingsRuntimeID || (Entries.ContainsKey(streamingSettingsRuntimeID) && !Entries[streamingSettingsRuntimeID].IsValid()))
    {
        InvalidateCachePerType<Texture>();
        InvalidateCachePerType<CubeTexture>();
        InvalidateCachePerType<SpriteAtlas>();
    }
}

void CookAssetsStep::CacheData::Save(CookingData& data)
{
    PROFILE_CPU();
    LOG(Info, "Saving incremental build cooking cache (entries count: {0})", Entries.Count());
    auto file = FileWriteStream::Open(HeaderFilePath);
    if (file == nullptr)
        return;
    DeleteMe<FileWriteStream> deleteFile(file);

    // Serialize
    file->WriteInt32(FLAXENGINE_VERSION_BUILD);
    file->WriteInt32(Entries.Count());
    file->WriteBytes(&Settings, sizeof(Settings));
    for (auto i = Entries.Begin(); i.IsNotEnd(); ++i)
    {
        auto& e = i->Value;
        file->Write(e.ID);
        file->Write(e.TypeName);
        file->Write(e.FileModified);
        file->Write(e.FileDependencies.Count());
        for (auto& f : e.FileDependencies)
        {
            file->Write(f.First, 10);
            file->Write(f.Second);
        }
    }
    file->Write(data.Tools->SaveCache(data, this));
    file->WriteInt32(13);
}

bool CookAssetsStep::ProcessDefaultAsset(AssetCookData& options)
{
    const auto asBinaryAsset = dynamic_cast<BinaryAsset*>(options.Asset);
    if (asBinaryAsset)
    {
        // Use default cooking rule (copy data)
        if (asBinaryAsset->LoadChunks(ALL_ASSET_CHUNKS))
            return true;
        for (int32 i = 0; i < ASSET_FILE_DATA_CHUNKS; i++)
        {
            const auto chunk = asBinaryAsset->GetChunk(i);
            if (chunk)
                options.InitData.Header.Chunks[i] = chunk->Clone();
        }

        return false;
    }

    const auto asJsonAsset = dynamic_cast<JsonAssetBase*>(options.Asset);
    if (asJsonAsset)
    {
        // Use compact json
        rapidjson_flax::StringBuffer buffer;
        if (Level::IsExternalActorsSceneAsset(asJsonAsset))
        {
            Array<String> externalActorFiles;
            if (Level::SaveSceneAssetToBytes(asJsonAsset, buffer, &externalActorFiles, false))
                return true;
            for (const String& file : externalActorFiles)
                options.FileDependencies.Add(ToPair(file, FileSystem::GetFileLastEditTime(file)));
        }
        else
        {
            CompactJsonWriter writerObj(buffer);
            asJsonAsset->Save(writerObj);
        }

        // Store json data in the first chunk
        auto chunk = New<FlaxChunk>();
        chunk->Flags = FlaxChunkFlags::CompressedLZ4; // Compress json data (internal storage layer will handle it)
        chunk->Data.Copy((byte*)buffer.GetString(), (int32)buffer.GetSize());
        options.InitData.Header.Chunks[0] = chunk;

        return false;
    }

    LOG(Error, "Unknown asset type \'{0}\'", options.Asset->GetTypeName());
    return false;
}

bool CookAssetsStep::Process(CookingData& data, CacheData& cache, Asset* asset)
{
    PROFILE_CPU_ASSET(asset);
    if (asset->IsVirtual())
    {
        // Virtual assets are not included into the build
        return false;
    }
    const bool wasLoaded = asset->IsLoaded();
    if (asset->WaitForLoaded())
    {
        LOG(Error, "Failed to load asset \'{0}\'", asset->ToString());
        return true;
    }
    if (!wasLoaded)
    {
        // HACK: give some time to resave any old assets in Asset::onLoad after it's loaded
        // This assumes that if Load Thread enters Asset::Save then it will get asset lock and hold it until asset is saved
        // So we can take the same lock to wait for save end but first we need to wait for it to get that lock
        // (in future try to handle it in a better way)
        Platform::Sleep(5);
    }
    ScopeLock lock(asset->Locker);

    // Switch based on an asset type
    const auto asBinaryAsset = dynamic_cast<BinaryAsset*>(asset);
    if (asBinaryAsset)
        return Process(data, cache, asBinaryAsset);
    const auto asJsonAsset = dynamic_cast<JsonAssetBase*>(asset);
    if (asJsonAsset)
        return Process(data, cache, asJsonAsset);

    LOG(Error, "Unknown asset type \'{0}\'", asset->GetTypeName());
    return false;
}

bool ProcessShaderBase(CookAssetsStep::AssetCookData& data, ShaderAssetBase* assetBase)
{
    auto asset = static_cast<BinaryAsset*>(data.Asset);

    // Decrypt source code
    auto sourceChunk = asset->GetChunk(SHADER_FILE_CHUNK_SOURCE);
    auto source = sourceChunk->Get<char>();
    auto sourceLength = sourceChunk->Size();
    Encryption::DecryptBytes((byte*)source, sourceLength);
    source[sourceLength - 1] = 0;
    while (sourceLength > 2 && source[sourceLength - 1] == 0)
        sourceLength--;

    // Init shader cache output stream
    // TODO: reuse MemoryWriteStream per cooking process to reduce dynamic memory allocations
    MemoryWriteStream cacheStream(32 * 1024);

    // Compile shader source
    ShaderCompilationOptions options;
    options.TargetName = StringUtils::GetFileNameWithoutExtension(asset->GetPath());
    options.TargetID = asset->GetID();
    options.Source = source;
    options.SourceLength = sourceLength;
    options.NoOptimize = data.Cache.Settings.Global.ShadersNoOptimize;
    options.GenerateDebugData = data.Cache.Settings.Global.ShadersGenerateDebugData;
    options.TreatWarningsAsErrors = false;
    options.Output = &cacheStream;
    Array<String> includes;

#define COMPILE_PROFILE(profile, cacheChunk) \
	{ \
		cacheStream.SetPosition(0); \
		options.Profile = ShaderProfile::profile; \
		options.Macros.Clear(); \
		auto& platformDefine = options.Macros.AddOne(); \
		platformDefine.Name = platformDefineName; \
		platformDefine.Definition = nullptr; \
		assetBase->InitCompilationOptions(options); \
		if (ShadersCompilation::Compile(options)) \
		{ \
			data.Data.Error(String::Format(TEXT("Failed to compile shader '{0}' (profile: {1})."), asset->ToString(), ::ToString(options.Profile))); \
			return true; \
		} \
        includes.Clear(); \
        ShadersCompilation::ExtractShaderIncludes(cacheStream.GetHandle(), cacheStream.GetPosition(), includes); \
        for (auto& include : includes) \
            data.FileDependencies.Add(ToPair(include, FileSystem::GetFileLastEditTime(include))); \
		auto chunk = New<FlaxChunk>(); \
		chunk->Data.Copy(cacheStream.GetHandle(), cacheStream.GetPosition()); \
		data.InitData.Header.Chunks[cacheChunk] = chunk; \
	}

    // Compile for a target platform
    switch (data.Data.Platform)
    {
#if PLATFORM_TOOLS_WINDOWS
    case BuildPlatform::Windows32:
    case BuildPlatform::Windows64:
    case BuildPlatform::WindowsARM64:
    {
        const char* platformDefineName = "PLATFORM_WINDOWS";
        const auto settings = WindowsPlatformSettings::Get();
        if (settings->SupportDX12)
        {
            COMPILE_PROFILE(DirectX_SM6, SHADER_FILE_CHUNK_INTERNAL_D3D_SM6_CACHE);
        }
        if (settings->SupportDX11)
        {
            COMPILE_PROFILE(DirectX_SM5, SHADER_FILE_CHUNK_INTERNAL_D3D_SM5_CACHE);
        }
        if (settings->SupportDX10)
        {
            COMPILE_PROFILE(DirectX_SM4, SHADER_FILE_CHUNK_INTERNAL_D3D_SM4_CACHE);
        }
        if (settings->SupportVulkan)
        {
            COMPILE_PROFILE(Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE);
        }
        break;
    }
#endif
#if PLATFORM_TOOLS_UWP
    case BuildPlatform::UWPx86:
    case BuildPlatform::UWPx64:
    {
        const char* platformDefineName = "PLATFORM_UWP";
        const auto settings = UWPPlatformSettings::Get();
        if (settings->SupportDX11)
        {
            COMPILE_PROFILE(DirectX_SM5, SHADER_FILE_CHUNK_INTERNAL_D3D_SM5_CACHE);
        }
        if (settings->SupportDX10)
        {
            COMPILE_PROFILE(DirectX_SM4, SHADER_FILE_CHUNK_INTERNAL_D3D_SM4_CACHE);
        }
        break;
    }
#endif
#if PLATFORM_TOOLS_LINUX
    case BuildPlatform::LinuxX64:
    {
        const char* platformDefineName = "PLATFORM_LINUX";
        const auto settings = LinuxPlatformSettings::Get();
        if (settings->SupportVulkan)
        {
            COMPILE_PROFILE(Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE);
        }
        break;
    }
#endif
#if PLATFORM_TOOLS_PS4
    case BuildPlatform::PS4:
    {
        const char* platformDefineName = "PLATFORM_PS4";
        COMPILE_PROFILE(PS4, SHADER_FILE_CHUNK_INTERNAL_GENERIC_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_XBOX_ONE
    case BuildPlatform::XboxOne:
    {
        const char* platformDefineName = "PLATFORM_XBOX_ONE";
        COMPILE_PROFILE(DirectX_SM6, SHADER_FILE_CHUNK_INTERNAL_D3D_SM6_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_XBOX_SCARLETT
    case BuildPlatform::XboxScarlett:
    {
        options.Platform = PlatformType::XboxScarlett;
        const char* platformDefineName = "PLATFORM_XBOX_SCARLETT";
        COMPILE_PROFILE(DirectX_SM6, SHADER_FILE_CHUNK_INTERNAL_D3D_SM6_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_ANDROID
    case BuildPlatform::AndroidARM64:
    {
        const char* platformDefineName = "PLATFORM_ANDROID";
        COMPILE_PROFILE(Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_SWITCH
    case BuildPlatform::Switch:
    {
        const char* platformDefineName = "PLATFORM_SWITCH";
        COMPILE_PROFILE(Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_PS5
    case BuildPlatform::PS5:
    {
        const char* platformDefineName = "PLATFORM_PS5";
        COMPILE_PROFILE(PS5, SHADER_FILE_CHUNK_INTERNAL_GENERIC_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_MAC
    case BuildPlatform::MacOSx64:
    case BuildPlatform::MacOSARM64:
    {
        const char* platformDefineName = "PLATFORM_MAC";
        COMPILE_PROFILE(Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_IOS
    case BuildPlatform::iOSARM64:
    {
        const char* platformDefineName = "PLATFORM_IOS";
        COMPILE_PROFILE(Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE);
        break;
    }
#endif
#if PLATFORM_TOOLS_WEB
    case BuildPlatform::Web:
    {
        const char* platformDefineName = "PLATFORM_WEB";
        COMPILE_PROFILE(WebGPU, SHADER_FILE_CHUNK_INTERNAL_GENERIC_CACHE);
        break;
    }
#endif
    default:
    {
        LOG(Warning, "Not implemented platform or shaders not supported.");
        return true;
    }
    }

    // Encrypt source code
    Encryption::EncryptBytes(reinterpret_cast<byte*>(source), sourceLength);

    return false;
}

bool ProcessMaterial(CookAssetsStep::AssetCookData& data)
{
    auto asset = static_cast<Material*>(data.Asset);

    // Material is loaded so it has valid source code generated from the Visject Surface.
    // Material::load performs any required upgrading and conversions.

    // Load material params and source code
    if (asset->LoadChunks(GET_CHUNK_FLAG(SHADER_FILE_CHUNK_MATERIAL_PARAMS) | GET_CHUNK_FLAG(SHADER_FILE_CHUNK_SOURCE)))
        return true;

    // Copy material params data
    const auto paramsChunk = asset->GetChunk(SHADER_FILE_CHUNK_MATERIAL_PARAMS);
    if (paramsChunk)
        data.InitData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS] = paramsChunk->Clone();

    // Compile shader for the target platform rendering devices
    return ProcessShaderBase(data, asset);
}

bool ProcessShader(CookAssetsStep::AssetCookData& data)
{
    auto asset = static_cast<Shader*>(data.Asset);

    // Load source code
    if (asset->LoadChunks(GET_CHUNK_FLAG(SHADER_FILE_CHUNK_SOURCE)))
        return true;

    // Compile shader for the target platform rendering devices
    return ProcessShaderBase(data, asset);
}

bool ProcessParticleEmitter(CookAssetsStep::AssetCookData& data)
{
    auto asset = static_cast<ParticleEmitter*>(data.Asset);

    // Particle Emitter is loaded so it has valid source code generated from the Visject Surface.
    // ParticleEmitter::load performs any required upgrading and conversions.

    // Load surface, material params and source code
    if (asset->LoadChunks(GET_CHUNK_FLAG(SHADER_FILE_CHUNK_VISJECT_SURFACE) | GET_CHUNK_FLAG(SHADER_FILE_CHUNK_MATERIAL_PARAMS) | GET_CHUNK_FLAG(SHADER_FILE_CHUNK_SOURCE)))
        return true;

    // Copy surface data
    const auto surfaceChunk = asset->GetChunk(SHADER_FILE_CHUNK_VISJECT_SURFACE);
    if (surfaceChunk)
        data.InitData.Header.Chunks[SHADER_FILE_CHUNK_VISJECT_SURFACE] = surfaceChunk->Clone();

    // Skip cooking shader if it's not using GPU particles
    const auto sourceChunk = asset->GetChunk(SHADER_FILE_CHUNK_SOURCE);
    if (sourceChunk == nullptr || asset->SimulationMode == ParticlesSimulationMode::CPU)
        return false;

    // Copy material params data
    const auto paramsChunk = asset->GetChunk(SHADER_FILE_CHUNK_MATERIAL_PARAMS);
    if (paramsChunk)
        data.InitData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS] = paramsChunk->Clone();

    // Compile shader for the target platform rendering devices
    return ProcessShaderBase(data, asset);
}

bool ProcessTextureBase(CookAssetsStep::AssetCookData& data)
{
    const auto asset = static_cast<TextureBase*>(data.Asset);
    const auto& assetHeader = asset->StreamingTexture()->GetHeader();
    const auto format = asset->Format();
    auto targetFormat = data.Data.Tools->GetTextureFormat(data.Data, asset, format);
    CHECK_RETURN(!PixelFormatExtensions::IsTypeless(targetFormat), true);
    const auto streamingSettings = StreamingSettings::Get();
    int32 mipLevelsMax = GPU_MAX_TEXTURE_MIP_LEVELS;
    if (assetHeader->TextureGroup >= 0 && assetHeader->TextureGroup < streamingSettings->TextureGroups.Count())
    {
        auto& group = streamingSettings->TextureGroups[assetHeader->TextureGroup];
        mipLevelsMax = group.MipLevelsMax;
        group.MipLevelsMaxPerPlatform.TryGet(data.Data.Tools->GetPlatform(), mipLevelsMax);
    }

    // If texture is smaller than the block size of the target format (eg. 4x4 texture using ASTC_6x6) then fallback to uncompressed
    int32 blockSize = PixelFormatExtensions::ComputeBlockSize(targetFormat);
    if (assetHeader->Width < blockSize || assetHeader->Height < blockSize || (blockSize != 1 && mipLevelsMax < 4))
        targetFormat = PixelFormatExtensions::FindUncompressedFormat(format);

    // Faster path if don't need to modify texture for the target platform
    if (format == targetFormat && assetHeader->MipLevels <= mipLevelsMax)
    {
        return CookAssetsStep::ProcessDefaultAsset(data);
    }

    // Extract texture data from the asset
    TextureData textureDataSrc;
    auto assetLock = asset->LockData();
    if (asset->GetTextureData(textureDataSrc, false))
    {
        LOG(Error, "Failed to load data from texture {0}", asset->ToString());
        return true;
    }

    TextureData* textureData = &textureDataSrc;
    TextureData textureDataTmp1;

    if (format != targetFormat)
    {
        // Convert texture data to the target format
        if (TextureTool::Convert(textureDataTmp1, *textureData, targetFormat))
        {
            LOG(Error, "Failed to convert texture {0} from format {1} to {2}", asset->ToString(), ScriptingEnum::ToString(format), ScriptingEnum::ToString(targetFormat));
            return true;
        }
        textureData = &textureDataTmp1;
    }

    if (assetHeader->MipLevels > mipLevelsMax)
    {
        // Reduce texture quality
        const int32 mipLevelsToStrip = assetHeader->MipLevels - mipLevelsMax;
        textureData->Width = Math::Max(1, textureData->Width >> mipLevelsToStrip);
        textureData->Height = Math::Max(1, textureData->Height >> mipLevelsToStrip);
        textureData->Depth = Math::Max(1, textureData->Depth >> mipLevelsToStrip);
        for (int32 arrayIndex = 0; arrayIndex < textureData->Items.Count(); arrayIndex++)
        {
            auto& item = textureData->Items[arrayIndex];
            Array<TextureMipData, FixedAllocation<GPU_MAX_TEXTURE_MIP_LEVELS>> oldMips(MoveTemp(item.Mips));
            item.Mips.Resize(mipLevelsMax);
            for (int32 mipIndex = 0; mipIndex < mipLevelsMax; mipIndex++)
            {
                auto& dstMip = item.Mips[mipIndex];
                auto& srcMip = oldMips[mipIndex + mipLevelsToStrip];
                dstMip = MoveTemp(srcMip);
            }
        }
    }

    // Adjust texture header
    data.InitData.CustomData.Allocate(sizeof(TextureHeader));
    auto& header = *(TextureHeader*)data.InitData.CustomData.Get();
    header = *assetHeader;
    header.Width = textureData->Width;
    header.Height = textureData->Height;
    header.Depth = textureData->Depth;
    header.Format = textureData->Format;
    header.MipLevels = textureData->GetMipLevels();

    // Serialize texture data into the asset chunks
    for (int32 mipIndex = 0; mipIndex < header.MipLevels; mipIndex++)
    {
        auto chunk = New<FlaxChunk>();
        data.InitData.Header.Chunks[mipIndex] = chunk;
        if (TextureTool::WriteTextureData(chunk->Data, *textureData, mipIndex))
            return true;
    }

    // Clone any custom asset chunks (eg. sprite atlas data, mips are in 0-13 chunks)
    for (int32 i = 14; i < ASSET_FILE_DATA_CHUNKS; i++)
    {
        const auto chunk = asset->GetChunk(i);
        if (chunk != nullptr && chunk->IsMissing() && chunk->ExistsInFile())
        {
            if (asset->Storage->LoadAssetChunk(chunk))
                return true;
            data.InitData.Header.Chunks[i] = chunk->Clone();
        }
    }

    return false;
}

CookAssetsStep::CookAssetsStep()
    : AssetsRegistry(1024)
    , AssetPathsMapping(256)
{
    AssetProcessors.Add(Material::TypeName, ProcessMaterial);
    AssetProcessors.Add(Shader::TypeName, ProcessShader);
    AssetProcessors.Add(ParticleEmitter::TypeName, ProcessParticleEmitter);
    AssetProcessors.Add(Texture::TypeName, ProcessTextureBase);
    AssetProcessors.Add(CubeTexture::TypeName, ProcessTextureBase);
    AssetProcessors.Add(SpriteAtlas::TypeName, ProcessTextureBase);
}

bool CookAssetsStep::Process(CookingData& data, CacheData& cache, BinaryAsset* asset)
{
    ASSERT(asset->IsLoaded() && asset->Storage != nullptr);
    FileDependenciesList fileDependencies;

    // Prepare asset data
    AssetInitData initData;
    if (asset->Storage->LoadAssetHeader(asset->GetID(), initData))
    {
        LOG(Warning, "Failed to load asset {} header from storage '{}'", asset->GetID(), asset->Storage->GetPath());
        return true;
    }
    initData.Header.UnlinkChunks();
    initData.Metadata.Release();
    for (auto& e : initData.Dependencies)
    {
        AssetInfo info;
        if (Content::GetRuntimeAssetInfo(e.First, info))
        {
            fileDependencies.Add(ToPair(info.Path, FileSystem::GetFileLastEditTime(info.Path)));
        }
    }
    initData.Dependencies.Resize(0);

    // Lock source asset chunks so they can be reused
    auto chunksLock = asset->Storage->LockSafe();

    // Process asset
    ProcessAssetFunc assetProcessor = nullptr;
    AssetProcessors.TryGet(asset->GetTypeName(), assetProcessor);
    AssetCookData options
    {
        data,
        cache,
        initData,
        asset,
        fileDependencies
    };
    if (!assetProcessor)
        assetProcessor = ProcessDefaultAsset;
    if (assetProcessor(options))
        return true;

    // Save cache
    String cachedFilePath;
    auto& entry = cache.CreateEntry(asset, cachedFilePath);
    entry.FileDependencies = MoveTemp(fileDependencies);
    const bool result = FlaxStorage::Create(cachedFilePath, initData);

    // Cleanup allocated data chunks
    initData.Header.DeleteChunks();

    if (result)
    {
        LOG(Warning, "Failed to save cooked file data.");
        return true;
    }
    return false;
}

bool CookAssetsStep::Process(CookingData& data, CacheData& cache, JsonAssetBase* asset)
{
    ASSERT(asset->IsLoaded() && asset->Data != nullptr);
    FileDependenciesList fileDependencies;

    // Create binary asset header
    AssetInitData initData;
    initData.SerializedVersion = 1;
    initData.Header.ID = asset->GetID();
    initData.Header.TypeName = asset->GetTypeName();

    // Process asset
    ProcessAssetFunc assetProcessor = nullptr;
    AssetProcessors.TryGet(asset->GetTypeName(), assetProcessor);
    AssetCookData options
    {
        data,
        cache,
        initData,
        asset,
        fileDependencies
    };
    if (!assetProcessor)
        assetProcessor = ProcessDefaultAsset;
    if (assetProcessor(options))
        return true;

    // Save cache
    String cachedFilePath;
    auto& entry = cache.CreateEntry(asset, cachedFilePath);
    entry.FileDependencies = MoveTemp(fileDependencies);
    const bool result = FlaxStorage::Create(cachedFilePath, initData);

    // Cleanup allocated data chunks
    initData.Header.DeleteChunks();

    if (result)
    {
        LOG(Warning, "Failed to save cooked file data.");
        return true;
    }
    return false;
}

/// <summary>
/// Helper utility to build a package of set of assets (using limits parameters).
/// </summary>
class PackageBuilder : public NonCopyable
{
private:
    int32 _packageIndex;
    int32 MaxAssetsPerPackage;
    int32 MaxPackageSize;
    FlaxStorage::CustomData CustomData;

    Array<FlaxFile*> files;
    Array<CookerPackagedAssetEntry*> addedEntries;
    uint64 bytesAdded;

    uint64 packagesSizeTotal;

public:
    /// <summary>
    /// Initializes a new instance of the <see cref="PackageBuilder" /> class.
    /// </summary>
    /// <param name="maxAssetsPerPackage">The maximum assets per package.</param>
    /// <param name="maxPackageSizeMB">The maximum package size in MB.</param>
    /// <param name="contentKey">The content keycode.</param>
    PackageBuilder(int32 maxAssetsPerPackage, int32 maxPackageSizeMB, int32 contentKey)
        : _packageIndex(0)
        , MaxAssetsPerPackage(maxAssetsPerPackage)
        , MaxPackageSize(maxPackageSizeMB * (1024 * 1024))
        , files(maxAssetsPerPackage)
        , addedEntries(maxAssetsPerPackage)
        , bytesAdded(0)
        , packagesSizeTotal(0)
    {
        Platform::MemoryClear(&CustomData, sizeof(CustomData));
        CustomData.ContentKey = contentKey;
    }

    /// <summary>
    /// Finalizes an instance of the <see cref="PackageBuilder"/> class.
    /// </summary>
    ~PackageBuilder()
    {
        Reset();
    }

public:
    uint64 GetPackagesSizeTotal() const
    {
        return packagesSizeTotal;
    }

    void Reset()
    {
        for (int32 i = 0; i < files.Count(); i++)
        {
            files[i]->Dispose();
            Delete(files[i]);
        }
        files.Clear();
        addedEntries.Clear();
        bytesAdded = 0;
        _packageIndex++;
    }

    bool Add(CookingData& data, CookerPackagedAssetEntry& entry, const String& sourcePath)
    {
        const uint64 size = FileSystem::GetFileSize(sourcePath);

        // Check if this will step out of the limit
        if (addedEntries.Count() + 1 > MaxAssetsPerPackage || (bytesAdded + size) > MaxPackageSize)
        {
            if (Package(data))
                return true;
        }

        // Add
        addedEntries.Add(&entry);
        bytesAdded += size;

        // Gather the asset to package it later
        auto file = New<FlaxFile>(sourcePath);
        if (file->Load())
        {
            Delete(file);
            data.Error(TEXT("Failed to load cooked asset."));
            return true;
        }
        files.Add(file);

        return false;
    }

    bool Package(CookingData& data)
    {
        // Skip if has no assets has been added
        const int32 count = addedEntries.Count();
        if (count == 0)
            return false;
        PROFILE_CPU();

        // Get assets init data and load all chunks
        Array<AssetInitData> assetsData;
        assetsData.Resize(count);
        for (int32 i = 0; i < count; i++)
        {
            if (files[i]->LoadAssetHeader(0, assetsData[i]))
            {
                data.Error(TEXT("Failed to load asset header data."));
                return true;
            }
            for (int32 j = 0; j < ASSET_FILE_DATA_CHUNKS; j++)
            {
                const auto chunk = assetsData[i].Header.Chunks[j];
                if (chunk)
                {
                    if (files[i]->LoadAssetChunk(chunk))
                    {
                        data.Error(TEXT("Failed to load asset data."));
                        return true;
                    }
                }
            }
        }

        // Create package
        // Note: FlaxStorage::Create overrides chunks locations in file so don't use files anymore (only readonly)
        const String localPath = String::Format(TEXT("Content/Data_{0}.{1}"), _packageIndex, PACKAGE_FILES_EXTENSION);
        const String path = data.DataOutputPath / localPath;
        if (FlaxStorage::Create(path, assetsData, false, &CustomData))
        {
            data.Error(TEXT("Failed to create assets package."));
            return true;
        }

        // Link storage info to all packaged assets
        for (int32 i = 0; i < count; i++)
        {
            addedEntries[i]->Info.Path = localPath;
        }

        packagesSizeTotal += FileSystem::GetFileSize(path);

        Reset();

        return false;
    }
};

bool CookAssetsStep::Perform(CookingData& data)
{
    float Step1ProgressStart = 0.1f;
    float Step1ProgressEnd = 0.6f;
    String Step1Info = TEXT("Cooking assets");
    float Step2ProgressStart = Step1ProgressEnd;
    float Step2ProgressEnd = 0.8f;
    String Step2Info = TEXT("Cooking files");
    float Step3ProgressStart = Step2ProgressStart;
    float Step3ProgressEnd = 0.9f;
    String Step3Info = TEXT("Packaging assets");

    data.StepProgress(TEXT("Loading build cache"), 0);

    // Prepare
    const auto gameSettings = GameSettings::Get();
    const auto buildSettings = BuildSettings::Get();
    const int32 contentKey = buildSettings->ContentKey == 0 ? rand() : buildSettings->ContentKey;
    AssetsRegistry.Clear();
    AssetPathsMapping.Clear();

    // Load incremental build cache
    CacheData cache;
    cache.Load(data);

    // Update build settings
#if PLATFORM_TOOLS_WINDOWS
    {
        const auto settings = WindowsPlatformSettings::Get();
        cache.Settings.Windows.SupportDX12 = settings->SupportDX12;
        cache.Settings.Windows.SupportDX11 = settings->SupportDX11;
        cache.Settings.Windows.SupportDX10 = settings->SupportDX10;
        cache.Settings.Windows.SupportVulkan = settings->SupportVulkan;
    }
#endif
#if PLATFORM_TOOLS_UWP
    {
        const auto settings = UWPPlatformSettings::Get();
        cache.Settings.UWP.SupportDX11 = settings->SupportDX11;
        cache.Settings.UWP.SupportDX10 = settings->SupportDX10;
    }
#endif
#if PLATFORM_TOOLS_LINUX
    {
        const auto settings = LinuxPlatformSettings::Get();
        cache.Settings.Linux.SupportVulkan = settings->SupportVulkan;
    }
#endif
    {
        cache.Settings.Global.ShadersNoOptimize = buildSettings->ShadersNoOptimize;
        cache.Settings.Global.ShadersGenerateDebugData = buildSettings->ShadersGenerateDebugData;
        cache.Settings.Global.StreamingSettingsAssetId = gameSettings->Streaming.IsValid() ? gameSettings->Streaming.ToRuntimeObjectGuid() : Guid::Empty;
        cache.Settings.Global.ShadersVersion = GPU_SHADER_CACHE_VERSION;
        cache.Settings.Global.MaterialGraphVersion = MATERIAL_GRAPH_VERSION;
        cache.Settings.Global.ParticleGraphVersion = PARTICLE_GPU_GRAPH_VERSION;
    }

    // Note: this step converts all the assets (even the json) into the binary files (FlaxStorage format).
    // Then files cooked files are packed into the packages.

    // Process all assets
    AssetInfo assetInfo;
    int32 subStepIndex = 0;
    const ArtifactTarget cookArtifactTarget = GetCookArtifactTarget(data);
    const AssetDatabaseSnapshot& buildDatabaseSnapshot = data.DatabaseSnapshot;
    if (buildDatabaseSnapshot.Revision == 0)
    {
        data.Error(TEXT("Asset cooking requires a frozen asset database snapshot."));
        return true;
    }
    Dictionary<AssetObjectId, const AssetRecord*> frozenRecords;
    for (const AssetRecord& record : buildDatabaseSnapshot.Records)
    {
        const AssetObjectId object(AssetGuid(record.SourceAssetID), record.LocalId);
        if (!object.IsValid() || frozenRecords.ContainsKey(object))
        {
            data.Error(TEXT("The frozen asset database contains an invalid or duplicate object identity."));
            return true;
        }
        frozenRecords.Add(object, &record);
    }
    Array<ArtifactLease> cookArtifactLeases;
    Dictionary<AssetObjectId, ArtifactKey> pinnedArtifactKeys;
    AssetReference<Asset> assetRef;
    assetRef.Unload.Bind([]
    {
        LOG(Error, "Asset got unloaded while cooking it!");
        Platform::Sleep(100);
    });
    for (auto i = data.Assets.Begin(); i.IsNotEnd(); ++i)
    {
        BUILD_STEP_CANCEL_CHECK;
        data.StepProgress(Step1Info, Math::Lerp(Step1ProgressStart, Step1ProgressEnd, static_cast<float>(subStepIndex++) / data.Assets.Count()));
        const AssetObjectId objectId = i->Item;
        const Guid assetId = objectId.ToRuntimeObjectGuid();
        const bool isBuiltin = data.BuiltinRootAssets.Contains(objectId);

        // Register asset
        auto& e = AssetsRegistry[objectId];
        e.Info.ID = assetId;
        e.Info.ObjectID = objectId;

        // Canonical sources are already target-processed artifacts. Resolve exact bytes and feed them
        // directly to the existing package writer instead of cooking a host-editor object.
        const AssetRecord* const* canonicalRecordPtr = frozenRecords.TryGet(objectId);
        const bool foundCanonical = canonicalRecordPtr != nullptr;
        AssetRecord canonicalRecord;
        if (foundCanonical)
            canonicalRecord = **canonicalRecordPtr;
        if (foundCanonical)
        {
            if (canonicalRecord.SourceKind == AssetSourceKind::Folder)
            {
                data.Error(String::Format(TEXT("Canonical folder object {0} cannot be included as a runtime asset."),
                    objectId.ToString()));
                return true;
            }
            AssetRecord currentRecord;
            if (!AssetDatabase::Get().TryGetRecord(objectId, currentRecord) || currentRecord.Status != canonicalRecord.Status ||
                !currentRecord.HasSameIdentityAndContent(canonicalRecord))
            {
                data.Error(String::Format(TEXT("Required asset object {0} changed after the build snapshot was frozen."),
                    objectId.ToString()));
                return true;
            }
            ArtifactRequest request;
            request.AssetID = assetId;
            request.Target = cookArtifactTarget;
            request.OutputKind = "runtime";
            request.Policy = ArtifactResolvePolicy::Exact;
            ResolvedArtifact artifact;
            AssetPipelineDiagnostic diagnostic;
            const bool resolveFailed = ArtifactResolver::Get().Resolve(request, artifact, diagnostic);
            if (resolveFailed || !artifact.IsExact || artifact.IsLastGood || !artifact.IsGenerated())
            {
                LOG(Error, "Failed to resolve exact canonical artifact {0} for cook target {1}: {2}", objectId.ToString(),
                    String(cookArtifactTarget.BuildKey(ArtifactTargetDimension::All).ToString()), diagnostic.Message);
                return true;
            }

            ArtifactKey pinnedArtifact;
            if (ArtifactKey::Parse(artifact.Key, pinnedArtifact))
            {
                data.Error(String::Format(TEXT("Exact canonical artifact {0} has an invalid immutable key."), assetId));
                return true;
            }
            pinnedArtifactKeys[objectId] = pinnedArtifact;

            cookArtifactLeases.Add(ArtifactLease::Acquire(artifact.StoragePath.Get()));
            String cachedFilePath;
            cache.GetFilePath(assetId, cachedFilePath);
            if (FileSystem::CopyFile(cachedFilePath, artifact.StoragePath.Get()))
            {
                LOG(Error, "Failed to copy exact canonical artifact from '{0}' to cooker cache '{1}'", artifact.StoragePath.Get(), cachedFilePath);
                return true;
            }
            auto& cacheEntry = cache.Entries[assetId];
            cacheEntry.ID = assetId;
            cacheEntry.TypeName = canonicalRecord.TypeName;
            cacheEntry.FileModified = FileSystem::GetFileLastEditTime(canonicalRecord.SourcePath.Get());
            cacheEntry.FileDependencies.Clear();
            cacheEntry.FileDependencies.Add(ToPair(String(canonicalRecord.SourcePath.Get()), cacheEntry.FileModified));
            cacheEntry.FileDependencies.Add(ToPair(String(canonicalRecord.MetaPath.Get()), FileSystem::GetFileLastEditTime(canonicalRecord.MetaPath.Get())));
            e.Info.TypeName = canonicalRecord.TypeName;
            data.Stats.CookedAssets++;
            continue;
        }

        // Check if asset is in cooking cache and was not modified since last build
        const auto cachedEntry = cache.Entries.TryGet(assetId);
        if (cachedEntry)
        {
            ASSERT(cachedEntry->ID == assetId);

            // Get actual asset info
            if (isBuiltin ? Content::GetRuntimeAssetInfo(assetId, assetInfo) : Content::GetAssetInfo(objectId, assetInfo))
            {
                // Ensure that cached entry is valid
                if (cachedEntry->TypeName == assetInfo.TypeName)
                {
                    // Check if file hasn't been modified
                    if (FileSystem::GetFileLastEditTime(assetInfo.Path) <= cachedEntry->FileModified)
                    {
                        // Check all dependant files
                        bool isValid = true;
                        for (auto& f : cachedEntry->FileDependencies)
                        {
                            if (FileSystem::GetFileLastEditTime(f.First) > f.Second)
                            {
                                isValid = false;
                                break;
                            }
                        }

                        if (isValid)
                        {
                            // Cache hit!
                            e.Info.TypeName = assetInfo.TypeName;
                            continue;
                        }
                    }
                }
                else
                {
                    // Remove invalid entry
                    cache.Entries.Remove(assetId);
                }
            }
        }

        // Load asset (and keep ref)
        assetRef = isBuiltin ? Content::LoadRuntimeObjectAsync<Asset>(assetId) : Content::LoadAssetAsync<Asset>(objectId);
        if (assetRef == nullptr)
        {
            LOG(Error, "Failed to load asset {} included in build", assetId);
            return true;
        }
        e.Info.TypeName = assetRef->GetTypeName();

        // Cook asset
        if (Process(data, cache, assetRef.Get()))
        {
            LOG(Error, "Failed to process asset {}", assetRef->ToString());
            cache.Save(data);
            return true;
        }
        data.Stats.CookedAssets++;

        // Auto save build cache after every few cooked assets (reduces next build time if cooking fails later)
        if (data.Stats.CookedAssets % 50 == 0)
        {
            cache.Save(data);
        }
    }

    // Save build cache header
    cache.Save(data);

    // Process all files
    for (auto i = data.Files.Begin(); i.IsNotEnd(); ++i)
    {
        BUILD_STEP_CANCEL_CHECK;
        data.StepProgress(Step2Info, Math::Lerp(Step2ProgressStart, Step2ProgressEnd, (float)subStepIndex++ / data.Files.Count()));
        const String& filePath = i->Item;

        // Calculate destination path
        String cookedPath = data.DataOutputPath;
        if (FileSystem::IsRelative(filePath))
            cookedPath /= filePath;
        else
            cookedPath /= String(TEXT("Content")) / StringUtils::GetFileName(filePath);

        // Copy file
        if (!FileSystem::FileExists(cookedPath) || FileSystem::GetFileLastEditTime(cookedPath) >= FileSystem::GetFileLastEditTime(filePath))
        {
            const String cookedFolder = StringUtils::GetDirectoryName(cookedPath);
            if (FileSystem::CreateDirectory(cookedFolder))
            {
                LOG(Error, "Failed to create directory '{}'", cookedFolder);
                return true;
            }
            if (FileSystem::CopyFile(cookedPath, filePath))
            {
                LOG(Error, "Failed to copy file from '{}' to '{}'", filePath, cookedPath);
                return true;
            }
        }

        // Count stats of file extension
        auto& assetStats = data.Stats.AssetStats[FileSystem::GetExtension(cookedPath)];
        assetStats.Count++;
        assetStats.ContentSize += FileSystem::GetFileSize(cookedPath);
    }

    // Create build game header
    {
        GameHeaderFlags gameFlags = GameHeaderFlags::None;
        if (!gameSettings->NoSplashScreen)
            gameFlags |= GameHeaderFlags::ShowSplashScreen;

        // Open file
        auto stream = FileWriteStream::Open(data.DataOutputPath / TEXT("Content/head"));
        if (stream == nullptr)
        {
            data.Error(TEXT("Failed to create game data file."));
            return true;
        }

        stream->WriteInt32(('x' + 'D') * 131); // think about it as '131 times xD'
        stream->WriteInt32(FLAXENGINE_VERSION_BUILD);

        Array<byte> bytes;
        bytes.Resize(808 + sizeof(Guid));
        Platform::MemoryClear(bytes.Get(), bytes.Count());
        int32 length = sizeof(Char) * gameSettings->ProductName.Length();
        Platform::MemoryCopy(bytes.Get() + 0, gameSettings->ProductName.Get(), length);
        bytes[length] = 0;
        bytes[length + 1] = 0;
        length = sizeof(Char) * gameSettings->CompanyName.Length();
        Platform::MemoryCopy(bytes.Get() + 400, gameSettings->CompanyName.Get(), length);
        bytes[length + 400] = 0;
        bytes[length + 401] = 0;
        *(int32*)(bytes.Get() + 800) = (int32)gameFlags;
        *(int32*)(bytes.Get() + 804) = contentKey;
        *(Guid*)(bytes.Get() + 808) = gameSettings->SplashScreen.IsValid() ? gameSettings->SplashScreen.ToRuntimeObjectGuid() : Guid::Empty;
        Encryption::EncryptBytes(bytes.Get(), bytes.Count());
        stream->Write(bytes);

        Delete(stream);
    }

    // Package all registered assets into packages
    {
        PackageBuilder packageBuilder(buildSettings->MaxAssetsPerPackage, buildSettings->MaxPackageSizeMB, contentKey);

        subStepIndex = 0;
        Array<AssetObjectId> packageAssetIDs;
        AssetsRegistry.GetKeys(packageAssetIDs);
        if (packageAssetIDs.Count() > 1)
            std::sort(packageAssetIDs.Get(), packageAssetIDs.Get() + packageAssetIDs.Count(), LessAssetObjectId);
        for (const AssetObjectId& objectId : packageAssetIDs)
        {
            BUILD_STEP_CANCEL_CHECK;
            data.StepProgress(Step3Info, Math::Lerp(Step3ProgressStart, Step3ProgressEnd, (float)subStepIndex++ / AssetsRegistry.Count()));
            CookerPackagedAssetEntry& registryEntry = AssetsRegistry[objectId];
            const Guid assetId = objectId.ToRuntimeObjectGuid();

            String cookedFilePath;
            cache.GetFilePath(assetId, cookedFilePath);
            if (!FileSystem::FileExists(cookedFilePath))
            {
                data.Error(String::Format(TEXT("Missing cooked file for asset {0}."), assetId));
                return true;
            }

            auto& assetStats = data.Stats.AssetStats[registryEntry.Info.TypeName];
            assetStats.Count++;
            assetStats.ContentSize += FileSystem::GetFileSize(cookedFilePath);

            if (packageBuilder.Add(data, registryEntry, cookedFilePath))
                return true;
        }
        if (packageBuilder.Package(data))
            return true;
        for (auto& e : data.Stats.AssetStats)
            e.Value.TypeName = e.Key;
        data.Stats.ContentSize += packageBuilder.GetPackagesSizeTotal();
    }

    BUILD_STEP_CANCEL_CHECK;

    data.StepProgress(TEXT("Creating runtime asset catalog"), Step3ProgressEnd);

    // Collect compatibility aliases for Content::Load(path). Only their normalized hashes enter the binary catalog.
    for (auto i = data.Assets.Begin(); i.IsNotEnd(); ++i)
    {
        const bool isBuiltin = data.BuiltinRootAssets.Contains(i->Item);
        const AssetRecord* const* frozen = frozenRecords.TryGet(i->Item);
        bool hasAssetInfo;
        if (frozen)
        {
            assetInfo = (**frozen).ToAssetInfo();
            hasAssetInfo = true;
        }
        else
        {
            hasAssetInfo = isBuiltin
                ? Content::GetRuntimeAssetInfo(i->Item.ToRuntimeObjectGuid(), assetInfo)
                : Content::GetAssetInfo(i->Item, assetInfo);
        }
        if (hasAssetInfo)
        {
            // Use local path relative to the game dir (assets cache is converting them to absolute paths because RelativePaths flag is set)
            String localPath;
            if (assetInfo.Path.StartsWith(Globals::StartupFolder))
                localPath = assetInfo.Path.Right(assetInfo.Path.Length() - Globals::StartupFolder.Length() - 1);
            else if (assetInfo.Path.StartsWith(Globals::ProjectFolder))
                localPath = assetInfo.Path.Right(assetInfo.Path.Length() - Globals::ProjectFolder.Length() - 1);
            else
                localPath = assetInfo.Path;
            AssetPathsMapping[localPath] = i->Item;
        }
    }

    BUILD_STEP_CANCEL_CHECK;

    // Freeze one object-level dependency graph from the same database revision used throughout this cook.
    for (auto i = data.Assets.Begin(); i.IsNotEnd(); ++i)
    {
        const AssetRecord* const* frozen = frozenRecords.TryGet(i->Item);
        if (!frozen)
            continue;
        AssetRecord current;
        if (!AssetDatabase::Get().TryGetRecord(i->Item, current) || current.Status != (*frozen)->Status ||
            !current.HasSameIdentityAndContent(**frozen))
        {
            data.Error(String::Format(TEXT("Required asset object {0} changed while cooking; refusing to publish a mixed-state catalog."),
                i->Item.ToString()));
            return true;
        }
    }
    Dictionary<Guid, AssetObjectId> objectsByRuntimeID;
    Array<RuntimeObjectDependencyRecord> dependencyRecords;
    dependencyRecords.EnsureCapacity(buildDatabaseSnapshot.Records.Count() + data.BuiltinRootAssets.Count());
    for (const AssetRecord& record : buildDatabaseSnapshot.Records)
    {
        const AssetObjectId object(AssetGuid(record.SourceAssetID), record.LocalId);
        if (!object.IsValid() || record.ID != object.ToRuntimeObjectGuid() || objectsByRuntimeID.ContainsKey(record.ID))
        {
            data.Error(TEXT("Build snapshot contains an invalid or duplicate runtime object mapping."));
            return true;
        }
        objectsByRuntimeID.Add(record.ID, object);
        RuntimeObjectDependencyRecord dependencyRecord;
        dependencyRecord.Object = object;
        dependencyRecords.Add(MoveTemp(dependencyRecord));
    }
    const int32 databaseRecordCount = buildDatabaseSnapshot.Records.Count();
    for (auto i = data.BuiltinRootAssets.Begin(); i.IsNotEnd(); ++i)
    {
        const Guid runtimeID = i->Item.ToRuntimeObjectGuid();
        const AssetObjectId* existing = objectsByRuntimeID.TryGet(runtimeID);
        if (existing && *existing != i->Item)
        {
            data.Error(TEXT("An engine built-in root collides with a project asset object."));
            return true;
        }
        if (existing)
            continue;
        objectsByRuntimeID.Add(runtimeID, i->Item);
        RuntimeObjectDependencyRecord dependencyRecord;
        dependencyRecord.Object = i->Item;
        dependencyRecords.Add(MoveTemp(dependencyRecord));
    }
    for (int32 recordIndex = 0; recordIndex < databaseRecordCount; recordIndex++)
    {
        const AssetRecord& record = buildDatabaseSnapshot.Records[recordIndex];
        HashSet<AssetObjectId> uniqueDependencies;
        for (const AssetObjectId& runtimeReference : record.RuntimeReferences)
        {
            if (!frozenRecords.ContainsKey(runtimeReference) && !data.BuiltinRootAssets.Contains(runtimeReference))
            {
                data.Error(String::Format(TEXT("Runtime dependency {0} from {1} is absent from the frozen build snapshot."),
                    runtimeReference, dependencyRecords[recordIndex].Object.ToString()));
                return true;
            }
            if (runtimeReference == dependencyRecords[recordIndex].Object || !uniqueDependencies.Add(runtimeReference))
            {
                data.Error(String::Format(TEXT("Runtime dependency graph for {0} contains a self or duplicate reference."), record.ID));
                return true;
            }
            dependencyRecords[recordIndex].Dependencies.Add(runtimeReference);
        }
    }

    Array<AssetObjectId> closureRoots;
    closureRoots.EnsureCapacity(data.RootAssets.Count());
    for (auto i = data.RootAssets.Begin(); i.IsNotEnd(); ++i)
        closureRoots.Add(i->Item);
    RuntimeDependencyClosureResult closure;
    AssetPipelineDiagnostic diagnostic;
    if (RuntimeDependencyClosure::Build(closureRoots, dependencyRecords, closure, diagnostic))
    {
        data.Error(String::Format(TEXT("Failed to build runtime dependency closure. {0}"), diagnostic.Message));
        return true;
    }
    if (closure.Objects.Count() != data.Assets.Count() || closure.Objects.Count() != AssetsRegistry.Count())
    {
        data.Error(TEXT("The cooked object set differs from the frozen dependency closure."));
        return true;
    }
    for (const AssetObjectId& object : closure.Objects)
    {
        if (!data.Assets.Contains(object) || !AssetsRegistry.ContainsKey(object))
        {
            data.Error(String::Format(TEXT("Exact runtime object {0} was not cooked."), object.ToString()));
            return true;
        }
    }

    AssetBuildSnapshot buildSnapshot;
    buildSnapshot.DatabaseRevision = buildDatabaseSnapshot.Revision;
    buildSnapshot.Target = cookArtifactTarget;
    buildSnapshot.TargetHash = cookArtifactTarget.BuildKey(ArtifactTargetDimension::All).Digest;
    buildSnapshot.ProjectSettingsHash = BuildProjectSettingsHash(*buildSettings, *gameSettings, contentKey);
    buildSnapshot.RootObjects = closureRoots;
    Array<RuntimeAssetCatalogEntry> catalogEntries;
    catalogEntries.EnsureCapacity(closure.Objects.Count());
    for (const AssetObjectId& object : closure.Objects)
    {
        const Guid runtimeID = object.ToRuntimeObjectGuid();
        const CookerPackagedAssetEntry* packaged = AssetsRegistry.TryGet(object);
        if (!packaged)
        {
            data.Error(String::Format(TEXT("Runtime dependency {0} has no packaged object."), object.ToString()));
            return true;
        }
        String cookedFilePath;
        cache.GetFilePath(runtimeID, cookedFilePath);
        ContentHash contentHash;
        if (HashCookedFile(cookedFilePath, contentHash))
        {
            data.Error(String::Format(TEXT("Failed to hash cooked object {0}."), object.ToString()));
            return true;
        }

        String packageName = packaged->Info.Path;
        packageName.Replace('\\', '/');
        if (packageName.StartsWith(TEXT("Content/")))
            packageName = packageName.Right(packageName.Length() - 8);
        RuntimeAssetCatalogEntry catalogEntry;
        catalogEntry.Object = object;
        catalogEntry.TypeName = StringAnsi(packaged->Info.TypeName);
        catalogEntry.PackageName = StringAnsi(packageName);
        catalogEntry.Offset = 0;
        catalogEntry.Size = FileSystem::GetFileSize(cookedFilePath);
        catalogEntry.Compression = RuntimeAssetCompression::None;
        catalogEntry.Content = contentHash;
        for (const RuntimeObjectDependencyRecord& dependencyRecord : dependencyRecords)
        {
            if (dependencyRecord.Object == object)
            {
                catalogEntry.Dependencies = dependencyRecord.Dependencies;
                break;
            }
        }
        catalogEntries.Add(MoveTemp(catalogEntry));

        AssetBuildSnapshotArtifact snapshotArtifact;
        snapshotArtifact.Object = object;
        const ArtifactKey* pinnedArtifact = pinnedArtifactKeys.TryGet(object);
        if (frozenRecords.ContainsKey(object) && !pinnedArtifact)
        {
            data.Error(String::Format(TEXT("Canonical runtime object {0} has no pinned exact target artifact."), object.ToString()));
            return true;
        }
        snapshotArtifact.Manifest = pinnedArtifact ? *pinnedArtifact : ArtifactKey(contentHash);
        snapshotArtifact.ObjectContent = contentHash;
        buildSnapshot.Artifacts.Add(MoveTemp(snapshotArtifact));
    }

    Array<RuntimeAssetCatalogAlias> catalogAliases;
    Dictionary<ContentHash, AssetObjectId> aliasesByHash;
    for (auto i = AssetPathsMapping.Begin(); i.IsNotEnd(); ++i)
    {
        if (!FileSystem::IsRelative(i->Key))
            continue;
        ContentHash pathHash;
        if (!i->Value.IsValid() || RuntimeAssetCatalog::HashPathAlias(i->Key, pathHash))
            continue;
        const AssetObjectId* existing = aliasesByHash.TryGet(pathHash);
        if (existing && *existing != i->Value)
        {
            data.Error(TEXT("Two runtime asset paths collide after portable normalization."));
            return true;
        }
        if (!existing)
        {
            aliasesByHash.Add(pathHash, i->Value);
            RuntimeAssetCatalogAlias alias;
            alias.PathHash = pathHash;
            alias.Object = i->Value;
            catalogAliases.Add(MoveTemp(alias));
        }
    }

    ArtifactKey snapshotFingerprint;
    if (buildSnapshot.ComputeFingerprint(snapshotFingerprint, diagnostic))
    {
        data.Error(String::Format(TEXT("Failed to finalize asset build snapshot. {0}"), diagnostic.Message));
        return true;
    }
    RuntimeAssetCatalog runtimeCatalog;
    const String runtimeCatalogPath = data.DataOutputPath / TEXT("Content/RuntimeAssetCatalog.bin");
    const AssetObjectId gameSettingsObject = GameSettings::GetGameSettingsObjectId();
    if (!gameSettingsObject.IsValid())
    {
        data.Error(TEXT("Cannot create runtime catalog without an exact GameSettings bootstrap object."));
        return true;
    }
    if (runtimeCatalog.Set(snapshotFingerprint.ToString(), buildSnapshot.TargetHash, catalogEntries, catalogAliases, diagnostic))
    {
        data.Error(String::Format(TEXT("Failed to create binary runtime asset catalog. {0}"), diagnostic.Message));
        return true;
    }
    runtimeCatalog.SetGameSettingsObject(gameSettingsObject);
    if (runtimeCatalog.SaveAtomic(runtimeCatalogPath, diagnostic))
    {
        data.Error(String::Format(TEXT("Failed to create binary runtime asset catalog. {0}"), diagnostic.Message));
        return true;
    }
    data.Stats.ContentSize += FileSystem::GetFileSize(runtimeCatalogPath);
    // Print stats
    LOG(Info, "Cooked {0} assets, total assets: {1}, total content packages size: {2} MB", data.Stats.CookedAssets, AssetsRegistry.Count(), (int32)(data.Stats.ContentSize / (1024 * 1024)));
    {
        Array<CookingData::AssetTypeStatistics> assetTypes;
        data.Stats.AssetStats.GetValues(assetTypes);
        Sorting::QuickSort(assetTypes);

        LOG(Info, "");
        LOG(Info, "Top assets types stats:");
        for (int32 i = 0; i < 10 && i < assetTypes.Count(); i++)
        {
            auto& e = assetTypes[i];
            String typeName;
            const int32 MinLength = 35;
            const int32 lengthDiff = MinLength - e.TypeName.Length();
            if (lengthDiff > 0)
            {
                typeName.ReserveSpace(MinLength);
                for (int32 j = 0; j < e.TypeName.Length(); j++)
                    typeName[j] = e.TypeName[j];
                for (int32 j = 0; j < lengthDiff; j++)
                    typeName[j + e.TypeName.Length()] = ' ';
            }
            else
            {
                typeName = e.TypeName;
            }
            if (e.Count == 1)
                LOG(Info, "{0}:    1 asset  of total size {1}", typeName, Utilities::BytesToText(e.ContentSize));
            else
                LOG(Info, "{0}: {1:>4} assets of total size {2}", typeName, e.Count, Utilities::BytesToText(e.ContentSize));
        }
        LOG(Info, "");
    }

    return false;
}
