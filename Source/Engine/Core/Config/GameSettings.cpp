// Copyright (c) Wojciech Figat. All rights reserved.

#include "GameSettings.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Physics/PhysicsSettings.h"
#include "Engine/Core/Log.h"
#include "LayersTagsSettings.h"
#include "TimeSettings.h"
#include "PlatformSettings.h"
#include "GraphicsSettings.h"
#include "BuildSettings.h"
#include "Engine/Input/InputSettings.h"
#include "Engine/Audio/AudioSettings.h"
#include "Engine/Networking/NetworkSettings.h"
#include "Engine/Navigation/NavigationSettings.h"
#include "Engine/Localization/LocalizationSettings.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/AssetPipeline/AssetPipelineSettings.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Streaming/StreamingSettings.h"
#include "Engine/Serialization/Serialization.h"
#if FLAX_TESTS || USE_EDITOR
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/File.h"
#endif

namespace
{
    Guid ReadAssetReference(const rapidjson_flax::Value& value)
    {
        if (!value.IsObject())
            return Guid::Empty;
        const auto guid = value.FindMember("guid");
        const auto localId = value.FindMember("localId");
        if (guid == value.MemberEnd() || localId == value.MemberEnd() || !localId->value.IsInt64() || localId->value.GetInt64() != 1)
            return Guid::Empty;
        return JsonTools::GetGuid(guid->value);
    }

    void ReadAssetReference(const rapidjson_flax::Value& stream, const char* name, Guid& result)
    {
        const auto value = stream.FindMember(name);
        result = value != stream.MemberEnd() ? ReadAssetReference(value->value) : Guid::Empty;
    }

    String GetGameSettingsAssetPath()
    {
#if USE_EDITOR
        Array<String> projects;
        if (!FileSystem::DirectoryGetFiles(projects, Globals::ProjectFolder, TEXT("*.flaxproj"), DirectorySearchOption::TopDirectoryOnly) && projects.Count() == 1)
        {
            StringAnsi source;
            rapidjson_flax::Document document;
            if (!File::ReadAllText(projects[0], source))
            {
                document.Parse(source.Get(), source.Length());
                if (!document.HasParseError() && JsonTools::GetInt(document, "AssetSystemVersion", 0) == 3)
                    return Globals::ProjectContentFolder / TEXT("Settings/Project Settings.json");
            }
        }
#endif
        return Globals::ProjectContentFolder / TEXT("GameSettings.json");
    }
}

class GameSettingsService : public EngineService
{
public:
    GameSettingsService()
        : EngineService(TEXT("GameSettings"), -70)
    {
    }

    bool Init() override
    {
        return GameSettings::Load();
    }
};

IMPLEMENT_ENGINE_SETTINGS_GETTER(BuildSettings, GameCooking);

#include "Engine/Content/Deprecated.h"
void GraphicsSettings::SetUeeHDRProbes(bool value)
{
    MARK_CONTENT_DEPRECATED();
    UseHDRProbes = value;
}

void GraphicsSettings::OnDeserializing(const CallbackContext& context)
{
#if 0 // TODO: move to Linear color space as default once it's ready for production
    // [Deprecated on 9.01.2026, expires on 9.01.2028]
    if (context.Modifier && context.Modifier->EngineBuild < 6901)
    {
        // Old projects were made in Gamma color space
        GammaColorSpace = true;
        MARK_CONTENT_DEPRECATED();
    }
#endif
}

IMPLEMENT_ENGINE_SETTINGS_GETTER(GraphicsSettings, Graphics);
IMPLEMENT_ENGINE_SETTINGS_GETTER(NetworkSettings, Network);
IMPLEMENT_ENGINE_SETTINGS_GETTER(LayersAndTagsSettings, LayersAndTags);
IMPLEMENT_ENGINE_SETTINGS_GETTER(TimeSettings, Time);
IMPLEMENT_ENGINE_SETTINGS_GETTER(AudioSettings, Audio);
IMPLEMENT_ENGINE_SETTINGS_GETTER(PhysicsSettings, Physics);
IMPLEMENT_ENGINE_SETTINGS_GETTER(InputSettings, Input);
IMPLEMENT_ENGINE_SETTINGS_GETTER(StreamingSettings, Streaming);
IMPLEMENT_ENGINE_SETTINGS_GETTER(AssetPipelineSettings, AssetPipeline);

