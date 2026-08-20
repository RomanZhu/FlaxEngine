// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactLease.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Threading/Threading.h"

namespace
{
    CriticalSection ArtifactLeasesLocker;
    Dictionary<String, int32> ArtifactLeases;

    String NormalizeLeasePath(const StringView& path)
    {
        String result(path);
        StringUtils::PathRemoveRelativeParts(result);
        result.Replace((Char)92, '/');
#if PLATFORM_WINDOWS
        result = result.ToLower();
#endif
        while (result.Length() > 1 && result.EndsWith(TEXT("/")))
            result.Resize(result.Length() - 1);
        return result;
    }

    bool IsSameOrChildPath(const StringView& path, const StringView& root)
    {
        if (path == root)
            return true;
        return path.Length() > root.Length() && path.StartsWith(root) && path[root.Length()] == '/';
    }
}

ArtifactLease::ArtifactLease(const StringView& path)
    : _path(NormalizeLeasePath(path))
{
    if (_path.HasChars())
        AddRef(_path);
}

ArtifactLease::ArtifactLease(const ArtifactLease& other)
    : _path(other._path)
{
    if (_path.HasChars())
        AddRef(_path);
}

ArtifactLease::ArtifactLease(ArtifactLease&& other) noexcept
    : _path(MoveTemp(other._path))
{
    other._path.Clear();
}

ArtifactLease::~ArtifactLease()
{
    Reset();
}

ArtifactLease& ArtifactLease::operator=(const ArtifactLease& other)
{
    if (this != &other)
    {
        Reset();
        _path = other._path;
        if (_path.HasChars())
            AddRef(_path);
    }
    return *this;
}

ArtifactLease& ArtifactLease::operator=(ArtifactLease&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        _path = MoveTemp(other._path);
        other._path.Clear();
    }
    return *this;
}

ArtifactLease ArtifactLease::Acquire(const StringView& path)
{
    return ArtifactLease(path);
}

int32 ArtifactLease::GetCount(const StringView& path)
{
    const String normalized = NormalizeLeasePath(path);
    ScopeLock lock(ArtifactLeasesLocker);
    const int32* count = ArtifactLeases.TryGet(normalized);
    return count ? *count : 0;
}

bool ArtifactLease::IsLeased(const StringView& path)
{
    return GetCount(path) != 0;
}

bool ArtifactLease::HasLeaseWithin(const StringView& path)
{
    const String normalized = NormalizeLeasePath(path);
    ScopeLock lock(ArtifactLeasesLocker);
    for (const auto& entry : ArtifactLeases)
    {
        if (entry.Value > 0 && IsSameOrChildPath(entry.Key, normalized))
            return true;
    }
    return false;
}

bool ArtifactLease::DeleteFileIfUnleased(const StringView& path, bool& wasLeased)
{
    const String normalized = NormalizeLeasePath(path);
    ScopeLock lock(ArtifactLeasesLocker);
    const int32* count = ArtifactLeases.TryGet(normalized);
    wasLeased = count && *count > 0;
    return !wasLeased && FileSystem::DeleteFile(path);
}

void ArtifactLease::Reset()
{
    if (_path.HasChars())
    {
        RemoveRef(_path);
        _path.Clear();
    }
}

void ArtifactLease::AddRef(const StringView& path)
{
    ScopeLock lock(ArtifactLeasesLocker);
    int32* count = ArtifactLeases.TryGet(path);
    if (count)
        (*count)++;
    else
        ArtifactLeases.Add(String(path), 1);
}

void ArtifactLease::RemoveRef(const StringView& path)
{
    ScopeLock lock(ArtifactLeasesLocker);
    int32* count = ArtifactLeases.TryGet(path);
    ASSERT(count && *count > 0);
    if (--(*count) == 0)
        ArtifactLeases.Remove(path);
}
