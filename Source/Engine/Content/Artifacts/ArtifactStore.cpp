// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactStore.h"
#include "ArtifactLock.h"
#include "ArtifactLease.h"
#include "ProjectLibrary.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    const Char* LayoutDirectories[] =
    {
        TEXT("Artifacts"),
        TEXT("Manifests"),
        TEXT("Temp"),
        TEXT("Locks"),
        TEXT("Logs"),
        TEXT("GC"),
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool IsReparsePoint(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        const DWORD attributes = GetFileAttributesW(*value);
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        return false;
#endif
    }

    bool PathFail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::LibraryPathInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    StringAnsi SanitizeIdentifier(const StringAnsiView& value)
    {
        StringAnsi prefix;
        const int32 prefixLength = Math::Min(value.Length(), 40);
        for (int32 i = 0; i < prefixLength; i++)
        {
            char c = value[i];
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                c = '_';
            prefix.Append(c);
        }
        const StringAnsi hash = ContentHash::Compute(value.Get(), value.Length()).ToString();
        prefix += '-';
        prefix += hash.Substring(0, 16);
        return prefix;
    }

    bool IsExtensionValid(const StringAnsiView& extension)
    {
        if (extension.Length() < 2 || extension.Length() > 16 || extension[0] != '.')
            return false;
        for (int32 i = 1; i < extension.Length(); i++)
        {
            const char c = extension[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                return false;
        }
        return true;
    }

    bool SetStoragePath(const StringView& libraryRoot, const StringView& candidate, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic)
    {
        String normalizedRoot(libraryRoot);
        String normalizedCandidate(candidate);
        StringUtils::PathRemoveRelativeParts(normalizedRoot);
        StringUtils::PathRemoveRelativeParts(normalizedCandidate);
        normalizedRoot.Replace(TEXT('\\'), TEXT('/'));
        normalizedCandidate.Replace(TEXT('\\'), TEXT('/'));
        while (normalizedRoot.Length() > 3 && normalizedRoot.EndsWith('/'))
            normalizedRoot.Remove(normalizedRoot.Length() - 1);
        if (FileSystem::IsRelative(normalizedCandidate))
            return PathFail(diagnostic, normalizedCandidate, TEXT("Calculated artifact store path is not absolute."));
        if (!AssetPathPolicy::IsSameOrChild(normalizedCandidate, normalizedRoot))
            return PathFail(diagnostic, normalizedCandidate, TEXT("Calculated artifact store path escaped the normalized Library root."));
        path = ArtifactStoragePath(normalizedCandidate);
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

String ArtifactStore::GetArtifactsPath(const StringView& libraryRoot)
{
    return String(libraryRoot) / TEXT("Artifacts");
}

String ArtifactStore::GetManifestsPath(const StringView& libraryRoot)
{
    return String(libraryRoot) / TEXT("Manifests");
}

String ArtifactStore::GetTemporaryPath(const StringView& libraryRoot)
{
    return String(libraryRoot) / TEXT("Temp");
}

String ArtifactStore::GetLocksPath(const StringView& libraryRoot)
{
    return String(libraryRoot) / TEXT("Locks");
}

String ArtifactStore::GetLogsPath(const StringView& libraryRoot)
{
    return String(libraryRoot) / TEXT("Logs");
}

String ArtifactStore::GetGcPath(const StringView& libraryRoot)
{
    return String(libraryRoot) / TEXT("GC");
}

bool ArtifactStore::TryGetArtifactPath(const StringView& libraryRoot, const ArtifactTarget& target, ArtifactTargetDimension dimensions,
    const Guid& assetId, const StringAnsiView& outputKind, const ArtifactKey& key, const StringAnsiView& extension,
    ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic)
{
    path = ArtifactStoragePath();
    if (!assetId.IsValid() || outputKind.IsEmpty() || key.IsZero() || !IsExtensionValid(extension))
        return PathFail(diagnostic, libraryRoot, TEXT("Artifact output path requires a valid asset, kind, key, and extension."));
    const String targetDirectory(target.BuildKey(dimensions).ToString());
    const String outputDirectory(SanitizeIdentifier(outputKind));
    const String fileName = String(key.ToString()) + String(extension);
    const String candidate = GetArtifactsPath(libraryRoot) / targetDirectory / assetId.ToString(Guid::FormatType::N).ToLower() / outputDirectory / fileName;
    return SetStoragePath(libraryRoot, candidate, path, diagnostic);
}

bool ArtifactStore::TryGetManifestPath(const StringView& libraryRoot, const ArtifactTarget& target, const Guid& assetId,
    ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic)
{
    path = ArtifactStoragePath();
    if (!assetId.IsValid())
        return PathFail(diagnostic, libraryRoot, TEXT("Artifact manifest path requires a valid asset GUID."));
    const String targetDirectory(target.BuildKey(ArtifactTargetDimension::All).ToString());
    const String candidate = GetManifestsPath(libraryRoot) / targetDirectory / (assetId.ToString(Guid::FormatType::N).ToLower() + TEXT(".json"));
    return SetStoragePath(libraryRoot, candidate, path, diagnostic);
}

bool ArtifactStore::TryGetLockPath(const StringView& libraryRoot, const ArtifactKey& key, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic)
{
    path = ArtifactStoragePath();
    if (key.IsZero())
        return PathFail(diagnostic, libraryRoot, TEXT("Artifact lock path requires a nonzero key."));
    return SetStoragePath(libraryRoot, GetLocksPath(libraryRoot) / (String(key.ToString()) + TEXT(".lock")), path, diagnostic);
}

bool ArtifactStore::TryGetJobStagingPath(const StringView& libraryRoot, const Guid& jobId, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic)
{
    path = ArtifactStoragePath();
    if (!jobId.IsValid())
        return PathFail(diagnostic, libraryRoot, TEXT("Artifact staging path requires a valid job GUID."));
    return SetStoragePath(libraryRoot, GetTemporaryPath(libraryRoot) / TEXT("Jobs") / jobId.ToString(Guid::FormatType::N).ToLower(), path, diagnostic);
}

bool ArtifactStore::TryGetJobLogPath(const StringView& libraryRoot, const StringView& buildId, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic)
{
    path = ArtifactStoragePath();
    if (buildId.IsEmpty())
        return PathFail(diagnostic, libraryRoot, TEXT("Artifact log path requires a build ID."));
    const StringAnsi buildIdUtf8(buildId);
    return SetStoragePath(libraryRoot, GetLogsPath(libraryRoot) / (String(SanitizeIdentifier(buildIdUtf8)) + TEXT(".jsonl")), path, diagnostic);
}

bool ArtifactStore::TryMakeLibraryRelative(const StringView& libraryRoot, const StringView& absolutePath, String& relativePath, AssetPipelineDiagnostic& diagnostic)
{
    relativePath.Clear();
    String normalizedRoot(libraryRoot);
    String normalizedPath(absolutePath);
    StringUtils::PathRemoveRelativeParts(normalizedRoot);
    StringUtils::PathRemoveRelativeParts(normalizedPath);
    normalizedRoot.Replace(TEXT('\\'), TEXT('/'));
    normalizedPath.Replace(TEXT('\\'), TEXT('/'));
    while (normalizedRoot.Length() > 3 && normalizedRoot.EndsWith('/'))
        normalizedRoot.Remove(normalizedRoot.Length() - 1);
    if (FileSystem::IsRelative(normalizedPath) || !AssetPathPolicy::IsSameOrChild(normalizedPath, normalizedRoot))
        return PathFail(diagnostic, absolutePath, TEXT("Artifact manifest path is not an absolute path within Library."));
    relativePath = FileSystem::ConvertAbsolutePathToRelative(normalizedRoot, normalizedPath);
    relativePath.Replace(TEXT('\\'), TEXT('/'));
    if (relativePath.IsEmpty() || relativePath.Contains(TEXT(":")) || !AssetPathPolicy::IsPackageEntryPathValid(PackageEntryPath(relativePath)))
    {
        relativePath.Clear();
        return PathFail(diagnostic, absolutePath, TEXT("Artifact path cannot be represented safely relative to Library."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ArtifactStore::TryResolveLibraryRelative(const StringView& libraryRoot, const StringView& relativePath, ArtifactStoragePath& absolutePath, AssetPipelineDiagnostic& diagnostic)
{
    absolutePath = ArtifactStoragePath();
    String normalized(relativePath);
    normalized.Replace(TEXT('\\'), TEXT('/'));
    if (normalized != relativePath || normalized.Contains(TEXT(":")) || !AssetPathPolicy::IsPackageEntryPathValid(PackageEntryPath(normalized)))
        return PathFail(diagnostic, relativePath, TEXT("Artifact manifest contains an invalid Library-relative path."));
    String candidate = String(libraryRoot) / normalized;
    StringUtils::PathRemoveRelativeParts(candidate);
    candidate.Replace(TEXT('\\'), TEXT('/'));
    return SetStoragePath(libraryRoot, candidate, absolutePath, diagnostic);
}

bool ArtifactStore::EnsureLayout(const StringView& libraryRoot, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (!FileSystem::DirectoryExists(libraryRoot) || IsReparsePoint(libraryRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, libraryRoot, TEXT("Project Library root is missing or is a filesystem link."));

    for (const Char* directory : LayoutDirectories)
    {
        const String path = String(libraryRoot) / directory;
        if ((!FileSystem::DirectoryExists(path) && FileSystem::CreateDirectory(path)) || FileSystem::FileExists(path))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, path, TEXT("Cannot create a required Project Library directory."));
    }

    const String marker = String(libraryRoot) / TEXT("schema.version");
    if (FileSystem::FileExists(marker))
    {
        String version;
        if (File::ReadAllText(marker, version) || !version.StartsWith(TEXT("1")))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, marker, TEXT("Project Library schema marker is invalid or unsupported."));
    }
    else if (File::WriteAllText(marker, TEXT("1\n"), Encoding::ANSI))
    {
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, marker, TEXT("Cannot write the Project Library schema marker."));
    }
    return false;
}

bool ArtifactStore::Recover(const StringView& libraryRoot, AssetPipelineDiagnostic& diagnostic)
{
    if (EnsureLayout(libraryRoot, diagnostic))
        return true;

    int32 recoveredLocks = 0;
    if (ArtifactLock::RecoverAbandoned(libraryRoot, ArtifactLockLivenessProbe(), recoveredLocks, diagnostic))
        return true;

    // Publication uses atomic rename. Only a known empty staging shell is unambiguously abandoned.
    const String interrupted = GetTemporaryPath(libraryRoot) / TEXT("Interrupted");
    if (FileSystem::DirectoryExists(interrupted))
    {
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, interrupted) || files.HasItems())
            return false;
        if (FileSystem::DeleteDirectory(interrupted, false))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, interrupted, TEXT("Cannot remove abandoned empty artifact staging data."));
    }
    return false;
}

bool ArtifactStore::CleanEntireLibrary(AssetPipelineDiagnostic& diagnostic)
{
#if USE_EDITOR
    String normalized;
    if (ProjectLibrary::ValidateRoot(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, normalized, diagnostic))
        return true;
    if (!FileSystem::AreFilePathsEquivalent(normalized, Globals::ProjectLibraryFolder) || IsReparsePoint(normalized))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, normalized, TEXT("Refusing to clean an unconfigured or linked Project Library root."));
    if (ArtifactLease::HasLeaseWithin(normalized))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, normalized, TEXT("Project Library contains artifacts that are currently leased."));

    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, normalized))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, normalized, TEXT("Cannot enumerate Project Library before cleaning."));
    for (const String& file : files)
        ContentStorageManager::EnsureAccess(file);
    if (FileSystem::DeleteDirectory(normalized, true) || FileSystem::CreateDirectory(normalized))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, normalized, TEXT("Cannot clean and recreate Project Library."));
    return EnsureLayout(normalized, diagnostic);
#else
    return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, StringView::Empty, TEXT("Project Library is available only in editor and cooker builds."));
#endif
}

#if USE_EDITOR
class ArtifactStoreService : public EngineService
{
public:
    ArtifactStoreService()
        : EngineService(TEXT("ArtifactStore"), -850)
    {
    }

    bool Init() override
    {
        AssetPipelineDiagnostic diagnostic;
        if (ArtifactStore::Recover(Globals::ProjectLibraryFolder, diagnostic))
        {
            LOG(Error, "{0}: {1} Path: '{2}'.", GetAssetPipelineDiagnosticCodeName(diagnostic.Code), diagnostic.Message, diagnostic.SourcePath);
            return true;
        }
        return false;
    }
};

ArtifactStoreService ArtifactStoreServiceInstance;
#endif
