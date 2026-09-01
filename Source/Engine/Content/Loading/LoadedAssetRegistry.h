// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Platform/CriticalSection.h"

enum class LoadedAssetState : byte
{
    Unresolved,
    Loading,
    Loaded,
};

/// <summary>Stable registry state for one exact asset object.</summary>
struct FLAXENGINE_API LoadedAssetRecord
{
    Guid Object = Guid::Empty;
    LoadedAssetState State = LoadedAssetState::Unresolved;
    void* Instance = nullptr;
    StringAnsi TypeName;
    ContentHash Content;
    uint64 Revision = 0;
    Array<Guid> Dependencies;
    AssetPipelineDiagnostic Diagnostic;
};

/// <summary>Opaque ownership token for one deduplicated load attempt.</summary>
struct FLAXENGINE_API LoadedAssetLoadTicket
{
    Guid Object = Guid::Empty;
    uint64 Attempt = 0;
};

enum class LoadedAssetAcquireResult : byte
{
    Owner,
    Ready,
    Joined,
    Invalid,
};

/// <summary>One replacement prepared off-thread for an atomic registry swap.</summary>
struct FLAXENGINE_API LoadedAssetReplacement
{
    Guid Object = Guid::Empty;
    void* Instance = nullptr;
    StringAnsi TypeName;
    ContentHash Content;
    uint64 Revision = 0;
    Array<Guid> Dependencies;
};

/// <summary>Old and new values produced by an accepted replacement.</summary>
struct FLAXENGINE_API LoadedAssetSwap
{
    Guid Object = Guid::Empty;
    void* PreviousInstance = nullptr;
    StringAnsi PreviousTypeName;
    ContentHash PreviousContent;
    uint64 PreviousRevision = 0;
    void* Instance = nullptr;
    StringAnsi TypeName;
    ContentHash Content;
    uint64 Revision = 0;
};

/// <summary>Thread-safe object-level registry used to deduplicate loads and publish hot-reload swaps.</summary>
class FLAXENGINE_API LoadedAssetRegistry : public NonCopyable
{
private:
    struct Entry;

    mutable CriticalSection _locker;
    ConditionVariable _changed;
    Dictionary<Guid, Entry*> _entries;

public:
    LoadedAssetRegistry() = default;
    ~LoadedAssetRegistry();

    /// <summary>Claims a load or joins the exact attempt already running for the same object.</summary>
    LoadedAssetAcquireResult AcquireLoad(const Guid& object, LoadedAssetLoadTicket& ticket,
        LoadedAssetRecord& record, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Publishes the result of a claimed load attempt.</summary>
    /// <returns>True if the ticket is stale or invalid.</returns>
    bool CompleteLoad(const LoadedAssetLoadTicket& ticket, void* instance, const StringAnsiView& typeName,
        const ContentHash& content, uint64 revision,
        const Array<Guid>& dependencies, const AssetPipelineDiagnostic& loadDiagnostic,
        LoadedAssetRecord& record, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Gets registry state while preserving unresolved object identity.</summary>
    bool TryGet(const Guid& object, LoadedAssetRecord& record) const;

    /// <summary>Forgets an unloaded instance so a future request materializes it again.</summary>
    /// <returns>True if the object was missing or referred to a different instance.</returns>
    bool Remove(const Guid& object, void* instance);

    /// <summary>Atomically validates and applies a complete replacement batch.</summary>
    /// <returns>True on missing objects, duplicate objects, or stale revisions.</returns>
    bool ReplaceBatch(const Array<LoadedAssetReplacement>& replacements, Array<LoadedAssetSwap>& swaps,
        AssetPipelineDiagnostic& diagnostic);

    int32 Count() const;
};
