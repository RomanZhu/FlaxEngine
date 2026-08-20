// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactStore.h"
#include "Engine/Content/Build/PreparedAsset.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/NonCopyable.h"

struct FLAXENGINE_API ArtifactLockRecord
{
    static constexpr uint32 CurrentSchemaVersion = 1;

    uint32 SchemaVersion = CurrentSchemaVersion;
    ArtifactKey Key;
    uint64 ProcessID = 0;
    uint64 ProcessStartIdentity = 0;
    String HostIdentity;
    int64 CreatedUtcTicks = 0;
    int64 HeartbeatUtcTicks = 0;
    Guid JobID = Guid::Empty;

    bool ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const;
    static bool Parse(const StringAnsiView& json, const StringView& path, ArtifactLockRecord& record, AssetPipelineDiagnostic& diagnostic);
};

enum class ArtifactLockProcessState : byte
{
    SameProcessAlive,
    RecordedProcessDead,
    PidReused,
    Unknown,
};

using ArtifactLockLivenessProbe = Function<ArtifactLockProcessState(const ArtifactLockRecord&)>;

/// <summary>Exclusive, create-new interprocess lock for one exact immutable key.</summary>
class FLAXENGINE_API ArtifactLock : public NonCopyable
{
private:
    ArtifactStoragePath _path;
    ArtifactLockRecord _record;
    bool _ownsLock = false;

public:
    ArtifactLock() = default;
    ArtifactLock(ArtifactLock&& other) noexcept;
    ArtifactLock& operator=(ArtifactLock&& other) noexcept;
    ~ArtifactLock();

    /// <summary>Waits for or conservatively recovers a key lock. Returns true on failure.</summary>
    bool Acquire(const StringView& libraryRoot, const ArtifactKey& key, const Guid& jobId, const AssetCancellationToken& cancellation,
        AssetPipelineDiagnostic& diagnostic, uint32 timeoutMilliseconds = 30000, const ArtifactLockLivenessProbe& probe = ArtifactLockLivenessProbe());

    void Release();

    bool IsHeld() const
    {
        return _ownsLock;
    }

    const ArtifactLockRecord& GetRecord() const
    {
        return _record;
    }

    static ArtifactLockProcessState ProbeLocalProcess(const ArtifactLockRecord& record);
    static uint64 GetCurrentProcessStartIdentity();

    /// <summary>Removes only locks and staging whose recorded process identity is disproven.</summary>
    static bool RecoverAbandoned(const StringView& libraryRoot, const ArtifactLockLivenessProbe& probe, int32& recoveredCount, AssetPipelineDiagnostic& diagnostic);
};
