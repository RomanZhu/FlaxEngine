// Copyright (c) Wojciech Figat. All rights reserved.

#include "CookAudioStep.h"
#include "Engine/Audio/Events/AudioCookManifest.h"
#include "Engine/Audio/AudioSettings.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/FileWriteStream.h"

namespace
{
    uint64 HashBytes(const Array<byte>& bytes)
    {
        uint64 hash = 14695981039346656037ull;
        for (int32 i = 0; i < bytes.Count(); i++)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    String HashFile(const String& path)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return String::Empty;
        return String::Format(TEXT("{0:x16}"), HashBytes(bytes));
    }

    void SortPaths(Array<String>& paths)
    {
        for (int32 i = 1; i < paths.Count(); i++)
        {
            String value = MoveTemp(paths[i]);
            int32 j = i - 1;
            while (j >= 0 && value < paths[j])
            {
                paths[j + 1] = MoveTemp(paths[j]);
                j--;
            }
            paths[j + 1] = MoveTemp(value);
        }
    }

    String RelativePath(const String& root, const String& path)
    {
        String result = path;
        if (result.StartsWith(root))
            result = result.Right(result.Length() - root.Length());
        while (result.StartsWith(TEXT("/")) || result.StartsWith(TEXT("\\")))
            result = result.Right(result.Length() - 1);
        result.Replace(TEXT("\\"), TEXT("/"));
        return result;
    }

    bool ValidateMetadata(const String& banksDir, const String& metadataPath, const Array<String>& bankFiles, String& revision)
    {
        StringAnsi metadata;
        if (File::ReadAllText(metadataPath, metadata))
            return false;

        rapidjson_flax::Document document;
        document.Parse(metadata.Get(), metadata.Length());
        if (document.HasParseError() || !document.IsObject())
        {
            LOG(Error, "FMOD metadata '{0}' is not valid JSON.", metadataPath);
            return true;
        }
        const auto schema = document.FindMember("schema");
        if (schema == document.MemberEnd() || !schema->value.IsInt() || schema->value.GetInt() < 1)
        {
            LOG(Error, "FMOD metadata '{0}' is missing a supported schema.", metadataPath);
            return true;
        }
        const auto banks = document.FindMember("banks");
        if (banks != document.MemberEnd() && banks->value.IsArray())
        {
            for (auto it = banks->value.Begin(); it != banks->value.End(); ++it)
            {
                if (!it->IsObject())
                    continue;
                const auto file = it->FindMember("file");
                if (file == it->MemberEnd() || !file->value.IsString())
                    continue;
                String expected = String(file->value.GetString());
                expected.Replace(TEXT("\\"), TEXT("/"));
                bool found = false;
                for (const String& bank : bankFiles)
                {
                    if (RelativePath(banksDir, bank) == expected)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    LOG(Error, "FMOD metadata references missing bank '{0}'.", expected);
                    return true;
                }
            }
        }
        revision = HashFile(metadataPath);
        return revision.IsEmpty();
    }

    bool WriteManifest(const String& path, const AudioCookManifest& manifest)
    {
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        const auto writeString = [&writer](const String& value)
        {
            const StringAsUTF8<256> utf8(*value, value.Length());
            writer.String(utf8.Get(), utf8.Length());
        };
        writer.StartObject();
        writer.JKEY("schema"); writer.Int(manifest.Schema);
        writer.JKEY("metadataRevision"); writeString(manifest.MetadataRevision);
        writer.JKEY("platform"); writeString(manifest.Platform);
        writer.JKEY("locale"); writeString(manifest.Locale);
        writer.JKEY("banks"); writer.StartArray();
        for (const auto& bank : manifest.Banks)
        {
            writer.StartObject();
            writer.JKEY("id"); writer.Guid(bank.ID);
            writer.JKEY("file"); writeString(bank.File);
            writer.JKEY("hash"); writeString(bank.Hash);
            writer.JKEY("size"); writer.Uint64(bank.Size);
            writer.EndObject();
        }
        writer.EndArray(manifest.Banks.Count());
        writer.EndObject();
        FileWriteStream* stream = FileWriteStream::Open(path);
        if (!stream)
            return true;
        stream->WriteBytes(buffer.GetString(), buffer.GetSize());
        stream->Close();
        Delete(stream);
        return false;
    }
}

