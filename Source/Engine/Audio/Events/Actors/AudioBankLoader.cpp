// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioBankLoader.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
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

            // AudioBank.Path may be a middleware object path (for example bank:/Master),
            // not a filesystem bank filename. Use explicit BankPaths for file loading.
            if (path.StartsWith(TEXT("bank:/")))
                path.Clear();
            if (path.HasChars())
            {
                AudioEventSystem::LoadBank(bankId, path, nonBlocking);
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
                path.Clear();
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
}

void AudioBankLoader::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif

    if (UnloadOnDisable)
        UnloadBanks();

    Actor::OnDisable();
}

void AudioBankLoader::BeginPlay(SceneBeginData* data)
{
    Actor::BeginPlay(data);

    if (LoadOnStart && IsDuringPlay())
        LoadBanks();
}

void AudioBankLoader::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioBankLoader);

    SERIALIZE(Banks);
    SERIALIZE(BankPaths);
    SERIALIZE(LoadOnStart);
    SERIALIZE(UnloadOnDisable);
}

void AudioBankLoader::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);

    DESERIALIZE(Banks);
    DESERIALIZE(BankPaths);
    DESERIALIZE(LoadOnStart);
    DESERIALIZE(UnloadOnDisable);
}
