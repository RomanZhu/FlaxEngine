// Copyright (c) Wojciech Figat. All rights reserved.

#include "LoadedAssetRegistry.h"
#include "Engine/Core/Collections/HashSet.h"

struct LoadedAssetRegistry::Entry
{
    LoadedAssetRecord Record;
    uint64 Attempt = 0;
};

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const Guid& object,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = object;
        diagnostic.Message = message;
        return true;
    }
}

LoadedAssetRegistry::~LoadedAssetRegistry()
{
    _entries.ClearDelete();
}

LoadedAssetAcquireResult LoadedAssetRegistry::AcquireLoad(const Guid& object, LoadedAssetLoadTicket& ticket,
    LoadedAssetRecord& record, AssetPipelineDiagnostic& diagnostic)
{
    ticket = LoadedAssetLoadTicket();
    record = LoadedAssetRecord();
    if (!object.IsValid())
    {
        Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, object, TEXT("Cannot load an invalid asset object ID."));
        return LoadedAssetAcquireResult::Invalid;
    }

    _locker.Lock();
    Entry** entryPtr = _entries.TryGet(object);
    if (!entryPtr)
    {
        Entry* entry = New<Entry>();
        entry->Record.Object = object;
        _entries.Add(object, entry);
        entryPtr = _entries.TryGet(object);
    }
    Entry* entry = *entryPtr;
    bool joined = false;
    while (entry->Record.State == LoadedAssetState::Loading)
    {
        joined = true;
        _changed.Wait(_locker);
    }
    if (joined)
    {
        record = entry->Record;
        diagnostic = record.Diagnostic;
        _locker.Unlock();
        return LoadedAssetAcquireResult::Joined;
    }
    if (entry->Record.State == LoadedAssetState::Loaded)
    {
        record = entry->Record;
        diagnostic = AssetPipelineDiagnostic();
        _locker.Unlock();
        return LoadedAssetAcquireResult::Ready;
    }

    entry->Attempt++;
    entry->Record.State = LoadedAssetState::Loading;
    entry->Record.Instance = nullptr;
    entry->Record.Diagnostic = AssetPipelineDiagnostic();
    ticket.Object = object;
    ticket.Attempt = entry->Attempt;
    record = entry->Record;
    diagnostic = AssetPipelineDiagnostic();
    _locker.Unlock();
    return LoadedAssetAcquireResult::Owner;
}

bool LoadedAssetRegistry::CompleteLoad(const LoadedAssetLoadTicket& ticket, void* instance, uint64 revision,
    const Array<Guid>& dependencies, const AssetPipelineDiagnostic& loadDiagnostic,
    LoadedAssetRecord& record, AssetPipelineDiagnostic& diagnostic)
{
    _locker.Lock();
    Entry** entryPtr = _entries.TryGet(ticket.Object);
    if (!entryPtr || (*entryPtr)->Attempt != ticket.Attempt || (*entryPtr)->Record.State != LoadedAssetState::Loading)
    {
        _locker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, ticket.Object,
            TEXT("Asset load completion no longer owns the current registry attempt."));
    }
    Entry* entry = *entryPtr;
    if (instance && revision != 0)
    {
        entry->Record.State = LoadedAssetState::Loaded;
        entry->Record.Instance = instance;
        entry->Record.Revision = revision;
        entry->Record.Dependencies = dependencies;
        entry->Record.Diagnostic = AssetPipelineDiagnostic();
    }
    else
    {
        entry->Record.State = LoadedAssetState::Unresolved;
        entry->Record.Instance = nullptr;
        entry->Record.Revision = 0;
        entry->Record.Dependencies.Clear();
        entry->Record.Diagnostic = loadDiagnostic;
    }
    record = entry->Record;
    diagnostic = entry->Record.Diagnostic;
    _changed.NotifyAll();
    _locker.Unlock();
    return false;
}

bool LoadedAssetRegistry::TryGet(const Guid& object, LoadedAssetRecord& record) const
{
    _locker.Lock();
    Entry** entry = _entries.TryGet(object);
    if (entry)
        record = (*entry)->Record;
    _locker.Unlock();
    return entry != nullptr;
}

bool LoadedAssetRegistry::Remove(const Guid& object, void* instance)
{
    ScopeLock lock(_locker);
    Entry** entry = _entries.TryGet(object);
    if (!entry || (*entry)->Record.State == LoadedAssetState::Loading || (*entry)->Record.Instance != instance)
        return true;
    Delete(*entry);
    _entries.Remove(object);
    return false;
}

bool LoadedAssetRegistry::ReplaceBatch(const Array<LoadedAssetReplacement>& replacements, Array<LoadedAssetSwap>& swaps,
    AssetPipelineDiagnostic& diagnostic)
{
    swaps.Clear();
    if (replacements.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, Guid::Empty,
            TEXT("Asset replacement batch cannot be empty."));

    _locker.Lock();
    HashSet<Guid> objects;
    for (const LoadedAssetReplacement& replacement : replacements)
    {
        Entry** entry = _entries.TryGet(replacement.Object);
        if (!replacement.Object.IsValid() || !replacement.Instance || replacement.Revision == 0 ||
            !objects.Add(replacement.Object) || !entry || (*entry)->Record.State != LoadedAssetState::Loaded)
        {
            _locker.Unlock();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, replacement.Object,
                TEXT("Asset replacement requires one unique, currently loaded object."));
        }
        if (replacement.Revision <= (*entry)->Record.Revision)
        {
            _locker.Unlock();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, replacement.Object,
                TEXT("Asset replacement revision is stale."));
        }
    }

    swaps.EnsureCapacity(replacements.Count());
    for (const LoadedAssetReplacement& replacement : replacements)
    {
        Entry* entry = *_entries.TryGet(replacement.Object);
        LoadedAssetSwap swap;
        swap.Object = replacement.Object;
        swap.PreviousInstance = entry->Record.Instance;
        swap.PreviousRevision = entry->Record.Revision;
        swap.Instance = replacement.Instance;
        swap.Revision = replacement.Revision;
        swaps.Add(swap);
        entry->Record.Instance = replacement.Instance;
        entry->Record.Revision = replacement.Revision;
        entry->Record.Dependencies = replacement.Dependencies;
        entry->Record.Diagnostic = AssetPipelineDiagnostic();
    }
    diagnostic = AssetPipelineDiagnostic();
    _locker.Unlock();
    return false;
}

int32 LoadedAssetRegistry::Count() const
{
    _locker.Lock();
    const int32 result = _entries.Count();
    _locker.Unlock();
    return result;
}
