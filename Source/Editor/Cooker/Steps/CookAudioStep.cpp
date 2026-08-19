// Copyright (c) Wojciech Figat. All rights reserved.

#include "CookAudioStep.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Audio/AudioSettings.h"
#include "Engine/Audio/Events/Assets/AudioBank.h"
#include "Engine/Core/Log.h"

bool CookAudioStep::Perform(CookingData& data)
{
    const auto audioSettings = AudioSettings::Get();
    if (audioSettings->DisableAudio)
    {
        LOG(Info, "Audio is disabled in project settings. Skipping audio cooking step.");
        return false;
    }

    LOG(Info, "Cooking sound banks and audio data for platform...");

    // Target audio output directory
    const String outputAudioDir = data.DataOutputPath / TEXT("Audio");
    if (!FileSystem::DirectoryExists(outputAudioDir))
    {
        FileSystem::CreateDirectory(outputAudioDir);
    }

    // Check project FMOD / sound banks folder
    const String projectBanksDir = Globals::ProjectFolder / TEXT("Content") / TEXT("Audio") / TEXT("Banks");
    if (FileSystem::DirectoryExists(projectBanksDir))
    {
        Array<String> bankFiles;
        FileSystem::DirectoryGetFiles(bankFiles, projectBanksDir, TEXT("*.bank"), DirectorySearchOption::AllDirectories);

        for (int32 i = 0; i < bankFiles.Count(); i++)
        {
            const String& srcPath = bankFiles[i];
            const String fileName = String(StringUtils::GetFileName(srcPath));
            const String dstPath = outputAudioDir / fileName;

            if (FileSystem::CopyFile(dstPath, srcPath))
            {
                data.Error(String::Format(TEXT("Failed to copy bank file '{0}' to '{1}'"), srcPath, dstPath));
                return true;
            }
            LOG(Info, "Deployed sound bank '{0}'", fileName);
        }
    }

    return false;
}
