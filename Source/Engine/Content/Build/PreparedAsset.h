// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/AssetDependency.h"
#include "Engine/Content/AssetDatabase/SubAsset.h"
#include <atomic>
#include <memory>

/// <summary>Shared cancellation state passed to processor stages.</summary>
class FLAXENGINE_API AssetCancellationToken
{
    friend class AssetCancellationSource;

private:
    struct State
    {
        std::atomic<bool> Cancelled { false };
    };
    std::shared_ptr<State> _state;

    explicit AssetCancellationToken(const std::shared_ptr<State>& state)
        : _state(state)
    {
    }

public:
    AssetCancellationToken() = default;

    bool IsCancellationRequested() const
    {
        return _state && _state->Cancelled.load(std::memory_order_acquire);
    }
};

/// <summary>Owner that can cancel all copies of its token.</summary>
class FLAXENGINE_API AssetCancellationSource
{
private:
    std::shared_ptr<AssetCancellationToken::State> _state = std::make_shared<AssetCancellationToken::State>();

public:
    AssetCancellationToken GetToken() const
    {
        return AssetCancellationToken(_state);
    }

    void Cancel()
    {
        _state->Cancelled.store(true, std::memory_order_release);
    }
};

/// <summary>One prepared artifact output selected before key calculation.</summary>
struct FLAXENGINE_API DeclaredArtifactOutput
{
    StringAnsi Kind;
    StringAnsi Extension;
    uint32 FormatVersion = 1;
    ArtifactTargetDimension TargetDimensions = ArtifactTargetDimension::None;
    StringAnsi CompatibilityTag;
    Guid EffectiveAssetID = Guid::Empty;
};

/// <summary>Processor-owned parsed payload with explicit memory accounting.</summary>
class FLAXENGINE_API PreparedAssetPayload
{
public:
    virtual ~PreparedAssetPayload() = default;
    virtual uint64 GetMemoryUsage() const = 0;
};

/// <summary>Deterministic Prepare result consumed by key planning and Build.</summary>
struct FLAXENGINE_API PreparedAsset
{
    Guid AssetID = Guid::Empty;
    String OutputType;
    uint64 DatabaseRevision = 0;
    ContentHash SettingsHash;
    ArtifactKey InputFingerprint;
    Array<DeclaredArtifactOutput> Outputs;
    Array<AssetDependency> Dependencies;
    Array<SubAssetCandidate> SubAssets;
    std::shared_ptr<PreparedAssetPayload> Payload;
    uint64 MemoryEstimate = 0;
};