#if !USE_EDITOR
#if PLATFORM_WINDOWS
IMPLEMENT_ENGINE_SETTINGS_GETTER(WindowsPlatformSettings, WindowsPlatform);
#elif PLATFORM_UWP
IMPLEMENT_ENGINE_SETTINGS_GETTER(UWPPlatformSettings, UWPPlatform);
#elif PLATFORM_LINUX
IMPLEMENT_ENGINE_SETTINGS_GETTER(LinuxPlatformSettings, LinuxPlatform);
#elif PLATFORM_PS4
IMPLEMENT_ENGINE_SETTINGS_GETTER(PS4PlatformSettings, PS4Platform);
#elif PLATFORM_PS5
IMPLEMENT_ENGINE_SETTINGS_GETTER(PS5PlatformSettings, PS5Platform);
#elif PLATFORM_XBOX_ONE
IMPLEMENT_ENGINE_SETTINGS_GETTER(XboxOnePlatformSettings, XboxOnePlatform);
#elif PLATFORM_XBOX_SCARLETT
IMPLEMENT_ENGINE_SETTINGS_GETTER(XboxScarlettPlatformSettings, XboxScarlettPlatform);
#elif PLATFORM_ANDROID
IMPLEMENT_ENGINE_SETTINGS_GETTER(AndroidPlatformSettings, AndroidPlatform);
#elif PLATFORM_SWITCH
IMPLEMENT_ENGINE_SETTINGS_GETTER(SwitchPlatformSettings, SwitchPlatform);
#elif PLATFORM_MAC
IMPLEMENT_ENGINE_SETTINGS_GETTER(MacPlatformSettings, MacPlatform);
#elif PLATFORM_IOS
IMPLEMENT_ENGINE_SETTINGS_GETTER(iOSPlatformSettings, iOSPlatform);
#elif PLATFORM_WEB
IMPLEMENT_ENGINE_SETTINGS_GETTER(WebPlatformSettings, WebPlatform);
#else
#error Unknown platform
#endif
#endif

GameSettingsService GameSettingsServiceInstance;
AssetReference<JsonAsset> GameSettingsAsset;

GameSettings* GameSettings::Get()
{
    if (!GameSettingsAsset)
    {
        // Load root game settings asset.
        // It may be missing in editor during dev but must be ready in the build game.
        PROFILE_CPU();
        const auto assetPath = GetGameSettingsAssetPath();
#if FLAX_TESTS
        // Silence missing GameSettings during test run before Editor creates it (not important)
        if (!FileSystem::FileExists(assetPath))
            return nullptr;
#endif
#if USE_EDITOR
        // Log once missing GameSettings in Editor
        if (!FileSystem::FileExists(assetPath))
        {
            static bool LogOnce = true;
            if (LogOnce)
            {
                LogOnce = false;
                LOG(Error, "Missing file game settings asset ({0})", assetPath);
            }
            return nullptr;
        }
#endif
        GameSettingsAsset = Content::LoadAsync<JsonAsset>(assetPath);
        if (GameSettingsAsset == nullptr)
        {
            LOG(Error, "Missing game settings asset.");
            return nullptr;
        }
        if (GameSettingsAsset->WaitForLoaded())
        {
            return nullptr;
        }
        if (GameSettingsAsset->InstanceType != GameSettings::TypeInitializer)
        {
            LOG(Error, "Invalid game settings asset data type.");
            return nullptr;
        }
    }
    auto asset = GameSettingsAsset.Get();
    if (asset && asset->WaitForLoaded())
        asset = nullptr;
    return asset ? (GameSettings*)asset->Instance : nullptr;
}

