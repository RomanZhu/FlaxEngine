// Copyright (c) Wojciech Figat. All rights reserved.

#include "CookAudioStep.h"
#include "Engine/Audio/Events/AudioCookManifest.h"
#include "Engine/Audio/AudioSettings.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/FileWriteStream.h"

namespace
{
    struct CookedFile
    {
        String File;
        String Hash;
        uint64 Size = 0;
    };

    struct MetadataBank
    {
        Guid ID = Guid::Empty;
        String File;
        Array<Guid> Events;
        Array<Guid> Dependencies;
    };

    bool ContainsGuid(const Array<Guid>& values, const Guid& value)
    {
        for (const Guid& item : values)
            if (item == value)
                return true;
        return false;
    }

    const MetadataBank* FindBank(const Array<MetadataBank>& banks, const Guid& id)
    {
        for (const MetadataBank& bank : banks)
            if (bank.ID == id)
                return &bank;
        return nullptr;
    }

    bool HasDependencyCycle(const Array<MetadataBank>& banks, const Guid& id, Array<Guid>& visiting, Array<Guid>& visited)
    {
        if (visited.Contains(id))
            return false;
        if (visiting.Contains(id))
            return true;
        visiting.Add(id);
        const MetadataBank* bank = FindBank(banks, id);
        if (bank)
        {
            for (const Guid& dependency : bank->Dependencies)
                if (HasDependencyCycle(banks, dependency, visiting, visited))
                    return true;
        }
        visiting.Remove(id);
        visited.Add(id);
        return false;
    }

    const MetadataBank* FindBankByFile(const Array<MetadataBank>& banks, const String& file)
    {
        for (const MetadataBank& bank : banks)
            if (bank.File == file)
                return &bank;
        return nullptr;
    }

    const MetadataBank* FindLocalizedBankByFile(const Array<MetadataBank>& banks, const String& file, String& language)
    {
        language.Clear();
        const String sourceDirectory(StringUtils::GetDirectoryName(file));
        const String sourceStem(StringUtils::GetFileNameWithoutExtension(file));
        const int32 sourceSeparator = sourceStem.FindLast('_');
        if (sourceSeparator < 0 || sourceStem.Length() - sourceSeparator - 1 != 2)
            return nullptr;
        language = sourceStem.Substring(sourceSeparator + 1).ToUpper();
        const String sourceBase = sourceStem.Left(sourceSeparator);
        for (const MetadataBank& bank : banks)
        {
            if (String(StringUtils::GetDirectoryName(bank.File)) != sourceDirectory)
                continue;
            const String metadataStem(StringUtils::GetFileNameWithoutExtension(bank.File));
            const int32 metadataSeparator = metadataStem.FindLast('_');
            const String metadataBase = metadataSeparator >= 0 && metadataStem.Length() - metadataSeparator - 1 == 2
                ? metadataStem.Left(metadataSeparator)
                : metadataStem;
            if (metadataBase == sourceBase)
                return &bank;
        }
        return nullptr;
    }

    bool HasEvent(const Array<MetadataBank>& banks, const Guid& eventId)
    {
        for (const MetadataBank& bank : banks)
            if (ContainsGuid(bank.Events, eventId))
                return true;
        return false;
    }

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
        return String::Format(TEXT("{0:016x}"), HashBytes(bytes));
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

