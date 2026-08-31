// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImporterRegistry.h"
#include "Engine/Core/Types/Pair.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& importer, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.ProcessorId = importer;
        diagnostic.Message = message;
        return true;
    }

    bool StableToken(const StringView& value)
    {
        if (value.IsEmpty() || value[0] == '.' || value[value.Length() - 1] == '.')
            return false;
        bool previousDot = false;
        for (int32 i = 0; i < value.Length(); i++)
        {
            const Char c = value[i];
            if (c == '.' && previousDot)
                return false;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
                return false;
            previousDot = c == '.';
        }
        return true;
    }

    bool MatchesExtension(const AssetImporterDescriptor& descriptor, const StringView& extension)
    {
        return descriptor.Extensions.Contains(String(extension));
    }
}

AssetImporterLease::AssetImporterLease(AssetImporterLease&& other) noexcept
{
    *this = MoveTemp(other);
}

AssetImporterLease& AssetImporterLease::operator=(AssetImporterLease&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _registry = other._registry;
        _id = MoveTemp(other._id);
        _providerGeneration = other._providerGeneration;
        _descriptor = MoveTemp(other._descriptor);
        other._registry = nullptr;
        other._providerGeneration = 0;
    }
    return *this;
}

AssetImporterLease::~AssetImporterLease()
{
    Reset();
}

void AssetImporterLease::Reset()
{
    if (_registry)
        _registry->ReleaseLease(_id, _providerGeneration);
    _registry = nullptr;
    _id.Clear();
    _providerGeneration = 0;
    _descriptor = AssetImporterDescriptor();
}

AssetImporterRegistration::AssetImporterRegistration(AssetImporterRegistration&& other) noexcept
{
    *this = MoveTemp(other);
}

AssetImporterRegistration& AssetImporterRegistration::operator=(AssetImporterRegistration&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _registry = other._registry;
        _id = MoveTemp(other._id);
        _providerGeneration = other._providerGeneration;
        other._registry = nullptr;
        other._providerGeneration = 0;
    }
    return *this;
}

AssetImporterRegistration::~AssetImporterRegistration()
{
    Reset();
}

void AssetImporterRegistration::Reset()
{
    if (_registry)
    {
        AssetPipelineDiagnostic diagnostic;
        _registry->Unregister(_id, _providerGeneration, true, diagnostic);
    }
    _registry = nullptr;
    _id.Clear();
    _providerGeneration = 0;
}

uint64 AssetImporterRegistry::GetGeneration() const
{
    ScopeLock lock(_locker);
    return _generation;
}

