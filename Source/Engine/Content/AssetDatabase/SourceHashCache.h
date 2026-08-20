// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Threading/Threading.h"

/// <summary>Disposable file state used only to avoid unnecessary content hashing.</summary>
struct FLAXENGINE_API SourceHashFileState
{
    String Path;
    uint64 Size = 0;
    int64 LastWriteTicks = 0;
    uint64 VolumeIdentity = 0;
    uint64 FileIdentity = 0;
    int64 ChangeTicks = 0;
    bool IdentityReliable = false;
    ContentHash CachedContentHash;
    uint32 CacheChecksum = 0;
};

/// <summary>Observable hashing fast-path counters.</summary>
struct FLAXENGINE_API SourceHashMetrics
{
    uint64 CacheHits = 0;
    uint64 CacheMisses = 0;
    uint64 BytesHashed = 0;
};

/// <summary>Thread-safe source content-hash cache. File metadata is never used as the semantic hash.</summary>
class FLAXENGINE_API SourceHashCache
{
public:
    void Seed(const Array<SourceHashFileState>& states);
    void Clear();

    /// <summary>Returns the SHA-256 of the current bytes and refreshed state. Returns true on failure.</summary>
    bool HashFile(const StringView& path, ContentHash& hash, SourceHashFileState& state, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Checks whether a reliable state still describes the same unchanged file.</summary>
    static bool IsStateCurrent(const SourceHashFileState& state);

    SourceHashMetrics GetMetrics() const;

private:
    mutable CriticalSection _locker;
    Dictionary<String, SourceHashFileState> _states;
    SourceHashMetrics _metrics;

    static bool CaptureState(const StringView& path, SourceHashFileState& state, AssetPipelineDiagnostic* diagnostic);
    static bool StateMatches(const SourceHashFileState& a, const SourceHashFileState& b);
    static uint32 ComputeCacheChecksum(const SourceHashFileState& state);
};
