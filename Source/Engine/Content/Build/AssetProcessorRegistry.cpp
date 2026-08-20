// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetProcessorRegistry.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/Types/Pair.h"
#include "Engine/Threading/Threading.h"
#include <algorithm>

namespace
{
    template<typename View>
    bool IsStableToken(const View& value, bool requireDot)
    {
        if (value.IsEmpty() || value[0] == '.' || value[value.Length() - 1] == '.')
            return false;
        bool hasDot = false;
        bool previousDot = false;
        for (int32 i = 0; i < value.Length(); i++)
        {
            const auto c = value[i];
            const bool dot = c == '.';
            if (dot)
            {
                if (previousDot)
                    return false;
                hasDot = true;
            }
            else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            {
                return false;
            }
            previousDot = dot;
        }
        return !requireDot || hasDot;
    }

    bool SourceKindsOverlap(const Array<AssetSourceKind>& a, const Array<AssetSourceKind>& b)
    {
        for (AssetSourceKind left : a)
        {
            if (b.Contains(left))
                return true;
        }
        return false;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& processor, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.ProcessorId = processor;
        diagnostic.Message = message;
        return true;
    }
}

AssetProcessorLease::AssetProcessorLease(AssetProcessorLease&& other) noexcept
{
    _registry = other._registry;
    _id = MoveTemp(other._id);
    _generation = other._generation;
    _descriptor = MoveTemp(other._descriptor);
    other._registry = nullptr;
    other._generation = 0;
}

AssetProcessorLease& AssetProcessorLease::operator=(AssetProcessorLease&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _registry = other._registry;
        _id = MoveTemp(other._id);
        _generation = other._generation;
        _descriptor = MoveTemp(other._descriptor);
        other._registry = nullptr;
        other._generation = 0;
    }
    return *this;
}

AssetProcessorLease::~AssetProcessorLease()
{
    Reset();
}

void AssetProcessorLease::Reset()
{
    if (_registry)
        _registry->ReleaseLease(_id, _generation);
    _registry = nullptr;
    _id.Clear();
    _generation = 0;
    _descriptor = AssetProcessorDescriptor();
}

AssetProcessorRegistration::AssetProcessorRegistration(AssetProcessorRegistration&& other) noexcept
{
    _registry = other._registry;
    _id = MoveTemp(other._id);
    _generation = other._generation;
    other._registry = nullptr;
    other._generation = 0;
}

AssetProcessorRegistration& AssetProcessorRegistration::operator=(AssetProcessorRegistration&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _registry = other._registry;
        _id = MoveTemp(other._id);
        _generation = other._generation;
        other._registry = nullptr;
        other._generation = 0;
    }
    return *this;
}

AssetProcessorRegistration::~AssetProcessorRegistration()
{
    Reset();
}

void AssetProcessorRegistration::Reset()
{
    if (_registry)
    {
        AssetPipelineDiagnostic diagnostic;
        _registry->Unregister(_id, _generation, true, diagnostic);
    }
    _registry = nullptr;
    _id.Clear();
    _generation = 0;
}

AssetProcessorRegistry& AssetProcessorRegistry::Get()
{
    static AssetProcessorRegistry instance;
    return instance;
}

void AssetProcessorRegistry::SetTrustPolicy(bool requireThirdPartyIsolation)
{
    ScopeLock lock(_locker);
    _requireThirdPartyIsolation = requireThirdPartyIsolation;
}

void AssetProcessorRegistry::SetEngineApiLevel(uint32 value)
{
    ScopeLock lock(_locker);
    _engineApiLevel = value;
}