bool AssetImporterRegistry::Validate(AssetImporterDescriptor& descriptor, AssetPipelineDiagnostic& diagnostic) const
{
    if (!StableToken(descriptor.ID) || descriptor.ImporterVersion == 0 || descriptor.SettingsSchemaVersion == 0 || descriptor.ImplementationHash.IsZero())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer identity or version is invalid."));
    if (descriptor.ProviderID.IsEmpty())
        descriptor.ProviderID = descriptor.ID;
    if (!StableToken(descriptor.ProviderID))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer provider identity is invalid."));
    if (descriptor.Extensions.IsEmpty() && descriptor.Fallback == AssetImporterFallback::None)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer must claim an extension or a fallback role."));
    const bool hasExternalNativeWorker = descriptor.ProviderKind == AssetProcessorProviderKind::Native &&
        descriptor.ProcessSafe && !descriptor.WorkerExecutable.IsEmpty();
    if (!hasExternalNativeWorker && !descriptor.Import.IsBinded() &&
        (!descriptor.Processor.Prepare.IsBinded() || !descriptor.Processor.Build.IsBinded()))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer has no import callback or processor implementation."));
    if (descriptor.ProviderKind == AssetProcessorProviderKind::Managed && !descriptor.WorkerExecutable.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Managed importers cannot override the restricted editor worker executable."));
    if (descriptor.ProviderKind == AssetProcessorProviderKind::Native && descriptor.ProcessSafe && descriptor.Import.IsBinded() &&
        descriptor.WorkerExecutable.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID,
            TEXT("A process-safe native callback importer requires a dedicated worker executable; in-process fallback is forbidden."));
    if (descriptor.RequiresMainThread && descriptor.SupportsParallelImport)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Main-thread importers cannot claim parallel execution."));
    if (descriptor.ProcessSafe && (descriptor.MaximumMemoryBytes == 0 ||
        descriptor.MaximumOutputBytes == 0 || descriptor.MaximumOutputFiles < 1 || descriptor.ImportTimeoutMilliseconds == 0))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Process-safe importer resource limits are invalid."));
    for (String& extension : descriptor.Extensions)
    {
        extension = extension.ToLower();
        if (extension.Length() < 2 || extension[0] != '.' || !StableToken(StringView(extension.Get() + 1, extension.Length() - 1)))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer extension is invalid."));
    }
    for (int32 i = 0; i < descriptor.Extensions.Count(); i++)
    {
        for (int32 j = i + 1; j < descriptor.Extensions.Count(); j++)
        {
            if (descriptor.Extensions[i] == descriptor.Extensions[j])
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer repeats an extension claim."));
        }
    }
    return false;
}

bool AssetImporterRegistry::ReplaceProviderSet(const StringView& providerID, Array<AssetImporterDescriptor> descriptors,
                                               Array<String>& changedImporterIDs, AssetPipelineDiagnostic& diagnostic)
{
    changedImporterIDs.Clear();
    const String provider(providerID);
    if (!StableToken(provider))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, provider, TEXT("Importer provider identity is invalid."));
    if (descriptors.Count() > 1)
    {
        std::sort(descriptors.Get(), descriptors.Get() + descriptors.Count(), [](const AssetImporterDescriptor& a, const AssetImporterDescriptor& b)
        {
            return a.ID < b.ID;
        });
    }

    ScopeLock lock(_locker);
    auto markChanged = [&changedImporterIDs](const String& id)
    {
        if (!changedImporterIDs.Contains(id))
            changedImporterIDs.Add(id);
    };
    for (int32 i = 0; i < descriptors.Count(); i++)
    {
        AssetImporterDescriptor& descriptor = descriptors[i];
        descriptor.ProviderID = provider;
        if (Validate(descriptor, diagnostic))
            return true;
        if (i > 0 && descriptors[i - 1].ID == descriptor.ID)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Reloadable importer provider repeats an importer ID."));
        for (int32 j = 0; j < i; j++)
        {
            if (descriptor.Fallback != AssetImporterFallback::None && descriptors[j].Fallback == descriptor.Fallback)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Reloadable importer provider repeats a fallback role."));
        }
        for (const auto& entry : _providers)
        {
            const AssetImporterDescriptor& existing = entry.Value->Descriptor;
            if (existing.ProviderID == provider || entry.Value->Revoking)
                continue;
            if (existing.ID == descriptor.ID)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer ID is owned by another provider."));
            if (descriptor.Fallback != AssetImporterFallback::None && existing.Fallback == descriptor.Fallback)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer fallback role is owned by another provider."));
        }
    }

    Array<std::shared_ptr<ProviderState>> previous;
    for (const auto& entry : _providers)
    {
        if (entry.Value->Descriptor.ProviderID == provider)
        {
            entry.Value->Revoking = true;
            previous.Add(entry.Value);
        }
    }
    for (const std::shared_ptr<ProviderState>& state : previous)
    {
        while (state->ActiveLeases != 0)
            _quiesced.Wait(_locker);
    }

    for (const std::shared_ptr<ProviderState>& oldState : previous)
    {
        const AssetImporterDescriptor* replacement = nullptr;
        for (const AssetImporterDescriptor& descriptor : descriptors)
        {
            if (descriptor.ID == oldState->Descriptor.ID)
            {
                replacement = &descriptor;
                break;
            }
        }
        if (!replacement || replacement->ImporterVersion != oldState->Descriptor.ImporterVersion ||
            replacement->SettingsSchemaVersion != oldState->Descriptor.SettingsSchemaVersion ||
            replacement->ImplementationHash != oldState->Descriptor.ImplementationHash ||
            replacement->WorkerExecutable != oldState->Descriptor.WorkerExecutable ||
            replacement->ProcessSafe != oldState->Descriptor.ProcessSafe)
            markChanged(oldState->Descriptor.ID);
        _providers.Remove(oldState->Descriptor.ID);
    }
    for (AssetImporterDescriptor& descriptor : descriptors)
    {
        bool existedUnchanged = false;
        for (const std::shared_ptr<ProviderState>& oldState : previous)
        {
            if (oldState->Descriptor.ID == descriptor.ID && oldState->Descriptor.ImporterVersion == descriptor.ImporterVersion &&
                oldState->Descriptor.SettingsSchemaVersion == descriptor.SettingsSchemaVersion &&
                oldState->Descriptor.ImplementationHash == descriptor.ImplementationHash &&
                oldState->Descriptor.WorkerExecutable == descriptor.WorkerExecutable &&
                oldState->Descriptor.ProcessSafe == descriptor.ProcessSafe)
            {
                existedUnchanged = true;
                break;
            }
        }
        if (!existedUnchanged)
            markChanged(descriptor.ID);
        auto state = std::make_shared<ProviderState>();
        state->Descriptor = MoveTemp(descriptor);
        state->ProviderGeneration = _nextProviderGeneration++;
        _providers.Add(state->Descriptor.ID, state);
    }
    if (previous.HasItems() || descriptors.HasItems())
        _generation++;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImporterRegistry::Register(AssetImporterDescriptor descriptor, AssetImporterRegistration& registration, AssetPipelineDiagnostic& diagnostic)
{
    registration.Reset();
    ScopeLock lock(_locker);
    if (Validate(descriptor, diagnostic))
        return true;
    if (_providers.ContainsKey(descriptor.ID))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer ID is already registered."));
    if (descriptor.Fallback != AssetImporterFallback::None)
    {
        for (const auto& provider : _providers)
        {
            if (!provider.Value->Revoking && provider.Value->Descriptor.Fallback == descriptor.Fallback)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Importer fallback role is already registered."));
        }
    }
    auto state = std::make_shared<ProviderState>();
    state->Descriptor = MoveTemp(descriptor);
    state->ProviderGeneration = _nextProviderGeneration++;
    _providers.Add(state->Descriptor.ID, state);
    _generation++;
    registration._registry = this;
    registration._id = state->Descriptor.ID;
    registration._providerGeneration = state->ProviderGeneration;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImporterRegistry::Unregister(const StringView& id, uint64 providerGeneration, bool waitForLeases, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    _locker.Lock();
    const String key(id);
    std::shared_ptr<ProviderState>* statePointer = _providers.TryGet(key);
    if (!statePointer || (*statePointer)->ProviderGeneration != providerGeneration)
    {
        _locker.Unlock();
        return false;
    }
    const std::shared_ptr<ProviderState> state = *statePointer;
    state->Revoking = true;
    while (waitForLeases && state->ActiveLeases != 0)
        _quiesced.Wait(_locker);
    if (state->ActiveLeases != 0)
    {
        _locker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, id, TEXT("Importer is still leased."));
    }
    statePointer = _providers.TryGet(key);
    if (statePointer && (*statePointer)->ProviderGeneration == providerGeneration)
    {
        _providers.Remove(key);
        _generation++;
    }
    _locker.Unlock();
    return false;
}

bool AssetImporterRegistry::TryAcquire(const StringView& id, AssetImporterLease& lease, AssetPipelineDiagnostic& diagnostic)
{
    lease.Reset();
    ScopeLock lock(_locker);
    std::shared_ptr<ProviderState>* state = _providers.TryGet(String(id));
    if (!state || (*state)->Revoking)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, id, TEXT("Importer is missing or unloading."));
    (*state)->ActiveLeases++;
    lease._registry = this;
    lease._id = (*state)->Descriptor.ID;
    lease._providerGeneration = (*state)->ProviderGeneration;
    lease._descriptor = (*state)->Descriptor;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetImporterRegistry::Resolve(const AssetImporterSelectionRequest& request, AssetImporterLease& lease, AssetPipelineDiagnostic& diagnostic)
{
    lease.Reset();
    if (!request.ExplicitImporterID.IsEmpty())
    {
        if (TryAcquire(request.ExplicitImporterID, lease, diagnostic))
            return true;
        if (!lease.Get().SupportsOverride)
        {
            lease.Reset();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, request.ExplicitImporterID, TEXT("Importer does not permit explicit override selection."));
        }
        return false;
    }

    Array<AssetImporterDescriptor> candidates;
    GetDescriptors(candidates);
    String extension = TEXT(".") + FileSystem::GetExtension(request.SourcePath).ToLower();
    Array<AssetImporterDescriptor> matches;
    AssetImporterDescriptor fallback;
    for (const AssetImporterDescriptor& candidate : candidates)
    {
        const bool predicateMatches = !candidate.MatchesSource.IsBinded() || candidate.MatchesSource(request.SourcePath);
        if (predicateMatches && candidate.Fallback == (request.PreferTextFallback ? AssetImporterFallback::Text : AssetImporterFallback::Binary))
            fallback = candidate;
        if (!MatchesExtension(candidate, extension))
            continue;
        if (!predicateMatches)
            continue;
        matches.Add(candidate);
    }
    if (matches.IsEmpty() && !fallback.ID.IsEmpty())
        matches.Add(fallback);
    if (matches.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, StringView::Empty, TEXT("No importer owns this source."));
    std::sort(matches.Get(), matches.Get() + matches.Count(), [](const AssetImporterDescriptor& a, const AssetImporterDescriptor& b)
    {
        return a.Priority != b.Priority ? a.Priority > b.Priority : a.ID < b.ID;
    });
    return TryAcquire(matches[0].ID, lease, diagnostic);
}

