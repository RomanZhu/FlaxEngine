// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioBankLoader.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Core/Log.h"

AudioBankLoader::AudioBankLoader(const SpawnParams& params)
    : Actor(params)
{
}

void AudioBankLoader::LoadBanks()
{
#if USE_EDITOR
    if (!IsDuringPlay())
        return;
#endif

    for (int32 i = 0; i < Banks.Count(); i++)
    {
        const auto& asset = Banks[i];
        if (asset)
        {
            asset->WaitForLoaded();
            Guid bankId = Guid::Empty;
            String path;
            bool nonBlocking = false;
            const auto* bank = asset->GetInstance<AudioBank>();
            if (bank)
            {
                AudioEventCatalog::RegisterBank(bank);
                bankId = bank->BackendId;
                path = bank->Path;
                nonBlocking = bank->NonBlocking;
            }
            else if (asset->Data && asset->DataTypeName == TEXT("FlaxEngine.AudioBank"))
            {
                auto& node = *asset->Data;
                auto itBackend = node.FindMember("BackendId");
                if (itBackend != node.MemberEnd() && itBackend->value.IsString())
                    JsonTools::GetGuid(bankId, node, "BackendId");
                auto itPath = node.FindMember("Path");
                if (itPath != node.MemberEnd() && itPath->value.IsString())
                    path = itPath->value.GetString();
                auto itNonBlocking = node.FindMember("NonBlocking");
                if (itNonBlocking != node.MemberEnd() && itNonBlocking->value.IsBool())
                    nonBlocking = itNonBlocking->value.GetBool();
            }
            else
            {
                LOG(Warning, "AudioBankLoader '{0}' ignored non-AudioBank asset '{1}'.", GetName(), asset->GetPath());
            }

            // Older synchronized assets stored bank:/Name instead of a bank file.
            // Keep those projects audible while reporting the required migration.
            if (path.StartsWith(TEXT("bank:/")))
            {
                LOG(Warning, "AudioBank '{0}' uses obsolete middleware path metadata. Resynchronize it with a .bank file path.", asset->GetPath());
                path = path.Substring(6) + TEXT(".bank");
            }
            if (path.HasChars())
            {
                if (!AudioEventSystem::LoadBank(bankId, path, nonBlocking))
                    LOG(Error, "AudioBankLoader '{0}' failed to load bank '{1}'.", GetName(), path);
                else if (PreloadSampleData && bankId.IsValid() && !AudioEventSystem::LoadBankSampleData(bankId))
                    LOG(Error, "AudioBankLoader '{0}' failed to preload bank sample data '{1}'.", GetName(), path);
            }
        }
    }

    for (int32 i = 0; i < BankPaths.Count(); i++)
    {
        const auto& path = BankPaths[i];
        if (path.HasChars() && !path.StartsWith(TEXT("bank:/")))
        {
            AudioEventSystem::LoadBank(Guid::Empty, path, false);
        }
    }
}

void AudioBankLoader::UnloadBanks()
{
    for (int32 i = 0; i < Banks.Count(); i++)
    {
        const auto& asset = Banks[i];
        if (asset)
        {
            asset->WaitForLoaded();
            Guid bankId = Guid::Empty;
            String path;
            const auto* bank = asset->GetInstance<AudioBank>();
            if (bank)
            {
                bankId = bank->BackendId;
                path = bank->Path;
            }
            else if (asset->Data && asset->DataTypeName == TEXT("FlaxEngine.AudioBank"))
            {
                auto& node = *asset->Data;
                auto itBackend = node.FindMember("BackendId");
                if (itBackend != node.MemberEnd() && itBackend->value.IsString())
                    JsonTools::GetGuid(bankId, node, "BackendId");
                auto itPath = node.FindMember("Path");
                if (itPath != node.MemberEnd() && itPath->value.IsString())
                    path = itPath->value.GetString();
            }
            else
            {
                LOG(Warning, "AudioBankLoader '{0}' ignored non-AudioBank asset '{1}'.", GetName(), asset->GetPath());
            }

            if (path.StartsWith(TEXT("bank:/")))
                path = path.Substring(6) + TEXT(".bank");
            if (bankId.IsValid() || path.HasChars())
            {
                AudioEventSystem::UnloadBank(bankId, path);
            }
        }
    }

    for (int32 i = 0; i < BankPaths.Count(); i++)
    {
        const auto& path = BankPaths[i];
        if (path.HasChars() && !path.StartsWith(TEXT("bank:/")))
            AudioEventSystem::UnloadBank(Guid::Empty, path);
    }
}

bool AudioBankLoader::SignalActivation(AudioActivationEvent activationEvent, Actor* source, Actor* target)
{
    bool handled = false;
    if (_loadActivationState.TryActivate(LoadActivation, activationEvent, source, target))
    {
        LoadBanks();
        handled = true;
    }
    if (_unloadActivationState.TryActivate(UnloadActivation, activationEvent, source, target))
    {
        UnloadBanks();
        handled = true;
    }
    if (activationEvent == AudioActivationEvent::TriggerExit || activationEvent == AudioActivationEvent::CollisionExit || activationEvent == AudioActivationEvent::PointerExit)
    {
        _loadActivationState.NotifyExit(LoadActivation);
        _unloadActivationState.NotifyExit(UnloadActivation);
    }
    return handled;
}

bool AudioBankLoader::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

void AudioBankLoader::OnEnable()
{
#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif

    Actor::OnEnable();
    if (IsDuringPlay())
        SignalActivation(AudioActivationEvent::ActorEnable, this, this);
}

void AudioBankLoader::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif

    if (IsDuringPlay())
        SignalActivation(AudioActivationEvent::ActorDisable, this, this);
    if (UnloadOnDisable)
        UnloadBanks();

    Actor::OnDisable();
}

void AudioBankLoader::BeginPlay(SceneBeginData* data)
{
    Actor::BeginPlay(data);

    _loadActivationState.Reset();
    _unloadActivationState.Reset();
    if (LoadOnStart && IsDuringPlay())
        LoadBanks();
    SignalActivation(AudioActivationEvent::BeginPlay, this, this);
}

void AudioBankLoader::EndPlay()
{
    SignalActivation(AudioActivationEvent::EndPlay, this, this);
    Actor::EndPlay();
}

void AudioBankLoader::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioBankLoader);

    SERIALIZE(Banks);
    SERIALIZE(BankPaths);
    SERIALIZE(LoadOnStart);
    SERIALIZE(UnloadOnDisable);
    SERIALIZE(PreloadSampleData);
    SERIALIZE(LoadActivation);
    SERIALIZE(UnloadActivation);
}

void AudioBankLoader::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);

    DESERIALIZE(Banks);
    DESERIALIZE(BankPaths);
    DESERIALIZE(LoadOnStart);
    DESERIALIZE(UnloadOnDisable);
    DESERIALIZE(PreloadSampleData);
    DESERIALIZE(LoadActivation);
    DESERIALIZE(UnloadActivation);
}
