// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSourceRoots.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

String AssetSourceRoots::GetEngineRoot()
{
    return Globals::StartupFolder / TEXT("Source/Editor/Assets");
}

void AssetSourceRoots::Resolve(const StringView& sourcePath, String& projectRoot, String& contentRoot)
{
    AssetSourceRootRegistry registry(Globals::ProjectFolder, Globals::ProjectLibraryFolder);
    AssetPipelineDiagnostic diagnostic;
    if (!registry.RegisterProjectRoots(Globals::ProjectContentFolder, diagnostic) &&
        !registry.RegisterEngineRoot(Globals::StartupFolder, GetEngineRoot(), diagnostic))
    {
        ResolvedAssetSourcePath resolved;
        if (!registry.Resolve(sourcePath, resolved, diagnostic))
        {
            projectRoot = resolved.Root.OwnerPath;
            contentRoot = resolved.Root.PhysicalPath;
            return;
        }
    }
    projectRoot = Globals::ProjectFolder;
    contentRoot = Globals::ProjectContentFolder;
}

AssetSourceRootRegistry AssetSourceRoots::CreateScannerRegistry(const StringView& projectRoot,
    const StringView& sourceRoot, const StringView& libraryRoot, AssetPipelineDiagnostic& diagnostic)
{
    AssetSourceRootRegistry registry(projectRoot, libraryRoot);
    String normalizedProject(projectRoot);
    String normalizedSource(sourceRoot);
    FileSystem::NormalizePath(normalizedProject);
    FileSystem::NormalizePath(normalizedSource);
    const String externalActors = normalizedProject / TEXT("ExternalActors");
    const String defaultContent = normalizedProject / TEXT("Content");
    if (AssetPathPolicy::IsSameOrChild(normalizedSource, externalActors))
    {
        registry.RegisterProjectRoots(defaultContent, diagnostic);
        return registry;
    }
    const String engineRoot = GetEngineRoot();
    if (FileSystem::AreFilePathsEqual(normalizedSource, engineRoot))
    {
        registry.RegisterEngineRoot(projectRoot, sourceRoot, diagnostic);
        return registry;
    }
    const String sourceName = StringUtils::GetFileName(normalizedSource);
    if (sourceName.Compare(String(TEXT("Content")), StringSearchCase::IgnoreCase) == 0)
    {
        registry.RegisterProjectRoots(sourceRoot, diagnostic);
        return registry;
    }
    const String logicalPrefix = sourceName.HasChars() ? sourceName : TEXT("ExternalContent");
    registry.RegisterExternalReadOnlyRoot(Guid(0x434f4d50u, 0x41545343u, 0x414e4e45u, 0x52000001u),
        TEXT("compatibility-scanner"), sourceRoot, logicalPrefix, diagnostic);
    return registry;
}
