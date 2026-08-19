// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodCatalogBuilder.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Core/Log.h"
#include "Engine/Audio/FMOD/FmodConvert.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/FileWriteStream.h"

bool FmodCatalogBuilder::BuildCatalog(const String& banksDirectory, const String& outputDirectory)
{
    if (!FileSystem::DirectoryExists(banksDirectory))
    {
        LOG(Warning, "FMOD banks directory '{0}' does not exist.", banksDirectory);
        return false;
    }

    if (!FileSystem::DirectoryExists(outputDirectory))
    {
        FileSystem::CreateDirectory(outputDirectory);
    }

    Array<String> bankFiles;
    FileSystem::DirectoryGetFiles(bankFiles, banksDirectory, TEXT("*.bank"), DirectorySearchOption::AllDirectories);

    LOG(Info, "Found {0} FMOD bank files to catalog.", bankFiles.Count());

    for (int32 i = 0; i < bankFiles.Count(); i++)
    {
        const String& bankPath = bankFiles[i];
        String fileName = String(StringUtils::GetFileNameWithoutExtension(bankPath));

        // Create deterministic bank ID from path hash
        uint32 h1 = GetHash(bankPath);
        uint32 h2 = GetHash(fileName);
        Guid bankId(h1, h2, (uint32)0x50000000, (uint32)0x00000001);
        LOG(Info, "Cataloged bank: {0} ({1})", fileName, bankId.ToString());
    }

    return true;
}
