// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEventCatalog.h"
#include "AudioEventSystem.h"
#include "Assets/AudioBank.h"
#include "Assets/AudioEvent.h"
#include "Engine/Core/Log.h"

Dictionary<Guid, AudioEventCatalog::BankRecord> AudioEventCatalog::_banks;

void AudioEventCatalog::Clear()
{
    _banks.Clear();
}

void AudioEventCatalog::RegisterBank(const AudioBank* bank)
{
    if (!bank || !bank->BackendId.IsValid())
        return;
    BankRecord& record = _banks[bank->BackendId];
    record.Path = bank->Path;
    record.Dependencies = bank->ReferencedBanks;
    record.NonBlocking = bank->NonBlocking;
}

bool AudioEventCatalog::EnsureBankLoaded(const Guid& bankId, bool preloadSampleData)
{
    Array<Guid> visiting;
    return EnsureBankLoaded(bankId, preloadSampleData, visiting);
}

bool AudioEventCatalog::EnsureBankLoaded(const Guid& bankId, bool preloadSampleData, Array<Guid>& visiting)
{
    if (!bankId.IsValid())
        return false;
    if (AudioEventSystem::IsBankLoaded(bankId))
        return true;
    if (visiting.Contains(bankId))
    {
        LOG(Error, "Audio event catalog bank dependency cycle detected at {0}.", bankId);
        return false;
    }
    const BankRecord* record = _banks.TryGet(bankId);
    if (!record)
    {
        LOG(Error, "Audio event catalog has no bank record for dependency {0}.", bankId);
        return false;
    }
    visiting.Add(bankId);
    for (const Guid& dependency : record->Dependencies)
        if (!AudioEventSystem::IsBankLoaded(dependency) && !EnsureBankLoaded(dependency, false, visiting))
        {
            visiting.Remove(bankId);
            return false;
        }
    visiting.Remove(bankId);
    if (!AudioEventSystem::LoadBank(bankId, record->Path, record->NonBlocking))
        return false;
    return !preloadSampleData || AudioEventSystem::LoadBankSampleData(bankId);
}

bool AudioEventCatalog::EnsureDependenciesLoaded(const AudioEvent* event)
{
    if (!event)
        return false;
    for (const auto& bankReference : event->BankAssets)
    {
        if (!bankReference || bankReference->WaitForLoaded())
            return false;
        const AudioBank* bank = bankReference->GetInstance<AudioBank>();
        if (!bank)
            return false;
        RegisterBank(bank);
        if (!EnsureBankLoaded(bank->BackendId, false))
            return false;
    }
    for (const Guid& bankId : event->BankDependencies)
        if (!EnsureBankLoaded(bankId, false))
            return false;
    return true;
}
