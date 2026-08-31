// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSourceRoots.h"
#include "AssetMount.h"
#include "AssetPath.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"

String AssetSourceRoots::GetEngineRoot()
{
    return Globals::StartupFolder / TEXT("Source/Editor/Assets");
}

void AssetSourceRoots::Resolve(const StringView& sourcePath, String& projectRoot, String& contentRoot)
{
    AssetMountResolution resolution;
    AssetPipelineDiagnostic diagnostic;
    if (!AssetMountRegistry::Get().ResolvePhysical(sourcePath, resolution, diagnostic))
    {
        contentRoot = resolution.Mount.PhysicalRoot;
        if (resolution.Mount.Kind == AssetMountKind::ProjectContent)
            projectRoot = Globals::ProjectFolder;
        else if (FileSystem::AreFilePathsEquivalent(contentRoot, GetEngineRoot()))
            projectRoot = Globals::StartupFolder;
        else
            projectRoot = StringUtils::GetDirectoryName(contentRoot);
        return;
    }
    const String engineRoot = GetEngineRoot();
    if (AssetPathPolicy::IsSameOrChild(sourcePath, engineRoot))
    {
        projectRoot = Globals::StartupFolder;
        contentRoot = engineRoot;
    }
    else
    {
        projectRoot = Globals::ProjectFolder;
        contentRoot = Globals::ProjectContentFolder;
    }
}