bool AssetProcessorRegistry::ValidateDescriptor(AssetProcessorDescriptor& descriptor, AssetPipelineDiagnostic& diagnostic) const
{
    if (!IsStableToken(StringView(descriptor.ID), true) || !IsStableToken(StringView(descriptor.ProviderID), false))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor or provider ID is not a stable token."));
    if (descriptor.EngineApiLevel != _engineApiLevel || descriptor.SettingsSchemaVersion == 0 || descriptor.ImplementationVersion == 0 || descriptor.InterfaceVersion == 0)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor API or version contract is unsupported."));
    if (descriptor.SourceKinds.IsEmpty() || (descriptor.SourceExtensions.IsEmpty() && descriptor.DocumentTypes.IsEmpty()) || descriptor.Outputs.IsEmpty() || descriptor.MainOutputType.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor input claims, outputs, or main output type are missing."));
    if (!descriptor.Prepare.IsBinded() || !descriptor.Build.IsBinded())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor Prepare and Build callbacks are required."));
    if (descriptor.MaxParallelismClass.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor parallelism class is missing."));
    if (descriptor.ProviderKind == AssetProcessorProviderKind::ThirdParty)
    {
        if (descriptor.ProviderSemanticIdentity.IsZero() || descriptor.TrustMode == AssetProcessorTrustMode::BuiltInTrusted)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Third-party processor semantic identity or trust declaration is invalid."));
        if ((_requireThirdPartyIsolation || descriptor.UsesExternalProcess) && descriptor.TrustMode != AssetProcessorTrustMode::IsolatedProcess)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Project policy requires this third-party processor to run isolated."));
    }

    CanonicalJsonError jsonError;
    StringAnsi canonicalDefaults;
    if (CanonicalJsonWriter::Canonicalize(descriptor.NormalizedDefaultSettings, canonicalDefaults, jsonError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor default settings are not canonicalizable JSON."));
    descriptor.NormalizedDefaultSettings = MoveTemp(canonicalDefaults);

    for (String& extension : descriptor.SourceExtensions)
    {
        extension = extension.ToLower();
        if (extension.Length() < 2 || extension[0] != TEXT('.') || !IsStableToken(StringView(extension.Get() + 1, extension.Length() - 1), false))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor source extension is invalid."));
    }
    for (int32 i = 0; i < descriptor.SourceExtensions.Count(); i++)
    {
        for (int32 j = i + 1; j < descriptor.SourceExtensions.Count(); j++)
        {
            if (descriptor.SourceExtensions[i] == descriptor.SourceExtensions[j])
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor repeats a source extension claim."));
        }
    }
    for (int32 i = 0; i < descriptor.Outputs.Count(); i++)
    {
        AssetProcessorOutputDescriptor& output = descriptor.Outputs[i];
        output.Kind = output.Kind.ToLower();
        output.Extension = output.Extension.ToLower();
        if (!IsStableToken(StringAnsiView(output.Kind), false) || output.FormatVersion == 0 ||
            output.Extension.Length() < 2 || output.Extension[0] != '.' || !IsStableToken(StringAnsiView(output.Extension.Get() + 1, output.Extension.Length() - 1), false))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor output kind, extension, or format version is invalid."));
        for (int32 j = 0; j < i; j++)
        {
            if (descriptor.Outputs[j].Kind == output.Kind)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor output kind is duplicated."));
        }
    }
    for (const auto& entry : _providers)
    {
        const AssetProcessorDescriptor& existing = entry.Value->Descriptor;
        if (existing.ID == descriptor.ID)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor ID is already registered."));
        if (!SourceKindsOverlap(existing.SourceKinds, descriptor.SourceKinds))
            continue;
        for (const String& extension : descriptor.SourceExtensions)
        {
            if (existing.SourceExtensions.Contains(extension))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor source claim is ambiguous with another provider."));
        }
        for (const String& documentType : descriptor.DocumentTypes)
        {
            if (existing.DocumentTypes.Contains(documentType))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, descriptor.ID, TEXT("Processor document claim is ambiguous with another provider."));
        }
    }
    return false;
}

bool AssetProcessorRegistry::Register(AssetProcessorDescriptor descriptor, AssetProcessorRegistration& registration, AssetPipelineDiagnostic& diagnostic)
{
    registration.Reset();
    ScopeLock lock(_locker);
    if (ValidateDescriptor(descriptor, diagnostic))
        return true;
    descriptor.ProviderGeneration = _nextGeneration++;
    auto state = std::make_shared<ProviderState>();
    state->Descriptor = MoveTemp(descriptor);
    _providers.Add(state->Descriptor.ID, state);
    registration._registry = this;
    registration._id = state->Descriptor.ID;
    registration._generation = state->Descriptor.ProviderGeneration;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetProcessorRegistry::RegisterManaged(AssetProcessorDescriptor descriptor, AssetProcessorRegistration& registration, AssetPipelineDiagnostic& diagnostic)
{
    descriptor.ProviderKind = AssetProcessorProviderKind::Managed;
    return Register(MoveTemp(descriptor), registration, diagnostic);
}

bool AssetProcessorRegistry::Unregister(const StringView& id, uint64 generation, bool waitForQuiescence, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    Function<void()> cancel;
    _locker.Lock();
    const String key(id);
    std::shared_ptr<ProviderState>* statePtr = _providers.TryGet(key);
    if (!statePtr || (*statePtr)->Descriptor.ProviderGeneration != generation)
    {
        _locker.Unlock();
        return false;
    }
    std::shared_ptr<ProviderState> state = *statePtr;
    if (!state->Revoking)
    {
        state->Revoking = true;
        cancel = state->Descriptor.CancelProviderWork;
    }
    _locker.Unlock();
    if (cancel.IsBinded())
        cancel();

    _locker.Lock();
    while (state->ActiveInvocations > 0 && waitForQuiescence)
        _quiesced.Wait(_locker);
    if (state->ActiveInvocations > 0)
    {
        _locker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, id, TEXT("Processor provider is revoking but still has active work."));
    }
    statePtr = _providers.TryGet(key);
    if (statePtr && (*statePtr)->Descriptor.ProviderGeneration == generation)
        _providers.Remove(key);
    _locker.Unlock();
    return false;
}

bool AssetProcessorRegistry::TryAcquire(const StringView& id, AssetProcessorInvocationStage stage, AssetProcessorLease& lease, AssetPipelineDiagnostic& diagnostic)
{
    lease.Reset();
    ScopeLock lock(_locker);
    std::shared_ptr<ProviderState>* statePtr = _providers.TryGet(String(id));
    if (!statePtr || (*statePtr)->Revoking)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, id, TEXT("Processor provider is missing or unloading."));
    std::shared_ptr<ProviderState>& state = *statePtr;
    const AssetProcessorThreadAffinity affinity = stage == AssetProcessorInvocationStage::Prepare ? state->Descriptor.PrepareAffinity : state->Descriptor.BuildAffinity;
    if (affinity == AssetProcessorThreadAffinity::MainThread && !IsInMainThread())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, id, TEXT("Processor callback was requested on the wrong thread."));
    state->ActiveInvocations++;
    lease._registry = this;
    lease._id = state->Descriptor.ID;
    lease._generation = state->Descriptor.ProviderGeneration;
    lease._descriptor = state->Descriptor;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetProcessorRegistry::TryGetDescriptor(const StringView& id, AssetProcessorDescriptor& descriptor) const
{
    ScopeLock lock(_locker);
    const std::shared_ptr<ProviderState>* state = _providers.TryGet(String(id));
    if (!state || (*state)->Revoking)
        return false;
    descriptor = (*state)->Descriptor;
    return true;
}

void AssetProcessorRegistry::GetDescriptors(Array<AssetProcessorDescriptor>& descriptors) const
{
    ScopeLock lock(_locker);
    descriptors.Clear();
    for (const auto& entry : _providers)
    {
        if (!entry.Value->Revoking)
            descriptors.Add(entry.Value->Descriptor);
    }
    std::sort(descriptors.Get(), descriptors.Get() + descriptors.Count(), [](const AssetProcessorDescriptor& a, const AssetProcessorDescriptor& b)
    {
        return a.ID < b.ID;
    });
}

void AssetProcessorRegistry::Clear()
{
    Array<Pair<String, uint64>> providers;
    {
        ScopeLock lock(_locker);
        for (const auto& entry : _providers)
            providers.Add(Pair<String, uint64>(entry.Key, entry.Value->Descriptor.ProviderGeneration));
    }
    for (const Pair<String, uint64>& provider : providers)
    {
        AssetPipelineDiagnostic diagnostic;
        Unregister(provider.First, provider.Second, true, diagnostic);
    }
}

void AssetProcessorRegistry::ReleaseLease(const String& id, uint64 generation)
{
    ScopeLock lock(_locker);
    std::shared_ptr<ProviderState>* state = _providers.TryGet(id);
    if (!state || (*state)->Descriptor.ProviderGeneration != generation)
        return;
    ASSERT((*state)->ActiveInvocations > 0);
    (*state)->ActiveInvocations--;
    if ((*state)->ActiveInvocations == 0 && (*state)->Revoking)
        _quiesced.NotifyAll();
}
