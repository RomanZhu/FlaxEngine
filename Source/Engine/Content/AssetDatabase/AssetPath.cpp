// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetPath.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Core/Collections/Dictionary.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    String NormalizeAbsoluteSemanticPath(const StringView& value)
    {
        String result(value);
        StringUtils::PathRemoveRelativeParts(result);
        return result;
    }

    String NormalizeLogicalSemanticPath(const StringView& value)
    {
        String result(value);
        result.Replace('\\', '/');
        return result;
    }

    bool IsUnicodePathValid(const StringView& path)
    {
        for (int32 i = 0; i < path.Length(); i++)
        {
            const Char c = path[i];
            if (c < 0x20)
                return false;
            if (c >= 0xd800 && c <= 0xdbff)
            {
                if (++i >= path.Length() || path[i] < 0xdc00 || path[i] > 0xdfff)
                    return false;
            }
            else if (c >= 0xdc00 && c <= 0xdfff)
            {
                return false;
            }
        }
        return true;
    }

    bool IsReservedSegment(const StringView& segment)
    {
        if (segment.IsEmpty() || segment.EndsWith(' ') || segment.EndsWith('.'))
            return true;
        String base(segment);
        const int32 dot = base.Find('.');
        if (dot != -1)
            base.Resize(dot);
        base = base.ToLower();
        if (base == TEXT("con") || base == TEXT("prn") || base == TEXT("aux") || base == TEXT("nul"))
            return true;
        if (base.Length() == 4 && (base.StartsWith(TEXT("com")) || base.StartsWith(TEXT("lpt"))) && base[3] >= '1' && base[3] <= '9')
            return true;
        return false;
    }

    bool HasReservedSegment(const StringView& relativePath)
    {
        String normalized(relativePath);
        normalized.Replace((Char)92, '/');
        Array<String> segments;
        normalized.Split('/', segments);
        for (const String& segment : segments)
        {
            if (IsReservedSegment(segment))
                return true;
        }
        return false;
    }

    bool ResolveExistingPath(const StringView& input, String& resolved)
    {
#if PLATFORM_WINDOWS
        String existing(input);
        while (!FileSystem::FileExists(existing) && !FileSystem::DirectoryExists(existing))
        {
            const StringView parent = StringUtils::GetDirectoryName(existing);
            if (parent.IsEmpty() || parent == existing)
                return true;
            existing = parent;
        }
        HANDLE handle = CreateFileW(*existing, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        Char buffer[4096];
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer, ARRAY_COUNT(buffer), FILE_NAME_NORMALIZED);
        CloseHandle(handle);
        if (length == 0 || length >= ARRAY_COUNT(buffer))
            return true;
        resolved = String(buffer, (int32)length);
        if (resolved.StartsWith(TEXT("\\\\?\\")))
            resolved = resolved.Substring(4);
        return false;
#else
        resolved = input;
        return false;
#endif
    }

    bool PathFail(AssetPipelineDiagnostic& diagnostic, const StringView& input, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = input;
        diagnostic.Message = message;
        return true;
    }
}

CanonicalAssetPath::CanonicalAssetPath(const StringView& value)
    : _value(NormalizeAbsoluteSemanticPath(value))
{
}

SourceFilePath::SourceFilePath(const StringView& value)
    : _value(NormalizeAbsoluteSemanticPath(value))
{
}

MetaFilePath::MetaFilePath(const StringView& value)
    : _value(NormalizeAbsoluteSemanticPath(value))
{
}

ArtifactStoragePath::ArtifactStoragePath(const StringView& value)
    : _value(NormalizeAbsoluteSemanticPath(value))
{
}

PackageEntryPath::PackageEntryPath(const StringView& value)
    : _value(NormalizeLogicalSemanticPath(value))
{
}

SubAssetKey::SubAssetKey(const StringView& value)
    : _value(value)
{
}

const Char* AssetPathPolicy::GetDebugLabel(AssetPathKind kind)
{
    switch (kind)
    {
    case AssetPathKind::Canonical: return TEXT("canonical");
    case AssetPathKind::Source: return TEXT("source");
    case AssetPathKind::Meta: return TEXT("meta");
    case AssetPathKind::ArtifactStorage: return TEXT("artifact-storage");
    case AssetPathKind::PackageEntry: return TEXT("package-entry");
    case AssetPathKind::SubAsset: return TEXT("subasset-key");
    default: return TEXT("unknown");
    }
}

bool AssetPathPolicy::IsSameOrChild(const StringView& path, const StringView& root)
{
    if (path.Compare(root, StringSearchCase::IgnoreCase) == 0)
        return true;
    if (path.Length() <= root.Length() || !path.StartsWith(root, StringSearchCase::IgnoreCase))
        return false;
    const Char separator = path[root.Length()];
    return separator == '/' || separator == '\\';
}

