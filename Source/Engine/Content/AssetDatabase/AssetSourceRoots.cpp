// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSourceRoots.h"
#include "AssetPath.h"
#include "Engine/Engine/Globals.h"

String AssetSourceRoots::GetEngineRoot()
{
    return Globals::StartupFolder / TEXT("Source/Editor/Assets");
}

void AssetSourceRoots::Resolve(const StringView& sourcePath, String& projectRoot, String& contentRoot)
{
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