bool GameSettings::Load()
{
    PROFILE_CPU();

    // Load main settings asset
    auto settings = Get();
    if (!settings)
    {
#if USE_EDITOR
        // Allow lack of Game Settings in Editor
        return false;
#else
        return true;
#endif
    }

    // Preload all settings assets
#define PRELOAD_SETTINGS(type) \
    { \
        if (settings->type) \
        { \
            Content::LoadAsync<JsonAsset>(settings->type); \
        } \
        else \
        { \
            LOG(Warning, "Missing {0} settings", TEXT(#type)); \
        } \
    }
    PRELOAD_SETTINGS(Time);
    PRELOAD_SETTINGS(Audio);
    PRELOAD_SETTINGS(LayersAndTags);
    PRELOAD_SETTINGS(Physics);
    PRELOAD_SETTINGS(Input);
    PRELOAD_SETTINGS(Graphics);
    PRELOAD_SETTINGS(Network);
    PRELOAD_SETTINGS(Navigation);
    PRELOAD_SETTINGS(Localization);
    PRELOAD_SETTINGS(GameCooking);
    PRELOAD_SETTINGS(Streaming);
#undef PRELOAD_SETTINGS
    if (settings->AssetPipeline)
        Content::LoadAsync<JsonAsset>(settings->AssetPipeline);

    // Apply the game settings to the engine
    settings->Apply();

    return false;
}

void GameSettings::Apply()
{
    PROFILE_CPU();
#define APPLY_SETTINGS(type) \
    { \
        type* obj = type::Get(); \
        if (obj) \
        { \
            obj->Apply(); \
        } \
        else \
        { \
            LOG(Warning, "Missing {0} settings", TEXT(#type)); \
        } \
    }
    APPLY_SETTINGS(TimeSettings);
    APPLY_SETTINGS(AudioSettings);
    APPLY_SETTINGS(LayersAndTagsSettings);
    APPLY_SETTINGS(PhysicsSettings);
    APPLY_SETTINGS(StreamingSettings);
    APPLY_SETTINGS(InputSettings);
    APPLY_SETTINGS(GraphicsSettings);
    APPLY_SETTINGS(NetworkSettings);
    APPLY_SETTINGS(NavigationSettings);
    APPLY_SETTINGS(LocalizationSettings);
    APPLY_SETTINGS(BuildSettings);
    APPLY_SETTINGS(PlatformSettings);
    APPLY_SETTINGS(AssetPipelineSettings);
#undef APPLY_SETTINGS
}

void GameSettings::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    (void)modifier;
    // Load properties
    ProductName = JsonTools::GetString(stream, "ProductName");
    CompanyName = JsonTools::GetString(stream, "CompanyName");
    CopyrightNotice = JsonTools::GetString(stream, "CopyrightNotice");
    Version = JsonTools::GetString(stream, "Version");
    ReadAssetReference(stream, "Icon", Icon);
    ReadAssetReference(stream, "FirstScene", FirstScene);
    NoSplashScreen = JsonTools::GetBool(stream, "NoSplashScreen", NoSplashScreen);
    ReadAssetReference(stream, "SplashScreen", SplashScreen);
    CustomSettings.Clear();
    const auto customSettings = stream.FindMember("CustomSettings");
    if (customSettings != stream.MemberEnd() && (customSettings->value.IsObject() || customSettings->value.IsArray()))
    {
        auto& items = customSettings->value;
        for (auto it = items.MemberBegin(); it != items.MemberEnd(); ++it)
        {
            const Guid value = ReadAssetReference(it->value);
            if (value.IsValid())
            {
                const String key = it->name.GetText();
                CustomSettings[key] = value;
            }
        }
    }

    // Settings containers use the canonical file GUID/local-ID representation.
#define DESERIALIZE_ASSET_REFERENCE(name) ReadAssetReference(stream, #name, name)
    DESERIALIZE_ASSET_REFERENCE(Time);
    DESERIALIZE_ASSET_REFERENCE(Audio);
    DESERIALIZE_ASSET_REFERENCE(LayersAndTags);
    DESERIALIZE_ASSET_REFERENCE(Physics);
    DESERIALIZE_ASSET_REFERENCE(Input);
    DESERIALIZE_ASSET_REFERENCE(Graphics);
    DESERIALIZE_ASSET_REFERENCE(Network);
    DESERIALIZE_ASSET_REFERENCE(Navigation);
    DESERIALIZE_ASSET_REFERENCE(Localization);
    DESERIALIZE_ASSET_REFERENCE(GameCooking);
    DESERIALIZE_ASSET_REFERENCE(Streaming);
    DESERIALIZE_ASSET_REFERENCE(AssetPipeline);

    // Per-platform settings containers
    DESERIALIZE_ASSET_REFERENCE(WindowsPlatform);
    DESERIALIZE_ASSET_REFERENCE(UWPPlatform);
    DESERIALIZE_ASSET_REFERENCE(LinuxPlatform);
    DESERIALIZE_ASSET_REFERENCE(PS4Platform);
    DESERIALIZE_ASSET_REFERENCE(XboxOnePlatform);
    DESERIALIZE_ASSET_REFERENCE(XboxScarlettPlatform);
    DESERIALIZE_ASSET_REFERENCE(AndroidPlatform);
    DESERIALIZE_ASSET_REFERENCE(SwitchPlatform);
    DESERIALIZE_ASSET_REFERENCE(PS5Platform);
    DESERIALIZE_ASSET_REFERENCE(MacPlatform);
    DESERIALIZE_ASSET_REFERENCE(iOSPlatform);
    DESERIALIZE_ASSET_REFERENCE(WebPlatform);
#undef DESERIALIZE_ASSET_REFERENCE
}

#if USE_EDITOR

void LayersAndTagsSettings::Serialize(SerializeStream& stream, const void* otherObj)
{
    SERIALIZE_GET_OTHER_OBJ(LayersAndTagsSettings);

    stream.JKEY("Tags");
    stream.StartArray();
    for (const String& e : Tags)
        stream.String(e);
    stream.EndArray();

    stream.JKEY("Layers");
    stream.StartArray();
    for (const String& e : Layers)
        stream.String(e);
    stream.EndArray();
}

#endif

void LayersAndTagsSettings::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    const auto tags = stream.FindMember("Tags");
    if (tags != stream.MemberEnd() && tags->value.IsArray())
    {
        auto& tagsArray = tags->value;
        Tags.Clear();
        Tags.EnsureCapacity(tagsArray.Size());
        for (uint32 i = 0; i < tagsArray.Size(); i++)
        {
            auto& v = tagsArray[i];
            if (v.IsString())
                Tags.Add(v.GetText());
        }
    }

    const auto layers = stream.FindMember("Layers");
    if (layers != stream.MemberEnd() && layers->value.IsArray())
    {
        auto& layersArray = layers->value;
        for (uint32 i = 0; i < layersArray.Size() && i < ARRAY_COUNT(Layers); i++)
        {
            auto& v = layersArray[i];
            if (v.IsString())
                Layers[i] = v.GetText();
            else
                Layers[i].Clear();
        }
        for (uint32 i = layersArray.Size(); i < ARRAY_COUNT(Layers); i++)
        {
            Layers[i].Clear();
        }
    }
}
