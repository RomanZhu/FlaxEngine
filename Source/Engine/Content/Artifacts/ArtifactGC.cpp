// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactGC.h"
#include "ArtifactLease.h"
#include "ArtifactStore.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <sys/statvfs.h>
#endif

namespace
{
    struct GCCandidate
    {
        String Path;
        uint64 Size = 0;
        DateTime LastWrite;
    };

    String NormalizeGCPath(const StringView& path)
    {
        String result(path);
        StringUtils::PathRemoveRelativeParts(result);
        result.Replace(TEXT('\\'), TEXT('/'));
#if PLATFORM_WINDOWS
        result = result.ToLower();
#endif
        return result;
    }

    bool GCFail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool QueryFreeBytes(const StringView& path, const ArtifactGCOptions& options, uint64& bytes)
    {
        if (options.QueryFreeBytes.IsBinded())
            return options.QueryFreeBytes(path, bytes);
#if PLATFORM_WINDOWS
        const String value(path);
        ULARGE_INTEGER available;
        return GetDiskFreeSpaceExW(*value, &available, nullptr, nullptr) == 0 ? true : (bytes = available.QuadPart, false);
#elif PLATFORM_LINUX || PLATFORM_MAC
        const StringAnsi value(path);
        struct statvfs info;
        if (statvfs(value.Get(), &info) != 0)
            return true;
        bytes = static_cast<uint64>(info.f_bavail) * static_cast<uint64>(info.f_frsize);
        return false;
#else
        return true;
#endif
    }

    bool ScanReachable(const StringView& libraryRoot, HashSet<String>& reachable, ArtifactGCResult& result)
    {
        Array<String> manifests;
        const String manifestsRoot = ArtifactStore::GetManifestsPath(libraryRoot);
        if (FileSystem::DirectoryGetFiles(manifests, manifestsRoot, TEXT("*.json"), DirectorySearchOption::AllDirectories))
            return true;
        for (const String& path : manifests)
        {
            StringAnsi json;
            ArtifactManifest manifest;
            AssetPipelineDiagnostic diagnostic;
            if (File::ReadAllText(path, json) || ArtifactManifest::Parse(json, path, manifest, diagnostic))
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                    GCFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path, TEXT("Cannot read current artifact manifest during garbage collection."));
                result.Diagnostics.Add(diagnostic);
                result.BlockedByInvalidManifest = true;
                continue;
            }
            for (const ArtifactManifestOutput& output : manifest.Outputs)
            {
                ArtifactStoragePath resolved;
                if (ArtifactStore::TryResolveLibraryRelative(libraryRoot, output.RelativePath, resolved, diagnostic))
                {
                    result.Diagnostics.Add(diagnostic);
                    result.BlockedByInvalidManifest = true;
                    continue;
                }
                reachable.Add(NormalizeGCPath(resolved.Get()));
            }
        }
        return false;
    }
}

