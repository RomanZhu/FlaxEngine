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
    {
        LOG(Error, "Failed to load audio event dependency bank {0} from '{1}'.", bankId, record->Path);
        return false;
    }
    if (preloadSampleData && !AudioEventSystem::LoadBankSampleData(bankId))
    {
        LOG(Error, "Failed to load sample data for audio event dependency bank {0} ('{1}').", bankId, record->Path);
        return false;
    }
    return true;
}

bool AudioEventCatalog::EnsureDependenciesLoaded(const AudioEvent* event)
{
    if (!event)
        return false;

    // The backend is the final authority. If it can already resolve the event
    // description, its metadata dependencies are necessarily available even if
    // Content references are in the middle of an editor reload or GUID remap.
    Array<AudioParameterDescription> availableParameters;
    if ((event->BackendId.IsValid() || event->Path.HasChars()) && AudioEventSystem::GetEventParameters(event->BackendId, event->Path, availableParameters))
        return true;

    // A typed event's stable backend dependency IDs are authoritative. If all
    // of them are already loaded, the contract is satisfied and there is no
    // reason to re-walk asset wrappers (which may still be completing a Content
    // reload even though the middleware bank is ready).
    if (event->BankDependencies.HasItems())
    {
        bool allLoaded = true;
        for (const Guid& bankId : event->BankDependencies)
            allLoaded &= bankId.IsValid() && AudioEventSystem::IsBankLoaded(bankId);
        if (allLoaded)
            return true;
    }

    for (const auto& bankReference : event->BankAssets)
    {
        if (!bankReference || bankReference->WaitForLoaded())
        {
            LOG(Error, "Audio event '{0}' has an unresolved bank asset reference.", event->Path);
            return false;
        }
        const AudioBank* bank = bankReference->GetInstance<AudioBank>();
        if (!bank)
        {
            LOG(Error, "Audio event '{0}' references an asset that is not an AudioBank.", event->Path);
            return false;
        }
        RegisterBank(bank);
        if (!EnsureBankLoaded(bank->BackendId, false))
        {
            LOG(Error, "Audio event '{0}' could not load dependency bank {1} ('{2}').", event->Path, bank->BackendId, bank->Path);
            return false;
        }
    }
    for (const Guid& bankId : event->BankDependencies)
        if (!EnsureBankLoaded(bankId, false))
        {
            LOG(Error, "Audio event '{0}' could not load dependency bank {1}.", event->Path, bankId);
            return false;
        }
    return true;
}
