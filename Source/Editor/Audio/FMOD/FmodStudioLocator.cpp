// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodStudioLocator.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Core/Collections/HashFunctions.h"

namespace
{
    String GetSettingsPath()
    {
        String appData;
        FileSystem::GetSpecialFolderPath(SpecialFolder::LocalAppData, appData);
        const uint32 projectHash = GetHash(Globals::ProjectFolder);
        const String directory = appData / TEXT("Flax") / TEXT("FMOD");
        if (!FileSystem::DirectoryExists(directory))
            FileSystem::CreateDirectory(directory);
        return directory / String::Format(TEXT("{0:x8}.fspro"), projectHash);
    }
}

String FmodStudioLocator::GetUserProjectPath()
{
    String path;
    const String settingsPath = GetSettingsPath();
    if (FileSystem::FileExists(settingsPath) && !File::ReadAllText(settingsPath, path))
    {
        path = path.TrimTrailing();
        if (path.HasChars() && FileSystem::FileExists(path))
            return path;
    }
    return String::Empty;
}

bool FmodStudioLocator::SetUserProjectPath(const StringView& path)
{
    String value(path);
    FileSystem::NormalizePath(value);
    value = value.TrimTrailing();
    if (value.HasChars() && !FileSystem::FileExists(value))
        return false;
    return File::WriteAllText(GetSettingsPath(), value, Encoding::UTF8);
}

String FmodStudioLocator::FindStudioExecutable()
{
    const String configured = GetUserProjectPath();
    if (configured.HasChars())
    {
        const String directory = StringUtils::GetDirectoryName(configured);
        const String sibling = directory / TEXT("fmodstudio.exe");
        if (FileSystem::FileExists(sibling))
            return sibling;
    }

#if PLATFORM_WINDOWS
    const String candidates[] =
    {
        TEXT("C:/Program Files/FMOD SoundSystem/FMOD Studio/bin/fmodstudio.exe"),
        TEXT("C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio/bin/fmodstudio.exe")
    };
    for (const String& candidate : candidates)
    {
        if (FileSystem::FileExists(candidate))
            return candidate;
    }
#endif
    return String::Empty;
}