bool ArtifactGC::Run(const StringView& libraryRoot, const ArtifactGCOptions& options, ArtifactGCResult& result, AssetPipelineDiagnostic& diagnostic)
{
    result = ArtifactGCResult();
    diagnostic = AssetPipelineDiagnostic();
    if (options.MaximumDeletes < 0 || options.MaximumDeleteBytes == 0 || options.DiskQuotaBytes == 0 || options.GracePeriod.Ticks < 0)
        return GCFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, libraryRoot, TEXT("Artifact GC limits are invalid."));
    if (ArtifactStore::EnsureLayout(libraryRoot, diagnostic))
        return true;
    HashSet<String> reachable;
    if (ScanReachable(libraryRoot, reachable, result))
        return GCFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, ArtifactStore::GetManifestsPath(libraryRoot), TEXT("Cannot enumerate current artifact manifests."));

    Array<String> files;
    const String artifactsRoot = ArtifactStore::GetArtifactsPath(libraryRoot);
    if (FileSystem::DirectoryGetFiles(files, artifactsRoot, TEXT("*"), DirectorySearchOption::AllDirectories))
        return GCFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, artifactsRoot, TEXT("Cannot enumerate immutable artifact outputs."));
    Array<GCCandidate> candidates;
    const DateTime cutoff = DateTime::NowUTC() - options.GracePeriod;
    for (const String& path : files)
    {
        if (options.Cancellation.IsCancellationRequested())
        {
            result.WasCancelled = true;
            return false;
        }
        const uint64 size = FileSystem::GetFileSize(path);
        result.TotalArtifactBytes += size;
        result.ScannedFiles++;
        if (reachable.Contains(NormalizeGCPath(path)))
        {
            result.ReachableBytes += size;
            result.ReachableFiles++;
            continue;
        }
        if (ArtifactLease::IsLeased(path))
        {
            result.LeasedFiles++;
            continue;
        }
        const DateTime lastWrite = FileSystem::GetFileLastEditTime(path);
        if (lastWrite == DateTime::MinValue() || lastWrite > cutoff)
            continue;
        GCCandidate candidate;
        candidate.Path = path;
        candidate.Size = size;
        candidate.LastWrite = lastWrite;
        candidates.Add(MoveTemp(candidate));
        result.CandidateBytes += size;
        result.CandidateFiles++;
    }

    uint64 freeBytes = 0;
    if (QueryFreeBytes(libraryRoot, options, freeBytes))
        return GCFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, libraryRoot, TEXT("Cannot query free space for artifact garbage collection."));
    result.PressureDetected = result.TotalArtifactBytes > options.DiskQuotaBytes || freeBytes < options.MinimumFreeBytes;
    if (result.BlockedByInvalidManifest || options.DryRun || options.MaximumDeletes == 0)
        return false;
    std::sort(candidates.Get(), candidates.Get() + candidates.Count(), [](const GCCandidate& a, const GCCandidate& b)
    {
        if (a.LastWrite != b.LastWrite)
            return a.LastWrite < b.LastWrite;
        return a.Path < b.Path;
    });
    for (const GCCandidate& candidate : candidates)
    {
        if (options.Cancellation.IsCancellationRequested())
        {
            result.WasCancelled = true;
            break;
        }
        if (result.DeletedFiles >= options.MaximumDeletes || result.ReclaimedBytes + candidate.Size > options.MaximumDeleteBytes)
            break;
        bool wasLeased = false;
        if (ArtifactLease::DeleteFileIfUnleased(candidate.Path, wasLeased))
            return GCFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, candidate.Path, TEXT("Cannot delete an unreachable immutable artifact."));
        if (wasLeased)
        {
            result.LeasedFiles++;
            continue;
        }
        result.DeletedFiles++;
        result.ReclaimedBytes += candidate.Size;
        result.DeletedPaths.Add(candidate.Path);
    }
    return false;
}

bool ArtifactGC::CheckBuildCapacity(const StringView& libraryRoot, uint64 estimatedAdditionalBytes, const ArtifactGCOptions& options, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    uint64 freeBytes = 0;
    if (QueryFreeBytes(libraryRoot, options, freeBytes))
        return GCFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, libraryRoot, TEXT("Cannot verify free space before artifact build."));
    const uint64 artifactBytes = FileSystem::GetDirectorySize(ArtifactStore::GetArtifactsPath(libraryRoot));
    const bool quotaExceeded = artifactBytes > options.DiskQuotaBytes || estimatedAdditionalBytes > options.DiskQuotaBytes - artifactBytes;
    const bool freeFloorExceeded = freeBytes < options.MinimumFreeBytes || estimatedAdditionalBytes > freeBytes - Math::Min(freeBytes, options.MinimumFreeBytes);
    if (quotaExceeded || freeFloorExceeded)
        return GCFail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, libraryRoot, TEXT("Artifact build would violate the configured disk quota or minimum free-space floor."));
    return false;
}
