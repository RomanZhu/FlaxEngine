// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodBankRegistry.h"

#if AUDIO_EVENT_API_FMOD

#include "FmodConvert.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"

void FmodBankRegistry::Init(FMOD::Studio::System* system)
{
    _system = system;
    _banksByGuid.Clear();
    _guidByPath.Clear();
}

void FmodBankRegistry::Dispose()
{
    UnloadAll();
    _system = nullptr;
}

bool FmodBankRegistry::Load(const Guid& bankId, const StringView& path, bool nonBlocking)
{
    if (!_system || (!bankId.IsValid() && path.IsEmpty()))
        return false;

    String filePath(path);
    if (filePath.HasChars() && FileSystem::IsRelative(filePath))
        filePath = FileSystem::ConvertRelativePathToAbsolute(Globals::ProjectFolder, filePath);

    // Check if already loaded
    if (bankId.IsValid())
    {
        BankEntry* existing = _banksByGuid.TryGet(bankId);
        if (existing)
        {
            existing->RefCount++;
            return true;
        }
    }

    if (filePath.HasChars())
    {
        Guid* existingGuid = _guidByPath.TryGet(filePath);
        if (existingGuid)
        {
            BankEntry* existing = _banksByGuid.TryGet(*existingGuid);
            if (existing)
            {
                existing->RefCount++;
                return true;
            }
        }
    }

    FMOD_STUDIO_LOAD_BANK_FLAGS flags = FMOD_STUDIO_LOAD_BANK_NORMAL;
    if (nonBlocking)
        flags = FMOD_STUDIO_LOAD_BANK_NONBLOCKING;

    StringAnsi pathAnsi(filePath);
    FMOD::Studio::Bank* bank = nullptr;
    FMOD_RESULT result = _system->loadBankFile(pathAnsi.Get(), flags, &bank);
    if (!FmodConvert::CheckResult(result, "loadBankFile") || !bank)
        return false;

    BankEntry entry;
    entry.Bank = bank;
    entry.Path = filePath;
    entry.RefCount = 1;
    entry.State = nonBlocking ? AudioBankState::Loading : AudioBankState::Loaded;

    Guid resolvedGuid = bankId;
    if (!resolvedGuid.IsValid())
    {
        FMOD_GUID fmodGuid;
        if (bank->getID(&fmodGuid) == FMOD_OK)
            resolvedGuid = FmodConvert::FromFmodGuid(fmodGuid);
        else
            resolvedGuid = Guid::New();
    }

    _banksByGuid[resolvedGuid] = entry;
    if (filePath.HasChars())
        _guidByPath[filePath] = resolvedGuid;

    return true;
}

bool FmodBankRegistry::Unload(const Guid& bankId, const StringView& path)
{
    if (!_system || (!bankId.IsValid() && path.IsEmpty()))
        return false;

    Guid resolvedGuid = bankId;
    BankEntry* entry = resolvedGuid.IsValid() ? _banksByGuid.TryGet(resolvedGuid) : nullptr;
    if (!entry && path.HasChars())
    {
        String filePath(path);
        if (FileSystem::IsRelative(filePath))
            filePath = FileSystem::ConvertRelativePathToAbsolute(Globals::ProjectFolder, filePath);
        const Guid* pathGuid = _guidByPath.TryGet(filePath);
        if (pathGuid)
        {
            resolvedGuid = *pathGuid;
            entry = _banksByGuid.TryGet(resolvedGuid);
        }
    }
    if (!entry)
        return false;

    entry->RefCount--;
    if (entry->RefCount <= 0)
    {
        if (entry->Bank)
        {
            entry->Bank->unload();
        }
        if (entry->Path.HasChars())
            _guidByPath.Remove(entry->Path);
        _banksByGuid.Remove(resolvedGuid);
    }
    return true;
}

bool FmodBankRegistry::UnloadAll()
{
    if (_system)
    {
        for (auto& it : _banksByGuid)
        {
            if (it.Value.Bank)
                it.Value.Bank->unload();
        }
    }
    _banksByGuid.Clear();
    _guidByPath.Clear();
    return true;
}

bool FmodBankRegistry::IsLoaded(const Guid& bankId) const
{
    const BankEntry* entry = _banksByGuid.TryGet(bankId);
    if (!entry || !entry->Bank)
        return false;

    FMOD_STUDIO_LOADING_STATE state;
    if (entry->Bank->getLoadingState(&state) == FMOD_OK)
        return state == FMOD_STUDIO_LOADING_STATE_LOADED;

    return false;
}

bool FmodBankRegistry::LoadSampleData(const Guid& bankId)
{
    BankEntry* entry = _banksByGuid.TryGet(bankId);
    if (!entry || !entry->Bank)
        return false;

    const FMOD_RESULT result = entry->Bank->loadSampleData();
    entry->SampleDataLoaded = result == FMOD_OK;
    return entry->SampleDataLoaded;
}

void FmodBankRegistry::UnloadSampleData(const Guid& bankId)
{
    BankEntry* entry = _banksByGuid.TryGet(bankId);
    if (entry && entry->Bank)
    {
        entry->Bank->unloadSampleData();
        entry->SampleDataLoaded = false;
    }
}

AudioBankState FmodBankRegistry::GetState(const Guid& bankId) const
{
    const BankEntry* entry = _banksByGuid.TryGet(bankId);
    if (!entry || !entry->Bank)
        return AudioBankState::Unloaded;

    FMOD_STUDIO_LOADING_STATE state;
    if (entry->Bank->getLoadingState(&state) != FMOD_OK)
        return AudioBankState::Error;
    if (state == FMOD_STUDIO_LOADING_STATE_LOADED)
        return AudioBankState::Loaded;
    if (state == FMOD_STUDIO_LOADING_STATE_LOADING)
        return AudioBankState::Loading;
    return AudioBankState::Error;
}

FMOD::Studio::Bank* FmodBankRegistry::Get(const Guid& bankId) const
{
    const BankEntry* entry = _banksByGuid.TryGet(bankId);
    return entry ? entry->Bank : nullptr;
}

#endif