void AssetImporterRegistry::GetDescriptors(Array<AssetImporterDescriptor>& descriptors) const
{
    ScopeLock lock(_locker);
    descriptors.Clear();
    for (const auto& provider : _providers)
    {
        if (!provider.Value->Revoking)
            descriptors.Add(provider.Value->Descriptor);
    }
    std::sort(descriptors.Get(), descriptors.Get() + descriptors.Count(), [](const AssetImporterDescriptor& a, const AssetImporterDescriptor& b)
    {
        return a.ID < b.ID;
    });
}

void AssetImporterRegistry::Clear()
{
    Array<Pair<String, uint64>> providers;
    {
        ScopeLock lock(_locker);
        for (const auto& provider : _providers)
            providers.Add(Pair<String, uint64>(provider.Key, provider.Value->ProviderGeneration));
    }
    for (const Pair<String, uint64>& provider : providers)
    {
        AssetPipelineDiagnostic diagnostic;
        Unregister(provider.First, provider.Second, true, diagnostic);
    }
}

void AssetImporterRegistry::ReleaseLease(const String& id, uint64 providerGeneration)
{
    ScopeLock lock(_locker);
    std::shared_ptr<ProviderState>* state = _providers.TryGet(id);
    if (!state || (*state)->ProviderGeneration != providerGeneration)
        return;
    ASSERT((*state)->ActiveLeases > 0);
    (*state)->ActiveLeases--;
    if ((*state)->ActiveLeases == 0 && (*state)->Revoking)
        _quiesced.NotifyAll();
}
