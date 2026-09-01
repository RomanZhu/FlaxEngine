// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetObjectLoader.h"
#include "Engine/Core/Delegate.h"

/// <summary>Exact object revision requested by an asset database change.</summary>
struct FLAXENGINE_API AssetObjectRevision
{
    Guid Object = Guid::Empty;
    uint64 Revision = 0;
};

/// <summary>Synchronous bridge for publishing one atomic replacement batch on the main thread.</summary>
class FLAXENGINE_API IAssetMainThreadDispatcher
{
public:
    virtual ~IAssetMainThreadDispatcher() = default;

    /// <summary>Runs the action on the main thread and waits for completion. Returns true on dispatch failure.</summary>
    virtual bool InvokeAndWait(const Function<void()>& action) = 0;
};

/// <summary>Receives replacement notifications after the complete batch is visible in the registry.</summary>
class FLAXENGINE_API IAssetObjectReloadListener
{
public:
    virtual ~IAssetObjectReloadListener() = default;
    virtual void OnAssetObjectReplaced(const LoadedAssetSwap& swap) = 0;
};

/// <summary>Prepares replacements off-thread and commits them atomically with dependency-first notifications.</summary>
class FLAXENGINE_API AssetHotReloadCoordinator : public NonCopyable
{
private:
    LoadedAssetRegistry& _registry;
    AssetObjectLoader& _loader;
    IAssetMainThreadDispatcher& _dispatcher;
    IAssetObjectReloadListener& _listener;

    static bool BuildNotificationOrder(const Array<LoadedAssetReplacement>& replacements, Array<int32>& order,
        AssetPipelineDiagnostic& diagnostic);

public:
    AssetHotReloadCoordinator(LoadedAssetRegistry& registry, AssetObjectLoader& loader,
        IAssetMainThreadDispatcher& dispatcher, IAssetObjectReloadListener& listener)
        : _registry(registry)
        , _loader(loader)
        , _dispatcher(dispatcher)
        , _listener(listener)
    {
    }

    /// <summary>Reloads one exact revision batch. Returns true without partial publication on failure.</summary>
    bool Reload(const Array<AssetObjectRevision>& changes, AssetPipelineDiagnostic& diagnostic);
};
