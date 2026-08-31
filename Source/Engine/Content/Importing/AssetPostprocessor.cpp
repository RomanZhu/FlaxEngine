// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetPostprocessor.h"
#include <algorithm>

namespace
{
    struct PostprocessorCall
    {
        String ID;
        uint64 ProviderGeneration = 0;
        AssetPostprocessorDescriptor Descriptor;
    };

    bool PostprocessorFailure(AssetPipelineDiagnostic& diagnostic, const StringView& id, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.ProcessorId = id;
        diagnostic.Message = message;
        return true;
    }
}

AssetPostprocessorRegistration::AssetPostprocessorRegistration(AssetPostprocessorRegistration&& other) noexcept
{
    *this = MoveTemp(other);
}

AssetPostprocessorRegistration& AssetPostprocessorRegistration::operator=(AssetPostprocessorRegistration&& other) noexcept
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

AssetPostprocessorRegistration::~AssetPostprocessorRegistration()
{
    Reset();
}

void AssetPostprocessorRegistration::Reset()
{
    if (_registry)
        _registry->Unregister(_id, _providerGeneration);
    _registry = nullptr;
    _id.Clear();
    _providerGeneration = 0;
}

uint64 AssetPostprocessorRegistry::GetGeneration() const
{
    ScopeLock lock(_locker);
    return _generation;
}

ArtifactKey AssetPostprocessorRegistry::GetVersionKey() const
{
    Array<AssetPostprocessorDescriptor> descriptors;
    {
        ScopeLock lock(_locker);
        for (const auto& state : _states)
        {
            if (!state.Value->Revoking)
                descriptors.Add(state.Value->Descriptor);
        }
    }
    std::sort(descriptors.Get(), descriptors.Get() + descriptors.Count(), [](const AssetPostprocessorDescriptor& a, const AssetPostprocessorDescriptor& b)
    {
        return a.ID < b.ID;
    });
    ArtifactKeyBuilder builder("flax-asset-postprocessors-v1");
    for (int32 i = 0; i < descriptors.Count(); i++)
    {
        const StringAnsi prefix = StringAnsi::Format("postprocessor-{0}-", i);
        builder.AddString(prefix + "id", descriptors[i].ID);
        builder.AddUInt32(prefix + "version", descriptors[i].Version);
        builder.AddHash(prefix + "implementation", descriptors[i].ImplementationHash);
        builder.AddUInt32(prefix + "order", static_cast<uint32>(descriptors[i].Order));
    }
    return builder.Finalize();
}

bool AssetPostprocessorRegistry::Register(AssetPostprocessorDescriptor descriptor, AssetPostprocessorRegistration& registration, AssetPipelineDiagnostic& diagnostic)
{
    registration.Reset();
    if (descriptor.ID.IsEmpty() || descriptor.Version == 0 || descriptor.ImplementationHash.IsZero() ||
        (!descriptor.Preprocess.IsBinded() && !descriptor.ProcessBatch.IsBinded()))
        return PostprocessorFailure(diagnostic, descriptor.ID, TEXT("Postprocessor identity, version, hash, or callback is invalid."));
    ScopeLock lock(_locker);
    if (_states.ContainsKey(descriptor.ID))
        return PostprocessorFailure(diagnostic, descriptor.ID, TEXT("Postprocessor ID is already registered."));
    auto state = std::make_shared<State>();
    state->Descriptor = MoveTemp(descriptor);
    state->ProviderGeneration = _nextProviderGeneration++;
    _states.Add(state->Descriptor.ID, state);
    _generation++;
    registration._registry = this;
    registration._id = state->Descriptor.ID;
    registration._providerGeneration = state->ProviderGeneration;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetPostprocessorRegistry::RunPreprocess(const AssetImportPlan& plan, bool& sourceChanged, AssetPipelineDiagnostic& diagnostic)
{
    Array<PostprocessorCall> calls;
    {
        ScopeLock lock(_locker);
        for (const auto& entry : _states)
        {
            if (entry.Value->Revoking)
                continue;
            entry.Value->ActiveCalls++;
            PostprocessorCall call;
            call.ID = entry.Key;
            call.ProviderGeneration = entry.Value->ProviderGeneration;
            call.Descriptor = entry.Value->Descriptor;
            calls.Add(MoveTemp(call));
        }
    }
    std::sort(calls.Get(), calls.Get() + calls.Count(), [](const PostprocessorCall& a, const PostprocessorCall& b)
    {
        if (a.Descriptor.Order != b.Descriptor.Order)
            return a.Descriptor.Order < b.Descriptor.Order;
        return a.ID < b.ID;
    });
    bool failed = false;
    for (int32 i = 0; i < calls.Count(); i++)
    {
        if (!failed && calls[i].Descriptor.Preprocess.IsBinded() && calls[i].Descriptor.Preprocess(plan, sourceChanged, diagnostic))
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                PostprocessorFailure(diagnostic, calls[i].ID, TEXT("Preprocessor failed without a diagnostic."));
            failed = true;
        }
        Release(calls[i].ID, calls[i].ProviderGeneration);
    }
    return failed;
}

bool AssetPostprocessorRegistry::RunBatch(const Array<AssetImportCompletion>& completed, bool& sourceChanged, AssetPipelineDiagnostic& diagnostic)
{
    Array<PostprocessorCall> calls;
    {
        ScopeLock lock(_locker);
        for (const auto& entry : _states)
        {
            if (entry.Value->Revoking)
                continue;
            entry.Value->ActiveCalls++;
            PostprocessorCall call;
            call.ID = entry.Key;
            call.ProviderGeneration = entry.Value->ProviderGeneration;
            call.Descriptor = entry.Value->Descriptor;
            calls.Add(MoveTemp(call));
        }
    }
    std::sort(calls.Get(), calls.Get() + calls.Count(), [](const PostprocessorCall& a, const PostprocessorCall& b)
    {
        if (a.Descriptor.Order != b.Descriptor.Order)
            return a.Descriptor.Order < b.Descriptor.Order;
        return a.ID < b.ID;
    });
    bool failed = false;
    for (int32 i = 0; i < calls.Count(); i++)
    {
        if (!failed && calls[i].Descriptor.ProcessBatch.IsBinded() && calls[i].Descriptor.ProcessBatch(completed, sourceChanged, diagnostic))
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                PostprocessorFailure(diagnostic, calls[i].ID, TEXT("Postprocessor failed without a diagnostic."));
            failed = true;
        }
        Release(calls[i].ID, calls[i].ProviderGeneration);
    }
    return failed;
}

void AssetPostprocessorRegistry::Unregister(const String& id, uint64 providerGeneration)
{
    _locker.Lock();
    std::shared_ptr<State>* statePointer = _states.TryGet(id);
    if (!statePointer || (*statePointer)->ProviderGeneration != providerGeneration)
    {
        _locker.Unlock();
        return;
    }
    const std::shared_ptr<State> state = *statePointer;
    state->Revoking = true;
    while (state->ActiveCalls != 0)
        _quiesced.Wait(_locker);
    _states.Remove(id);
    _generation++;
    _locker.Unlock();
}

void AssetPostprocessorRegistry::Release(const String& id, uint64 providerGeneration)
{
    ScopeLock lock(_locker);
    std::shared_ptr<State>* state = _states.TryGet(id);
    if (!state || (*state)->ProviderGeneration != providerGeneration)
        return;
    ASSERT((*state)->ActiveCalls > 0);
    (*state)->ActiveCalls--;
    if ((*state)->ActiveCalls == 0 && (*state)->Revoking)
        _quiesced.NotifyAll();
}