bool CookAudioStep::Perform(CookingData& data)
{
    const auto audioSettings = AudioSettings::Get();
    if (audioSettings->DisableAudio)
    {
        LOG(Info, "Audio is disabled in project settings. Skipping audio cooking step.");
        return false;
    }

    const String projectBanksDir = Globals::ProjectFolder / TEXT("Content") / TEXT("Audio") / TEXT("Banks");
    const String outputAudioDir = data.DataOutputPath / TEXT("Audio");
    if (!FileSystem::DirectoryExists(outputAudioDir) && FileSystem::CreateDirectory(outputAudioDir))
    {
        data.Error(String::Format(TEXT("Failed to create audio output directory '{0}'."), outputAudioDir));
        return true;
    }

    AudioCookManifest manifest;
    const Char* platformName;
    const Char* architecture;
    data.GetBuildPlatformName(platformName, architecture);
    manifest.Platform = platformName;

    if (!FileSystem::DirectoryExists(projectBanksDir))
    {
        LOG(Info, "No FMOD banks directory found at '{0}'. Audio manifest is empty.", projectBanksDir);
        return WriteManifest(outputAudioDir / TEXT("AudioCookManifest.json"), manifest);
    }

    Array<String> bankFiles;
    if (FileSystem::DirectoryGetFiles(bankFiles, projectBanksDir, TEXT("*.bank"), DirectorySearchOption::AllDirectories))
    {
        data.Error(String::Format(TEXT("Failed to enumerate FMOD banks in '{0}'."), projectBanksDir));
        return true;
    }
    SortPaths(bankFiles);

    const String metadataCandidates[] =
    {
        projectBanksDir / TEXT("fmod-metadata.json"),
        projectBanksDir / TEXT("metadata.json"),
        Globals::ProjectFolder / TEXT("Content") / TEXT("Audio") / TEXT("fmod-metadata.json")
    };
    String metadataRevision;
    for (const String& candidate : metadataCandidates)
    {
        if (FileSystem::FileExists(candidate))
        {
            if (ValidateMetadata(projectBanksDir, candidate, bankFiles, metadataRevision))
            {
                data.Error(String::Format(TEXT("FMOD metadata validation failed for '{0}'."), candidate));
                return true;
            }
            break;
        }
    }
    if (metadataRevision.IsEmpty())
        LOG(Warning, "No FMOD metadata sidecar found. Cooking banks in metadata-only fallback mode.");
    manifest.MetadataRevision = metadataRevision;

    for (const String& sourcePath : bankFiles)
    {
        const String relative = RelativePath(projectBanksDir, sourcePath);
        const String destination = outputAudioDir / relative;
        const String destinationDirectory = StringUtils::GetDirectoryName(destination);
        if (!FileSystem::DirectoryExists(destinationDirectory) && FileSystem::CreateDirectory(destinationDirectory))
        {
            data.Error(String::Format(TEXT("Failed to create audio directory '{0}'."), destinationDirectory));
            return true;
        }
        if (FileSystem::CopyFile(destination, sourcePath))
        {
            data.Error(String::Format(TEXT("Failed to copy FMOD bank '{0}'."), sourcePath));
            return true;
        }
        AudioCookManifestBank& bank = manifest.Banks.AddOne();
        bank.File = relative;
        bank.ID = Guid(GetHash(relative), GetHash(sourcePath), 0x41554449, 2);
        bank.Hash = HashFile(sourcePath);
        bank.Size = FileSystem::GetFileSize(sourcePath);
        if (bank.Hash.IsEmpty())
        {
            data.Error(String::Format(TEXT("Failed to hash FMOD bank '{0}'."), sourcePath));
            return true;
        }
    }

    LOG(Info, "Cooked {0} FMOD banks for {1}.", manifest.Banks.Count(), manifest.Platform);
    if (WriteManifest(outputAudioDir / TEXT("AudioCookManifest.json"), manifest))
    {
        data.Error(TEXT("Failed to write AudioCookManifest.json."));
        return true;
    }
    return false;
}
