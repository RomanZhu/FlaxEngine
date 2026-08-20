// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include <fmod_studio.hpp>

/// <summary>
/// Registry managing loaded FMOD Studio banks with ref-counting and lookup.
/// </summary>
class FmodBankRegistry
{
public:
    struct BankEntry
    {
        FMOD::Studio::Bank* Bank = nullptr;
        String Path;
        int32 RefCount = 0;
        bool SampleDataLoaded = false;
        AudioBankState State = AudioBankState::Unloaded;
    };

private:
    FMOD::Studio::System* _system = nullptr;
    Dictionary<Guid, BankEntry> _banksByGuid;
    Dictionary<String, Guid> _guidByPath;

public:
    FmodBankRegistry() = default;
    ~FmodBankRegistry() = default;

    void Init(FMOD::Studio::System* system);
    void Dispose();

    bool Load(const Guid& bankId, const StringView& path, bool nonBlocking);
    bool Unload(const Guid& bankId, const StringView& path = StringView::Empty);
    bool UnloadAll();
    bool IsLoaded(const Guid& bankId) const;
    bool LoadSampleData(const Guid& bankId);
    void UnloadSampleData(const Guid& bankId);
    AudioBankState GetState(const Guid& bankId) const;

    FMOD::Studio::Bank* Get(const Guid& bankId) const;
    int32 GetLoadedCount() const { return _banksByGuid.Count(); }
};

#endif
