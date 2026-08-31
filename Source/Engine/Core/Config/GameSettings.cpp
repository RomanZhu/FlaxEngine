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
#include "Engine/Scripting/Internal/InternalCalls.h"
#if USE_EDITOR
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Editor/Editor.h"
#include "Editor/ProjectInfo.h"
#endif

class GameSettingsService : public EngineService
{
public:
    GameSettingsService()
        : EngineService(TEXT("GameSettings"), -500)
    {
    }

    bool Init() override
    {
#if USE_EDITOR
        if (!Editor::Project || Editor::Project->AssetSystemVersion != 2)
        {
            LOG(Error, "This project uses the legacy Flax asset system and cannot be opened by this engine build. This branch requires source assets and version-2 metadata using GUID plus local-file-ID references.");
            return true;
        }
        if (!Editor::Project->ProjectSettingsIndexGuid.IsValid())
        {
            LOG(Error, "Project descriptor is missing a valid ProjectSettingsIndexGuid.");
            return true;
        }
        if (AssetPipelineService::LoadOrScan(true))
        {
            const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
            for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
            {
                LOG(Error, "[{0}] {1} Source: '{2}'.", GetAssetPipelineDiagnosticCodeName(diagnostic.Code),
                    diagnostic.Message, diagnostic.SourcePath);
            }
            return true;
        }
#endif
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

AssetObjectId GameSettings::GetGameSettingsObjectId()
{
#if USE_EDITOR
    if (!Editor::Project || !Editor::Project->ProjectSettingsIndexGuid.IsValid())
        return AssetObjectId();
    const AssetObjectId indexObject = AssetObjectId::Main(AssetGuid(Editor::Project->ProjectSettingsIndexGuid));
    const AssetReference<JsonAsset> indexAsset = Content::LoadAssetAsync<JsonAsset>(indexObject);
    if (!indexAsset || indexAsset->WaitForLoaded() || !indexAsset->Data || !indexAsset->Data->IsObject())
    {
        LOG(Error, "Failed to load project settings index object {0} through the asset pipeline.", indexObject);
        return AssetObjectId();
    }
    const auto gameSettings = indexAsset->Data->FindMember("GameSettings");
    AssetObjectId result;
    if (gameSettings != indexAsset->Data->MemberEnd())
        Serialization::Deserialize(gameSettings->value, result, nullptr);
    if (!result.IsValid())
        LOG(Error, "Project settings index does not contain a valid GameSettings object reference.");
    return result;
#else
    return Content::GetRuntimeGameSettingsObject();
#endif
}

DEFINE_INTERNAL_CALL(void) GameSettingsInternal_GetGameSettingsObjectId(AssetObjectId* result)
{
    *result = GameSettings::GetGameSettingsObjectId();
}

GameSettings* GameSettings::Get()
{
    if (!GameSettingsAsset)
    {
        // Load root game settings asset.
        // It may be missing in editor during dev but must be ready in the build game.
        PROFILE_CPU();
#if FLAX_TESTS
        // Silence missing GameSettings during test run before Editor creates it (not important)
        return nullptr;
#endif
        const AssetObjectId gameSettingsObject = GetGameSettingsObjectId();
        if (!gameSettingsObject.IsValid())
        {
            LOG(Error, "Runtime catalog is missing the GameSettings bootstrap object.");
            return nullptr;
        }
        GameSettingsAsset = Content::LoadAssetAsync<JsonAsset>(gameSettingsObject);
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
        if (settings->type.IsValid()) \
        { \
            Content::LoadAssetAsync<JsonAsset>(settings->type); \
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
    if (settings->AssetPipeline.IsValid())
        Content::LoadAssetAsync<JsonAsset>(settings->AssetPipeline);

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
    // Load properties
    ProductName = JsonTools::GetString(stream, "ProductName");
    CompanyName = JsonTools::GetString(stream, "CompanyName");
    CopyrightNotice = JsonTools::GetString(stream, "CopyrightNotice");
    Version = JsonTools::GetString(stream, "Version");
    DESERIALIZE(Icon);
    const auto firstScene = stream.FindMember("FirstScene");
    if (firstScene != stream.MemberEnd())
        Serialization::Deserialize(firstScene->value, FirstScene, modifier);
    NoSplashScreen = JsonTools::GetBool(stream, "NoSplashScreen", NoSplashScreen);
    DESERIALIZE(SplashScreen);
    CustomSettings.Clear();
    const auto customSettings = stream.FindMember("CustomSettings");
    if (customSettings != stream.MemberEnd() && customSettings->value.IsObject())
    {
        auto& items = customSettings->value;
        for (auto it = items.MemberBegin(); it != items.MemberEnd(); ++it)
        {
            AssetObjectId value;
            Serialization::Deserialize(it->value, value, modifier);
            if (value.IsValid())
                CustomSettings[it->name.GetText()] = value;
        }
    }

    // Settings containers
    DESERIALIZE(Time);
    DESERIALIZE(Audio);
    DESERIALIZE(LayersAndTags);
    DESERIALIZE(Physics);
    DESERIALIZE(Input);
    DESERIALIZE(Graphics);
    DESERIALIZE(Network);
    DESERIALIZE(Navigation);
    DESERIALIZE(Localization);
    DESERIALIZE(GameCooking);
    DESERIALIZE(Streaming);
    DESERIALIZE(AssetPipeline);

    // Per-platform settings containers
    DESERIALIZE(WindowsPlatform);
    DESERIALIZE(UWPPlatform);
    DESERIALIZE(LinuxPlatform);
    DESERIALIZE(PS4Platform);
    DESERIALIZE(XboxOnePlatform);
    DESERIALIZE(XboxScarlettPlatform);
    DESERIALIZE(AndroidPlatform);
    DESERIALIZE(SwitchPlatform);
    DESERIALIZE(PS5Platform);
    DESERIALIZE(MacPlatform);
    DESERIALIZE(iOSPlatform);
    DESERIALIZE(WebPlatform);
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