bool AssetPathPolicy::IsCanonicalPathValid(const CanonicalAssetPath& path, const StringView& contentRoot)
{
    return !path.IsEmpty() && !FileSystem::IsRelative(path.Get()) && IsSameOrChild(path.Get(), contentRoot);
}

bool AssetPathPolicy::IsSourcePathValid(const SourceFilePath& path, const StringView& contentRoot)
{
    return !path.IsEmpty() && !FileSystem::IsRelative(path.Get()) && IsSameOrChild(path.Get(), contentRoot);
}

bool AssetPathPolicy::IsMetaPathValid(const MetaFilePath& path, const StringView& contentRoot)
{
    return !path.IsEmpty() && !FileSystem::IsRelative(path.Get()) && IsSameOrChild(path.Get(), contentRoot) && path.Get().EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
}

bool AssetPathPolicy::IsArtifactPathValid(const ArtifactStoragePath& path, const StringView& libraryRoot)
{
    return !path.IsEmpty() && !FileSystem::IsRelative(path.Get()) && IsSameOrChild(path.Get(), libraryRoot);
}

bool AssetPathPolicy::IsPackageEntryPathValid(const PackageEntryPath& path)
{
    if (path.IsEmpty() || path.Get() == TEXT("..") || !FileSystem::IsRelative(path.Get()) || path.Get().StartsWith('/') || path.Get().Contains(TEXT("../")) || path.Get().EndsWith(TEXT("/..")))
        return false;
    return true;
}

bool AssetPathPolicy::TryNormalizeProjectPath(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const StringView& input, ProjectPath& result, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    result = ProjectPath();
    if (projectRoot.IsEmpty() || contentRoot.IsEmpty() || input.IsEmpty() || !IsUnicodePathValid(input))
        return PathFail(diagnostic, input, TEXT("Project asset path is empty or contains invalid Unicode/control characters."));

    result.DisplayPath = input;
    result.AbsolutePath = FileSystem::IsRelative(input) ? String(projectRoot) / input : String(input);
    StringUtils::PathRemoveRelativeParts(result.AbsolutePath);
    result.AbsolutePath.Replace((Char)92, '/');
    String project(projectRoot);
    String content(contentRoot);
    String library(libraryRoot);
    StringUtils::PathRemoveRelativeParts(project);
    StringUtils::PathRemoveRelativeParts(content);
    StringUtils::PathRemoveRelativeParts(library);
    project.Replace((Char)92, '/');
    content.Replace((Char)92, '/');
    library.Replace((Char)92, '/');
    if (!IsSameOrChild(result.AbsolutePath, content) || IsSameOrChild(result.AbsolutePath, library))
        return PathFail(diagnostic, input, TEXT("Project asset path must remain under Content and outside Library."));
    result.ProjectRelativePath = FileSystem::ConvertAbsolutePathToRelative(project, result.AbsolutePath);
    result.ProjectRelativePath.Replace((Char)92, '/');
    if (result.ProjectRelativePath.Contains(TEXT("../")) || HasReservedSegment(result.ProjectRelativePath))
        return PathFail(diagnostic, input, TEXT("Project asset path contains traversal, a reserved name, or a non-portable trailing character."));

    String resolvedContent;
    String resolvedCandidate;
    if (ResolveExistingPath(content, resolvedContent) || ResolveExistingPath(result.AbsolutePath, resolvedCandidate) || !IsSameOrChild(resolvedCandidate, resolvedContent))
        return PathFail(diagnostic, input, TEXT("Project asset path escapes Content through a filesystem link or cannot be resolved safely."));
    result.PortabilityKey = result.ProjectRelativePath.ToLower();
    return false;
}

void AssetPathPolicy::FindPortabilityCollisions(const Array<ProjectPath>& paths, Array<AssetPipelineDiagnostic>& diagnostics)
{
    Dictionary<String, String> seen;
    for (const ProjectPath& path : paths)
    {
        String* previous = seen.TryGet(path.PortabilityKey);
        if (!previous)
        {
            seen.Add(path.PortabilityKey, path.AbsolutePath);
            continue;
        }
        if (*previous == path.AbsolutePath)
            continue;
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path.AbsolutePath;
        diagnostic.Message = TEXT("Project asset paths collide under the portable case/separator policy.");
        diagnostic.Related.Add(*previous);
        diagnostic.Related.Add(path.AbsolutePath);
        diagnostics.Add(MoveTemp(diagnostic));
    }
}
