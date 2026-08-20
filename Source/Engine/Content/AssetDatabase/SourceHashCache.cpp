// Copyright (c) Wojciech Figat. All rights reserved.

#include "SourceHashCache.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    String NormalizeCachePath(const StringView& path)
    {
        String result(path);
        result.Replace(TEXT('\\'), TEXT('/'));
        return result.ToLower();
    }

    void SetFailure(AssetPipelineDiagnostic* diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        if (!diagnostic)
            return;
        *diagnostic = AssetPipelineDiagnostic();
        diagnostic->Code = code;
        diagnostic->Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic->SourcePath = path;
        diagnostic->Message = message;
    }

    bool HashBytes(const StringView& path, ContentHash& hash, uint64& bytesHashed)
    {
        File* file = File::Open(path, FileMode::OpenExisting, FileAccess::Read, FileShare::All);
        if (!file)
            return true;
        ContentHasher hasher;
        Array<byte> buffer;
        buffer.Resize(256 * 1024, false);
        bool failed = false;
        for (;;)
        {
            uint32 bytesRead = 0;
            if (file->Read(buffer.Get(), buffer.Count(), &bytesRead))
            {
                failed = true;
                break;
            }
            if (bytesRead == 0)
                break;
            hasher.Update(buffer.Get(), bytesRead);
            bytesHashed += bytesRead;
        }
        Delete(file);
        if (!failed)
            hash = hasher.Finalize();
        return failed;
    }
}

void SourceHashCache::Seed(const Array<SourceHashFileState>& states)
{
    ScopeLock lock(_locker);
    _states.Clear();
    for (const SourceHashFileState& state : states)
        _states[NormalizeCachePath(state.Path)] = state;
}

void SourceHashCache::Clear()
{
    ScopeLock lock(_locker);
    _states.Clear();
    _metrics = SourceHashMetrics();
}

bool SourceHashCache::CaptureState(const StringView& path, SourceHashFileState& state, AssetPipelineDiagnostic* diagnostic)
{
    state = SourceHashFileState();
    state.Path = path;
#if PLATFORM_WINDOWS
    const String pathString(path);
    HANDLE handle = CreateFileW(*pathString, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        SetFailure(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, path, TEXT("Cannot inspect source file identity."));
        return true;
    }
    BY_HANDLE_FILE_INFORMATION information;
    FILE_BASIC_INFO basic;
    const bool failed = GetFileInformationByHandle(handle, &information) == 0 || GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0;
    CloseHandle(handle);
    if (failed)
    {
        SetFailure(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, path, TEXT("Cannot read stable source file state."));
        return true;
    }
    state.Size = (static_cast<uint64>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    state.LastWriteTicks = basic.LastWriteTime.QuadPart;
    state.ChangeTicks = basic.ChangeTime.QuadPart;
    state.VolumeIdentity = information.dwVolumeSerialNumber;
    state.FileIdentity = (static_cast<uint64>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
    Char volumeRoot[MAX_PATH];
    state.IdentityReliable = GetVolumePathNameW(*pathString, volumeRoot, ARRAY_COUNT(volumeRoot)) != 0 && GetDriveTypeW(volumeRoot) != DRIVE_REMOTE;
#else
    if (!FileSystem::FileExists(path))
    {
        SetFailure(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, path, TEXT("Cannot inspect missing source file."));
        return true;
    }
    state.Size = FileSystem::GetFileSize(path);
    state.LastWriteTicks = FileSystem::GetFileLastEditTime(path).Ticks;
    state.IdentityReliable = false;
#endif
    return false;
}

bool SourceHashCache::StateMatches(const SourceHashFileState& a, const SourceHashFileState& b)
{
    return a.IdentityReliable && b.IdentityReliable &&
           a.Size == b.Size &&
           a.LastWriteTicks == b.LastWriteTicks &&
           a.VolumeIdentity == b.VolumeIdentity &&
           a.FileIdentity == b.FileIdentity &&
           a.ChangeTicks == b.ChangeTicks;
}

uint32 SourceHashCache::ComputeCacheChecksum(const SourceHashFileState& state)
{
    uint32 result = GetHash(state.CachedContentHash);
    CombineHash(result, GetHash(state.Size));
    CombineHash(result, GetHash(state.LastWriteTicks));
    CombineHash(result, GetHash(state.VolumeIdentity));
    CombineHash(result, GetHash(state.FileIdentity));
    CombineHash(result, GetHash(state.ChangeTicks));
    CombineHash(result, state.IdentityReliable ? 1u : 0u);
    return result;
}

bool SourceHashCache::HashFile(const StringView& path, ContentHash& hash, SourceHashFileState& state, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    const String normalized = NormalizeCachePath(path);
    SourceHashFileState before;
    if (CaptureState(path, before, &diagnostic))
        return true;

    {
        ScopeLock lock(_locker);
        const SourceHashFileState* cached = _states.TryGet(normalized);
        if (cached && !cached->CachedContentHash.IsZero() && cached->CacheChecksum == ComputeCacheChecksum(*cached) && StateMatches(*cached, before))
        {
            _metrics.CacheHits++;
            hash = cached->CachedContentHash;
            state = *cached;
            state.Path = path;
            return false;
        }
        _metrics.CacheMisses++;
    }

    uint64 bytesHashed = 0;
    for (int32 attempt = 0; attempt < 2; attempt++)
    {
        ContentHash computed;
        if (HashBytes(path, computed, bytesHashed))
        {
            SetFailure(&diagnostic, AssetPipelineDiagnosticCode::SourceBusy, path, TEXT("Cannot read source bytes for hashing."));
            return true;
        }
        SourceHashFileState after;
        if (CaptureState(path, after, &diagnostic))
            return true;
        if (StateMatches(before, after) || (!before.IdentityReliable && before.Size == after.Size && before.LastWriteTicks == after.LastWriteTicks))
        {
            after.CachedContentHash = computed;
            after.CacheChecksum = ComputeCacheChecksum(after);
            {
                ScopeLock lock(_locker);
                _states[normalized] = after;
                _metrics.BytesHashed += bytesHashed;
            }
            hash = computed;
            state = after;
            return false;
        }
        before = after;
    }

    {
        ScopeLock lock(_locker);
        _metrics.BytesHashed += bytesHashed;
    }
    SetFailure(&diagnostic, AssetPipelineDiagnosticCode::SourceBusy, path, TEXT("Source changed repeatedly while hashing."));
    return true;
}

bool SourceHashCache::IsStateCurrent(const SourceHashFileState& state)
{
    if (state.CachedContentHash.IsZero() || !state.IdentityReliable || state.CacheChecksum != ComputeCacheChecksum(state))
        return false;
    SourceHashFileState current;
    if (CaptureState(state.Path, current, nullptr))
        return false;
    return StateMatches(state, current);
}

SourceHashMetrics SourceHashCache::GetMetrics() const
{
    ScopeLock lock(_locker);
    return _metrics;
}
