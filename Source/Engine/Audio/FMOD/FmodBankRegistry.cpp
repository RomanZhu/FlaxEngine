// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodBankRegistry.h"

#if AUDIO_EVENT_API_FMOD

#include "FmodConvert.h"
#include "FmodBankPathResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

void FmodBankRegistry::Init(FMOD::Studio::System* system)
{
    _system = system;
    _banksByGuid.Clear();
    _guidByPath.Clear();
    _aliases.Clear();
}

void FmodBankRegistry::Dispose()
{
    UnloadAll();
    _banksByGuid.Clear();
    _guidByPath.Clear();
    _aliases.Clear();
    _system = nullptr;
}

bool FmodBankRegistry::Load(const Guid& bankId, const StringView& path, bool nonBlocking)
{
    if (!_system || (!bankId.IsValid() && path.IsEmpty()))
        return false;

    String filePath;
    if (path.HasChars() && !FmodBankPathResolver::Resolve(path, filePath))
    {
        LOG(Error, "FMOD bank path '{0}' could not be resolved for this runtime.", path);
        return false;
    }

    // Check if already loaded
    if (bankId.IsValid())
    {
        BankEntry* existing = _banksByGuid.TryGet(ResolveBankId(bankId));
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
                if (bankId.IsValid() && *existingGuid != bankId)
                {
                    if (!existing->SyntheticKey)
                    {
                        // Managed System.Guid and native Flax Guid field order
                        // can represent the same canonical Studio ID
                        // differently. The normalized physical path is the
                        // authoritative ownership key.
                        _aliases[bankId] = *existingGuid;
                        existing->RefCount++;
                        return true;
                    }

                    // Promote a path-only load to its stable typed ID. This keeps
                    // dependency lookup and IsBankLoaded coherent when an async
                    // loader was the first owner of the physical bank.
                    BankEntry promoted = *existing;
                    promoted.RefCount++;
                    promoted.SyntheticKey = false;
                    const Guid oldKey = *existingGuid;
                    _banksByGuid.Remove(oldKey);
                    _banksByGuid[bankId] = promoted;
                    _guidByPath[filePath] = bankId;
                    return true;
                }
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

    // A bank may already be resident in the Studio system even when this
    // registry has just been rebuilt (for example after an Editor-side bank
    // refresh or a Play-mode transition). FMOD reports that perfectly valid
    // state as ERR_EVENT_ALREADY_LOADED and does not return the existing
    // handle from loadBankFile. Recover the handle by its stable bank ID so
    // loading remains idempotent and the registry can resume ownership.
    if (result == FMOD_ERR_EVENT_ALREADY_LOADED && bankId.IsValid())
    {
        FMOD_GUID fmodGuid = FmodConvert::ToFmodStudioGuid(bankId);
        result = _system->getBankByID(&fmodGuid, &bank);
    }
    if (!FmodConvert::CheckResult(result, "loadBankFile") || !bank)
        return false;

    BankEntry entry;
    entry.Bank = bank;
    entry.Path = filePath;
    entry.RefCount = 1;
    entry.State = nonBlocking ? AudioBankState::Loading : AudioBankState::Loaded;
    entry.LastResult = result;
    entry.FileRevision = filePath.HasChars() ? (uint64)FileSystem::GetFileLastEditTime(filePath).Ticks : 0;

    Guid resolvedGuid = ResolveBankId(bankId);
    bool syntheticKey = false;
    if (!resolvedGuid.IsValid())
    {
        FMOD_GUID fmodGuid;
        // Non-blocking banks are not guaranteed to expose their ID until the
        // metadata load completes. Path-based callers are already tracked by
        // _guidByPath, so use an internal key without issuing a premature
        // getID call that FMOD reports as ERR_NOTREADY.
        if (!nonBlocking && bank->getID(&fmodGuid) == FMOD_OK)
            resolvedGuid = FmodConvert::FromFmodStudioGuid(fmodGuid);
        else
        {
            resolvedGuid = Guid::New();
            syntheticKey = true;
        }
    }
    entry.SyntheticKey = syntheticKey;

    _banksByGuid[resolvedGuid] = entry;
    if (filePath.HasChars())
        _guidByPath[filePath] = resolvedGuid;

    return true;
}

bool FmodBankRegistry::Unload(const Guid& bankId, const StringView& path)
{
    if (!_system || (!bankId.IsValid() && path.IsEmpty()))
        return false;

    Guid resolvedGuid = ResolveBankId(bankId);
    BankEntry* entry = resolvedGuid.IsValid() ? _banksByGuid.TryGet(resolvedGuid) : nullptr;
    if (!entry && path.HasChars())
    {
        String filePath;
        if (!FmodBankPathResolver::Resolve(path, filePath))
            return false;
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
        if (entry->Bank && entry->Bank->unload() != FMOD_OK)
        {
            entry->RefCount = 1;
            entry->State = AudioBankState::Error;
            return false;
        }
        // Studio bank unloading is asynchronous. Complete it before removing
        // the registry entry so an immediate editor reload cannot race the old
        // physical bank and receive FMOD_ERR_EVENT_ALREADY_LOADED.
        if (_system->flushCommands() != FMOD_OK)
        {
            entry->RefCount = 1;
            entry->State = AudioBankState::Error;
            return false;
        }
        if (entry->Path.HasChars())
            _guidByPath.Remove(entry->Path);
        _banksByGuid.Remove(resolvedGuid);
        Array<Guid, InlinedAllocation<4>> aliases;
        for (const auto& alias : _aliases)
            if (alias.Value == resolvedGuid || alias.Key == resolvedGuid)
                aliases.Add(alias.Key);
        for (const Guid& alias : aliases)
            _aliases.Remove(alias);
    }
    return true;
}

bool FmodBankRegistry::UnloadAll()
{
    bool success = true;
    Array<Guid, InlinedAllocation<16>> unloaded;
    if (_system)
    {
        for (auto& it : _banksByGuid)
        {
            const FMOD_RESULT result = it.Value.Bank ? it.Value.Bank->unload() : FMOD_OK;
            if (result == FMOD_OK)
                unloaded.Add(it.Key);
            else
            {
                it.Value.State = AudioBankState::Error;
                it.Value.LastResult = result;
                success = false;
            }
        }

        // Bank::unload queues work in FMOD Studio. ReloadBanks deliberately
        // follows UnloadAll with loadBankFile in the same frame, so make the
        // ownership handoff synchronous at this boundary.
        if (!unloaded.IsEmpty())
        {
            const FMOD_RESULT result = _system->flushCommands();
            if (result != FMOD_OK)
            {
                FmodConvert::CheckResult(result, "flushCommands after unloading banks");
                success = false;
            }
        }
    }
    for (const Guid& id : unloaded)
    {
        BankEntry* entry = _banksByGuid.TryGet(id);
        if (entry && entry->Path.HasChars())
            _guidByPath.Remove(entry->Path);
        _banksByGuid.Remove(id);
    }
    _aliases.Clear();
    return success;
}

bool FmodBankRegistry::IsLoaded(const Guid& bankId) const
{
    const BankEntry* entry = _banksByGuid.TryGet(ResolveBankId(bankId));
    if (!entry || !entry->Bank)
        return false;

    FMOD_STUDIO_LOADING_STATE state;
    if (entry->Bank->getLoadingState(&state) == FMOD_OK)
        return state == FMOD_STUDIO_LOADING_STATE_LOADED;

    return false;
}

bool FmodBankRegistry::LoadSampleData(const Guid& bankId)
{
    BankEntry* entry = _banksByGuid.TryGet(ResolveBankId(bankId));
    if (!entry || !entry->Bank)
        return false;

    const FMOD_RESULT result = entry->Bank->loadSampleData();
    entry->SampleDataLoaded = result == FMOD_OK;
    return entry->SampleDataLoaded;
}

void FmodBankRegistry::UnloadSampleData(const Guid& bankId)
{
    BankEntry* entry = _banksByGuid.TryGet(ResolveBankId(bankId));
    if (entry && entry->Bank)
    {
        entry->Bank->unloadSampleData();
        entry->SampleDataLoaded = false;
    }
}

AudioBankState FmodBankRegistry::GetState(const Guid& bankId) const
{
    const BankEntry* entry = _banksByGuid.TryGet(ResolveBankId(bankId));
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

bool FmodBankRegistry::Query(const Guid& bankId, const StringView& path, AudioBankRuntimeState& outState) const
{
    outState = AudioBankRuntimeState();
    Guid resolved = ResolveBankId(bankId);
    const BankEntry* entry = resolved.IsValid() ? _banksByGuid.TryGet(resolved) : nullptr;
    if (!entry && path.HasChars())
    {
        String filePath;
        if (!FmodBankPathResolver::Resolve(path, filePath))
            return false;
        const Guid* found = _guidByPath.TryGet(filePath);
        if (found)
        {
            resolved = *found;
            entry = _banksByGuid.TryGet(*found);
        }
    }
    if (!entry)
        return false;
    FMOD_STUDIO_LOADING_STATE loadingState;
    if (entry->Bank->getLoadingState(&loadingState) != FMOD_OK)
        outState.State = AudioBankState::Error;
    else if (loadingState == FMOD_STUDIO_LOADING_STATE_LOADED)
        outState.State = AudioBankState::Loaded;
    else if (loadingState == FMOD_STUDIO_LOADING_STATE_LOADING)
        outState.State = AudioBankState::Loading;
    else
        outState.State = AudioBankState::Error;
    outState.SampleDataLoaded = entry->SampleDataLoaded;
    outState.AssetId = resolved;
    outState.RefCount = entry->RefCount;
    outState.Path = entry->Path;
    outState.FileRevision = entry->FileRevision;
    outState.Name = String(StringUtils::GetFileNameWithoutExtension(entry->Path));
    outState.LastResult = (int32)entry->LastResult;
    return true;
}

FMOD::Studio::Bank* FmodBankRegistry::Get(const Guid& bankId) const
{
    const BankEntry* entry = _banksByGuid.TryGet(ResolveBankId(bankId));
    return entry ? entry->Bank : nullptr;
}

int32 FmodBankRegistry::GetSampleDataLoadedCount() const
{
    int32 result = 0;
    for (const auto& item : _banksByGuid)
        result += item.Value.SampleDataLoaded ? 1 : 0;
    return result;
}

void FmodBankRegistry::Capture(Array<AudioBankRuntimeState>& result) const
{
    result.Clear();
    result.EnsureCapacity(_banksByGuid.Count());
    for (const auto& item : _banksByGuid)
    {
        AudioBankRuntimeState& state = result.AddOne();
        Query(item.Key, item.Value.Path, state);
    }
}

Guid FmodBankRegistry::ResolveBankId(const Guid& bankId) const
{
    const Guid* resolved = bankId.IsValid() ? _aliases.TryGet(bankId) : nullptr;
    return resolved ? *resolved : bankId;
}

#endif
