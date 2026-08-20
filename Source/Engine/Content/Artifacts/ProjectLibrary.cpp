// Copyright (c) Wojciech Figat. All rights reserved.

#include "ProjectLibrary.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

namespace
{
    void NormalizeRoot(String& path)
    {
        StringUtils::PathRemoveRelativeParts(path);
        while (path.Length() > 1 && (path[path.Length() - 1] == '/' || path[path.Length() - 1] == '\\'))
            path.Resize(path.Length() - 1);
    }

    bool IsSameOrChildPath(const StringView& path, const StringView& root)
    {
        if (path.Compare(root, StringSearchCase::IgnoreCase) == 0)
            return true;
        if (path.Length() <= root.Length() || !path.StartsWith(root, StringSearchCase::IgnoreCase))
            return false;
        const Char separator = path[root.Length()];
        return separator == '/' || separator == '\\';
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }
}

bool ProjectLibrary::ValidateRoot(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, String& normalizedRoot, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    String project(projectRoot);
    String content(contentRoot);
    normalizedRoot = libraryRoot;
    NormalizeRoot(project);
    NormalizeRoot(content);
    NormalizeRoot(normalizedRoot);

    if (project.IsEmpty() || content.IsEmpty() || normalizedRoot.IsEmpty() || FileSystem::IsRelative(project) || FileSystem::IsRelative(content) || FileSystem::IsRelative(normalizedRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, normalizedRoot, TEXT("Project Library paths must be absolute."));
    if (!IsSameOrChildPath(content, project) || !IsSameOrChildPath(normalizedRoot, project) || FileSystem::AreFilePathsEquivalent(normalizedRoot, project))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, normalizedRoot, TEXT("Project Library must be a dedicated directory inside the project root."));
    if (IsSameOrChildPath(normalizedRoot, content) || IsSameOrChildPath(content, normalizedRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryPathInvalid, normalizedRoot, TEXT("Project Library and Content roots must not overlap."));
    return false;
}

bool ProjectLibrary::EnsureRoot(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, String& normalizedRoot, AssetPipelineDiagnostic& diagnostic)
{
    if (ValidateRoot(projectRoot, contentRoot, libraryRoot, normalizedRoot, diagnostic))
        return true;
    if (FileSystem::DirectoryExists(normalizedRoot))
        return false;
    if (FileSystem::FileExists(normalizedRoot) || FileSystem::CreateDirectory(normalizedRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, normalizedRoot, TEXT("Cannot create the project Library directory. Check project directory permissions."));
    return false;
}