    bool ValidateMetadata(const String& banksDir, const String& metadataPath, const Array<String>& bankFiles, String& revision, Array<MetadataBank>& metadataBanks)
    {
        StringAnsi metadata;
        if (File::ReadAllText(metadataPath, metadata))
        {
            LOG(Error, "FMOD metadata '{0}' could not be read.", metadataPath);
            return true;
        }

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
        if (banks == document.MemberEnd() || !banks->value.IsArray() || banks->value.Empty())
        {
            LOG(Error, "FMOD metadata '{0}' has no bank entries.", metadataPath);
            return true;
        }
        const String metadataDir = StringUtils::GetDirectoryName(metadataPath);
        for (auto it = banks->value.Begin(); it != banks->value.End(); ++it)
        {
            if (!it->IsObject())
            {
                LOG(Error, "FMOD metadata '{0}' contains a non-object bank entry.", metadataPath);
                return true;
            }
            const auto id = it->FindMember("id");
            const auto file = it->FindMember("file");
            if (id == it->MemberEnd() || !id->value.IsString() || file == it->MemberEnd() || !file->value.IsString())
            {
                LOG(Error, "FMOD metadata '{0}' has a bank without id/file.", metadataPath);
                return true;
            }
            MetadataBank& metadataBank = metadataBanks.AddOne();
            metadataBank.File = String(file->value.GetString());
            metadataBank.File.Replace(TEXT("\\"), TEXT("/"));
            if (Guid::Parse(String(id->value.GetString()), metadataBank.ID) || !metadataBank.ID.IsValid())
            {
                LOG(Error, "FMOD metadata '{0}' has an invalid bank id.", metadataPath);
                return true;
            }
            if (FindBank(metadataBanks, metadataBank.ID) != &metadataBank || FindBankByFile(metadataBanks, metadataBank.File) != &metadataBank)
            {
                LOG(Error, "FMOD metadata '{0}' contains duplicate bank id '{1}'.", metadataPath, metadataBank.ID);
                return true;
            }
            bool found = false;
            for (const String& bank : bankFiles)
            {
                const String rootRelative = RelativePath(banksDir, bank);
                const String metadataRelative = RelativePath(metadataDir, bank);
                if (rootRelative == metadataBank.File || metadataRelative == metadataBank.File)
                {
                    metadataBank.File = rootRelative;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                LOG(Error, "FMOD metadata references missing bank '{0}'.", metadataBank.File);
                return true;
            }
            const auto events = it->FindMember("events");
            if (events != it->MemberEnd())
            {
                if (!events->value.IsArray())
                {
                    LOG(Error, "FMOD metadata bank '{0}' has invalid events.", metadataBank.File);
                    return true;
                }
                for (auto event = events->value.Begin(); event != events->value.End(); ++event)
                {
                    String eventId;
                    if (event->IsString())
                        eventId = String(event->GetString());
                    else if (event->IsObject())
                    {
                        const auto eventIdMember = event->FindMember("id");
                        if (eventIdMember != event->MemberEnd() && eventIdMember->value.IsString())
                            eventId = String(eventIdMember->value.GetString());
                    }
                    Guid parsedEvent;
                    if (eventId.IsEmpty() || Guid::Parse(eventId, parsedEvent) || !parsedEvent.IsValid() || HasEvent(metadataBanks, parsedEvent))
                    {
                        LOG(Error, "FMOD metadata bank '{0}' has an invalid or duplicate event id.", metadataBank.File);
                        return true;
                    }
                    metadataBank.Events.Add(parsedEvent);
                }
            }
            const auto dependencies = it->FindMember("dependencies");
            if (dependencies != it->MemberEnd())
            {
                if (!dependencies->value.IsArray())
                {
                    LOG(Error, "FMOD metadata bank '{0}' has invalid dependencies.", metadataBank.File);
                    return true;
                }
                for (auto dependency = dependencies->value.Begin(); dependency != dependencies->value.End(); ++dependency)
                {
                    Guid dependencyId;
                    if (!dependency->IsString() || Guid::Parse(String(dependency->GetString()), dependencyId) || !dependencyId.IsValid() || ContainsGuid(metadataBank.Dependencies, dependencyId))
                    {
                        LOG(Error, "FMOD metadata bank '{0}' has an invalid dependency id.", metadataBank.File);
                        return true;
                    }
                    metadataBank.Dependencies.Add(dependencyId);
                }
            }
        }
        for (const MetadataBank& bank : metadataBanks)
        {
            for (const Guid& dependency : bank.Dependencies)
            {
                if (!FindBank(metadataBanks, dependency))
                {
                    LOG(Error, "FMOD metadata bank '{0}' references missing dependency '{1}'.", bank.File, dependency);
                    return true;
                }
            }
        }
        Array<Guid> visiting;
        Array<Guid> visited;
        for (const MetadataBank& bank : metadataBanks)
        {
            if (HasDependencyCycle(metadataBanks, bank.ID, visiting, visited))
            {
                LOG(Error, "FMOD metadata contains a bank dependency cycle involving '{0}'.", bank.File);
                return true;
            }
        }
        revision = HashFile(metadataPath);
        return revision.IsEmpty();
    }

    bool ValidateConfiguredBank(const JsonAssetReference<AudioBank>& reference, const Char* label, const Array<MetadataBank>& metadataBanks)
    {
        if (!reference)
            return false;
        if (reference->WaitForLoaded())
        {
            LOG(Error, "Configured FMOD {0} bank could not be loaded.", label);
            return true;
        }
        const AudioBank* configured = reference.GetInstance();
        if (!configured || !configured->BackendId.IsValid())
        {
            LOG(Error, "Configured FMOD {0} bank has no valid backend id.", label);
            return true;
        }
        const MetadataBank* metadata = FindBank(metadataBanks, configured->BackendId);
        if (!metadata)
        {
            LOG(Error, "Configured FMOD {0} bank '{1}' is stale or missing from metadata.", label, configured->Path);
            return true;
        }
        String configuredPath = configured->Path;
        configuredPath.Replace(TEXT("\\"), TEXT("/"));
        if (configuredPath.HasChars() && configuredPath != metadata->File)
        {
            LOG(Error, "Configured FMOD {0} bank path '{1}' does not match metadata '{2}'.", label, configuredPath, metadata->File);
            return true;
        }
        for (const Guid& eventId : configured->IncludedEvents)
        {
            if (!HasEvent(metadataBanks, eventId))
            {
                LOG(Error, "Configured FMOD {0} bank references missing event '{1}'.", label, eventId);
                return true;
            }
        }
        for (const Guid& dependency : configured->ReferencedBanks)
        {
            if (!FindBank(metadataBanks, dependency))
            {
                LOG(Error, "Configured FMOD {0} bank references missing dependency '{1}'.", label, dependency);
                return true;
            }
        }
        return false;
    }

    bool ValidateEventAssets(const String& eventsDir, const Array<MetadataBank>& metadataBanks, CookingData& data)
    {
        Array<String> eventFiles;
        if (!FileSystem::DirectoryExists(eventsDir))
        {
            for (const MetadataBank& bank : metadataBanks)
            {
                if (bank.Events.Count() > 0)
                {
                    data.Error(String::Format(TEXT("FMOD metadata references events, but the Audio/Events directory '{0}' is missing."), eventsDir));
                    return true;
                }
            }
            return false;
        }
        if (FileSystem::DirectoryGetFiles(eventFiles, eventsDir, TEXT("*.json"), DirectorySearchOption::AllDirectories))
        {
            data.Error(String::Format(TEXT("Failed to enumerate FMOD event assets in '{0}'."), eventsDir));
            return true;
        }
        SortPaths(eventFiles);
        Dictionary<Guid, Array<Guid>> assets;
        Array<Guid> assetIds;
        for (const String& path : eventFiles)
        {
            StringAnsi text;
            if (File::ReadAllText(path, text))
            {
                data.Error(String::Format(TEXT("FMOD event asset '{0}' could not be read."), path));
                return true;
            }
            rapidjson_flax::Document document;
            document.Parse(text.Get(), text.Length());
            if (document.HasParseError() || !document.IsObject())
                continue;
            const auto type = document.FindMember("TypeName");
            if (type == document.MemberEnd() || !type->value.IsString() || String(type->value.GetString()) != TEXT("FlaxEngine.AudioEvent"))
                continue;
            const auto dataMember = document.FindMember("Data");
            if (dataMember == document.MemberEnd() || !dataMember->value.IsObject())
            {
                data.Error(String::Format(TEXT("FMOD event asset '{0}' has no Data object."), path));
                return true;
            }
            const auto backendId = dataMember->value.FindMember("BackendId");
            Guid eventId;
            if (backendId == dataMember->value.MemberEnd() || !backendId->value.IsString() || Guid::Parse(String(backendId->value.GetString()), eventId) || !eventId.IsValid())
            {
                data.Error(String::Format(TEXT("FMOD event asset '{0}' has an invalid BackendId."), path));
                return true;
            }
            if (assets.ContainsKey(eventId))
            {
                data.Error(String::Format(TEXT("FMOD event GUID '{0}' is duplicated in Audio/Events assets."), eventId));
                return true;
            }
            Array<Guid> dependencies;
            const auto bankDependencies = dataMember->value.FindMember("BankDependencies");
            if (bankDependencies != dataMember->value.MemberEnd())
            {
                if (!bankDependencies->value.IsArray())
                {
                    data.Error(String::Format(TEXT("FMOD event asset '{0}' has invalid BankDependencies."), path));
                    return true;
                }
                for (auto dependency = bankDependencies->value.Begin(); dependency != bankDependencies->value.End(); ++dependency)
                {
                    Guid bankId;
                    if (!dependency->IsString() || Guid::Parse(String(dependency->GetString()), bankId) || !bankId.IsValid() || ContainsGuid(dependencies, bankId))
                    {
                        data.Error(String::Format(TEXT("FMOD event asset '{0}' has an invalid or duplicate bank dependency."), path));
                        return true;
                    }
                    if (!FindBank(metadataBanks, bankId))
                    {
                        data.Error(String::Format(TEXT("FMOD event asset '{0}' references missing bank '{1}'."), path, bankId));
                        return true;
                    }
                    dependencies.Add(bankId);
                }
            }
            assets.Add(eventId, MoveTemp(dependencies));
            assetIds.Add(eventId);
        }
        for (const MetadataBank& bank : metadataBanks)
        {
            for (const Guid& eventId : bank.Events)
            {
                if (!assets.ContainsKey(eventId))
                {
                    data.Error(String::Format(TEXT("FMOD metadata event '{0}' has no synchronized AudioEvent asset."), eventId));
                    return true;
                }
                if (!ContainsGuid(assets[eventId], bank.ID))
                {
                    data.Error(String::Format(TEXT("FMOD AudioEvent asset '{0}' does not declare owning bank '{1}' in BankDependencies."), eventId, bank.ID));
                    return true;
                }
            }
        }
        for (const Guid& eventId : assetIds)
        {
            if (!HasEvent(metadataBanks, eventId))
            {
                data.Error(String::Format(TEXT("Synchronized AudioEvent asset '{0}' is stale or missing from FMOD metadata."), eventId));
                return true;
            }
        }
        return false;
    }

    bool IsRuntimeArtifact(const String& path, bool plugin, const Array<String>& pluginAllowlist)
    {
        const String extension = FileSystem::GetExtension(path).ToLower();
        if (extension != TEXT("dll") && extension != TEXT("so") && extension != TEXT("dylib") &&
            extension != TEXT("bundle") && extension != TEXT("aar") && extension != TEXT("apk"))
            return false;
        if (!plugin)
            return true;
        if (pluginAllowlist.IsEmpty())
            return false;
        String fileName = String(StringUtils::GetFileNameWithoutExtension(path)).ToLower();
        String fullName = String(StringUtils::GetFileName(path)).ToLower();
        for (const String& allowed : pluginAllowlist)
        {
            String normalized = allowed.TrimTrailing().ToLower();
            if (normalized == fileName || normalized == fullName)
                return true;
        }
        return false;
    }

    bool CopyArtifacts(const String& sourceDir, const String& outputDir, bool plugin, const Array<String>& pluginAllowlist, Array<CookedFile>& artifacts, CookingData& data)
    {
        if (!FileSystem::DirectoryExists(sourceDir))
            return false;
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, sourceDir, TEXT("*"), DirectorySearchOption::AllDirectories))
        {
            data.Error(String::Format(TEXT("Failed to enumerate FMOD runtime files in '{0}'."), sourceDir));
            return true;
        }
        SortPaths(files);
        for (const String& sourcePath : files)
        {
            if (!IsRuntimeArtifact(sourcePath, plugin, pluginAllowlist))
                continue;
            const String relative = RelativePath(sourceDir, sourcePath);
            const String destination = outputDir / relative;
            const String destinationDirectory = StringUtils::GetDirectoryName(destination);
            if (!FileSystem::DirectoryExists(destinationDirectory) && FileSystem::CreateDirectory(destinationDirectory))
            {
                data.Error(String::Format(TEXT("Failed to create audio directory '{0}'."), destinationDirectory));
                return true;
            }
            if (FileSystem::CopyFile(destination, sourcePath))
            {
                data.Error(String::Format(TEXT("Failed to copy FMOD runtime file '{0}'."), sourcePath));
                return true;
            }
            CookedFile& artifact = artifacts.AddOne();
            artifact.File = relative;
            artifact.Hash = HashFile(sourcePath);
            artifact.Size = FileSystem::GetFileSize(sourcePath);
            if (artifact.Hash.IsEmpty())
            {
                data.Error(String::Format(TEXT("Failed to hash FMOD runtime file '{0}'."), sourcePath));
                return true;
            }
        }
        return false;
    }

    bool WriteManifest(const String& path, const AudioCookManifest& manifest, const Array<CookedFile>& runtimeFiles, const Array<CookedFile>& pluginFiles)
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
        writer.EndArray((int32)manifest.Banks.Count());
        const auto writeFiles = [&writer, &writeString](const char* key, const Array<CookedFile>& files)
        {
            writer.JKEY(key); writer.StartArray();
            for (const CookedFile& file : files)
            {
                writer.StartObject();
                writer.JKEY("file"); writeString(file.File);
                writer.JKEY("hash"); writeString(file.Hash);
                writer.JKEY("size"); writer.Uint64(file.Size);
                writer.EndObject();
            }
            writer.EndArray((int32)files.Count());
        };
        writeFiles("runtime", runtimeFiles);
        writeFiles("plugins", pluginFiles);
        writer.EndObject();
        FileWriteStream* stream = FileWriteStream::Open(path);
        if (!stream)
            return true;
        stream->WriteBytes(buffer.GetString(), (uint32)buffer.GetSize());
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
    const String projectAudioDir = Globals::ProjectFolder / TEXT("Content") / TEXT("Audio");
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
    manifest.Locale = audioSettings->AudioLocale;
    if (manifest.Locale.IsEmpty())
        manifest.Locale = TEXT("default");

    String selectedBanksDir = projectBanksDir;
    const String platformBanksDir = projectBanksDir / String(platformName);
    if (FileSystem::DirectoryExists(platformBanksDir))
        selectedBanksDir = platformBanksDir;
    const String localeBanksDir = selectedBanksDir / manifest.Locale;
    if (manifest.Locale != TEXT("default") && FileSystem::DirectoryExists(localeBanksDir))
        selectedBanksDir = localeBanksDir;

    Array<CookedFile> runtimeFiles;
    Array<CookedFile> pluginFiles;
    const String runtimeRoot = projectAudioDir / TEXT("Runtime");
    const String pluginRoot = projectAudioDir / TEXT("Plugins");
    const String platformRuntime = runtimeRoot / String(platformName);
    const String platformPlugins = pluginRoot / String(platformName);
    if (CopyArtifacts(FileSystem::DirectoryExists(platformRuntime) ? platformRuntime : runtimeRoot, outputAudioDir / TEXT("Runtime"), false, audioSettings->RuntimePluginAllowlist, runtimeFiles, data) ||
        CopyArtifacts(FileSystem::DirectoryExists(platformPlugins) ? platformPlugins : pluginRoot, outputAudioDir / TEXT("Plugins"), true, audioSettings->RuntimePluginAllowlist, pluginFiles, data))
        return true;

    if (!FileSystem::DirectoryExists(projectBanksDir))
    {
        data.Error(String::Format(TEXT("FMOD banks directory '{0}' is missing; audio cooking cannot produce an empty manifest."), projectBanksDir));
        return true;
    }

    Array<String> bankFiles;
    if (FileSystem::DirectoryGetFiles(bankFiles, selectedBanksDir, TEXT("*.bank"), DirectorySearchOption::AllDirectories))
    {
        data.Error(String::Format(TEXT("Failed to enumerate FMOD banks in '{0}'."), selectedBanksDir));
        return true;
    }
    SortPaths(bankFiles);

    const String metadataCandidates[] =
    {
        selectedBanksDir / TEXT("fmod-metadata.json"),
        selectedBanksDir / TEXT("metadata.json"),
        projectBanksDir / TEXT("fmod-metadata.json"),
        projectBanksDir / TEXT("metadata.json"),
        projectAudioDir / TEXT("fmod-metadata.json")
    };
    String metadataRevision;
    Array<MetadataBank> metadataBanks;
    bool metadataFound = false;
    for (const String& candidate : metadataCandidates)
    {
        if (FileSystem::FileExists(candidate))
        {
            metadataFound = true;
            if (ValidateMetadata(projectBanksDir, candidate, bankFiles, metadataRevision, metadataBanks))
            {
                data.Error(String::Format(TEXT("FMOD metadata validation failed for '{0}'."), candidate));
                return true;
            }
            break;
        }
    }
    if (!metadataFound || metadataRevision.IsEmpty() || metadataBanks.IsEmpty())
    {
        data.Error(TEXT("FMOD banks exist but no valid fmod-metadata.json sidecar was found."));
        return true;
    }
    if (ValidateConfiguredBank(audioSettings->MasterStringsBank, TEXT("master strings"), metadataBanks) ||
        ValidateConfiguredBank(audioSettings->MasterBank, TEXT("master"), metadataBanks))
        return true;
    if (ValidateEventAssets(projectAudioDir / TEXT("Events"), metadataBanks, data))
        return true;
    for (int32 startupIndex = 0; startupIndex < audioSettings->StartupBanks.Count(); startupIndex++)
    {
        if (ValidateConfiguredBank(audioSettings->StartupBanks[startupIndex], TEXT("startup"), metadataBanks))
            return true;
    }
    manifest.MetadataRevision = metadataRevision;

    for (const String& sourcePath : bankFiles)
    {
        // Preserve the platform/locale directory in the cooked output. The runtime
        // manifest and synchronized editor assets therefore use one root-relative path.
        const String relative = RelativePath(projectBanksDir, sourcePath);
        const MetadataBank* metadataBank = FindBankByFile(metadataBanks, relative);
        if (!metadataBank)
        {
            String language;
            metadataBank = FindLocalizedBankByFile(metadataBanks, relative, language);
            String selectedLanguage = manifest.Locale.ToUpper();
            if (selectedLanguage.IsEmpty() || selectedLanguage == TEXT("DEFAULT"))
                selectedLanguage = TEXT("EN");
            if (metadataBank && language != selectedLanguage && language != TEXT("EN"))
                continue;
        }
        if (!metadataBank)
        {
            data.Error(String::Format(TEXT("FMOD bank '{0}' is missing from metadata."), relative));
            return true;
        }
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
        bank.ID = metadataBank->ID;
        bank.Hash = HashFile(sourcePath);
        bank.Size = FileSystem::GetFileSize(sourcePath);
        if (bank.Hash.IsEmpty())
        {
            data.Error(String::Format(TEXT("Failed to hash FMOD bank '{0}'."), sourcePath));
            return true;
        }
    }

    LOG(Info, "Cooked {0} FMOD banks for {1}.", manifest.Banks.Count(), manifest.Platform);
    if (WriteManifest(outputAudioDir / TEXT("AudioCookManifest.json"), manifest, runtimeFiles, pluginFiles))
    {
        data.Error(TEXT("Failed to write AudioCookManifest.json."));
        return true;
    }
    return false;
}
