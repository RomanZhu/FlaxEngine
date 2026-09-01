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

    int32 FindPostprocessor(const Array<PostprocessorCall>& calls, const StringView& id)
    {
        for (int32 i = 0; i < calls.Count(); i++)
        {
            if (calls[i].ID == id)
                return i;
        }
        return -1;
    }

    bool AddOrderingConstraints(const Array<PostprocessorCall>& calls, int32 ownerIndex,
                                const Array<String>& constraints, bool runBefore,
                                Array<Array<int32>>& edges, Array<int32>& indegrees,
                                AssetPipelineDiagnostic& diagnostic)
    {
        const PostprocessorCall& owner = calls[ownerIndex];
        for (int32 i = 0; i < constraints.Count(); i++)
        {
            const String& targetId = constraints[i];
            for (int32 j = 0; j < i; j++)
            {
                if (constraints[j] == targetId)
                {
                    return PostprocessorFailure(diagnostic, owner.ID,
                        String::Format(TEXT("Postprocessor '{0}' contains duplicate {1} constraint '{2}'."),
                            owner.ID, runBefore ? TEXT("RunBefore") : TEXT("RunAfter"), targetId));
                }
            }
            if (targetId == owner.ID)
            {
                return PostprocessorFailure(diagnostic, owner.ID,
                    String::Format(TEXT("Postprocessor '{0}' cannot declare itself in {1}."),
                        owner.ID, runBefore ? TEXT("RunBefore") : TEXT("RunAfter")));
            }
            const int32 targetIndex = FindPostprocessor(calls, targetId);
            if (targetIndex == -1)
            {
                return PostprocessorFailure(diagnostic, owner.ID,
                    String::Format(TEXT("Postprocessor '{0}' {1} references unknown postprocessor '{2}'."),
                        owner.ID, runBefore ? TEXT("RunBefore") : TEXT("RunAfter"), targetId));
            }
            const int32 predecessor = runBefore ? ownerIndex : targetIndex;
            const int32 successor = runBefore ? targetIndex : ownerIndex;
            if (!edges[predecessor].Contains(successor))
            {
                edges[predecessor].Add(successor);
                indegrees[successor]++;
            }
        }
        return false;
    }

    bool ResolvePostprocessorOrder(Array<PostprocessorCall>& calls, AssetPipelineDiagnostic& diagnostic)
    {
        Array<Array<int32>> edges;
        edges.Resize(calls.Count());
        Array<int32> indegrees;
        indegrees.AddZeroed(calls.Count());
        for (int32 i = 0; i < calls.Count(); i++)
        {
            if (AddOrderingConstraints(calls, i, calls[i].Descriptor.RunBefore, true, edges, indegrees, diagnostic) ||
                AddOrderingConstraints(calls, i, calls[i].Descriptor.RunAfter, false, edges, indegrees, diagnostic))
                return true;
        }

        Array<byte> emitted;
        emitted.AddZeroed(calls.Count());
        Array<PostprocessorCall> ordered;
        ordered.EnsureCapacity(calls.Count());
        while (ordered.Count() != calls.Count())
        {
            int32 next = -1;
            for (int32 i = 0; i < calls.Count(); i++)
            {
                if (emitted[i] || indegrees[i] != 0)
                    continue;
                if (next == -1 || calls[i].Descriptor.Order < calls[next].Descriptor.Order ||
                    (calls[i].Descriptor.Order == calls[next].Descriptor.Order && calls[i].ID < calls[next].ID))
                    next = i;
            }
            if (next == -1)
            {
                Array<String> unresolved;
                for (int32 i = 0; i < calls.Count(); i++)
                {
                    if (!emitted[i])
                        unresolved.Add(calls[i].ID);
                }
                std::sort(unresolved.Get(), unresolved.Get() + unresolved.Count());
                String ids;
                for (int32 i = 0; i < unresolved.Count(); i++)
                {
                    if (i != 0)
                        ids += TEXT(", ");
                    ids += TEXT("'");
                    ids += unresolved[i];
                    ids += TEXT("'");
                }
                return PostprocessorFailure(diagnostic, unresolved[0],
                    String::Format(TEXT("Postprocessor ordering cycle involves {0}. Check RunBefore and RunAfter constraints."), ids));
            }
            emitted[next] = 1;
            ordered.Add(MoveTemp(calls[next]));
            for (const int32 successor : edges[next])
                indegrees[successor]--;
        }
        calls = MoveTemp(ordered);
        diagnostic = AssetPipelineDiagnostic();
        return false;
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
    ArtifactKeyBuilder builder("flax-asset-postprocessors-v2");
    for (int32 i = 0; i < descriptors.Count(); i++)
    {
        const StringAnsi prefix = StringAnsi::Format("postprocessor-{0}-", i);
        builder.AddString(prefix + "id", descriptors[i].ID);
        builder.AddUInt32(prefix + "version", descriptors[i].Version);
        builder.AddHash(prefix + "implementation", descriptors[i].ImplementationHash);
        builder.AddUInt32(prefix + "order", static_cast<uint32>(descriptors[i].Order));
        Array<StringAnsi> runBefore;
        runBefore.EnsureCapacity(descriptors[i].RunBefore.Count());
        for (const String& id : descriptors[i].RunBefore)
            runBefore.Add(StringAnsi(id));
        builder.AddSortedStrings(prefix + "run-before", runBefore);
        Array<StringAnsi> runAfter;
        runAfter.EnsureCapacity(descriptors[i].RunAfter.Count());
        for (const String& id : descriptors[i].RunAfter)
            runAfter.Add(StringAnsi(id));
        builder.AddSortedStrings(prefix + "run-after", runAfter);
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
    if (ResolvePostprocessorOrder(calls, diagnostic))
    {
        for (const PostprocessorCall& call : calls)
            Release(call.ID, call.ProviderGeneration);
        return true;
    }
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
    if (ResolvePostprocessorOrder(calls, diagnostic))
    {
        for (const PostprocessorCall& call : calls)
            Release(call.ID, call.ProviderGeneration);
        return true;
    }
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
