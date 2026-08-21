// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodCatalogBuilder.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/FileWriteStream.h"
#include "Engine/Audio/FMOD/FmodConvert.h"

#if AUDIO_EVENT_API_FMOD
namespace
{
    struct LoadedBank
    {
        String File;
        Guid ID;
        FMOD::Studio::Bank* Bank = nullptr;
    };

    template<typename T>
    String ReadFmodPath(T* value)
    {
        char buffer[2048] = {};
        int length = 0;
        return value && value->getPath(buffer, sizeof(buffer), &length) == FMOD_OK ? String(buffer) : String::Empty;
    }

    // FMOD returns native GUID fields, while authored metadata uses the
    // canonical Studio text ordering (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx).
    Guid FromFmodStudioGuid(const FMOD_GUID& value)
    {
        const uint32 b = ((uint32)value.Data2 << 16) | (uint32)value.Data3;
        const uint32 c = ((uint32)value.Data4[0] << 24) | ((uint32)value.Data4[1] << 16) |
                         ((uint32)value.Data4[2] << 8) | (uint32)value.Data4[3];
        const uint32 d = ((uint32)value.Data4[4] << 24) | ((uint32)value.Data4[5] << 16) |
                         ((uint32)value.Data4[6] << 8) | (uint32)value.Data4[7];
        return Guid(value.Data1, b, c, d);
    }

    String GetLocalizedBankFamily(const String& path)
    {
        const String file = String(StringUtils::GetFileNameWithoutExtension(path));
        if (file.Length() < 4 || file[file.Length() - 3] != '_' ||
            !StringUtils::IsAlpha(file[file.Length() - 2]) || !StringUtils::IsAlpha(file[file.Length() - 1]))
            return String::Empty;
        const String directory = String(StringUtils::GetDirectoryName(path));
        return directory / file.Substring(0, file.Length() - 3);
    }
}
#endif

bool FmodCatalogBuilder::BuildCatalog(const String& banksDirectory, const String& outputDirectory)
{
#if !AUDIO_EVENT_API_FMOD
    LOG(Error, "FMOD catalog generation is unavailable because this editor was built without FMOD Studio.");
    return false;
#else
    if (!FileSystem::DirectoryExists(banksDirectory))
    {
        LOG(Error, "FMOD banks directory '{0}' does not exist.", banksDirectory);
        return false;
    }
    if (!FileSystem::DirectoryExists(outputDirectory) && FileSystem::CreateDirectory(outputDirectory))
        return false;

    Array<String> bankFiles;
    FileSystem::DirectoryGetFiles(bankFiles, banksDirectory, TEXT("*.bank"), DirectorySearchOption::AllDirectories);
    if (bankFiles.IsEmpty())
    {
        LOG(Error, "FMOD bank directory '{0}' contains no .bank files.", banksDirectory);
        return false;
    }
    const auto comesBefore = [](const String& a, const String& b)
    {
        const bool aStrings = a.EndsWith(TEXT(".strings.bank"), StringSearchCase::IgnoreCase);
        const bool bStrings = b.EndsWith(TEXT(".strings.bank"), StringSearchCase::IgnoreCase);
        if (aStrings != bStrings)
            return aStrings;
        const bool aMaster = StringUtils::GetFileName(a).StartsWith(TEXT("Master."), StringSearchCase::IgnoreCase);
        const bool bMaster = StringUtils::GetFileName(b).StartsWith(TEXT("Master."), StringSearchCase::IgnoreCase);
        if (aMaster != bMaster)
            return aMaster;
        // Flat sample-bank layouts often contain mutually exclusive localized
        // variants with one shared FMOD bank GUID. Prefer English as the
        // default catalog representative; platform/locale subdirectories keep
        // producing their own sidecars independently.
        const bool aEnglish = a.EndsWith(TEXT("_EN.bank"), StringSearchCase::IgnoreCase);
        const bool bEnglish = b.EndsWith(TEXT("_EN.bank"), StringSearchCase::IgnoreCase);
        return aEnglish != bEnglish ? aEnglish : a < b;
    };
    for (int32 i = 1; i < bankFiles.Count(); i++)
    {
        const String value = bankFiles[i];
        int32 j = i - 1;
        while (j >= 0 && comesBefore(value, bankFiles[j]))
        {
            bankFiles[j + 1] = bankFiles[j];
            j--;
        }
        bankFiles[j + 1] = value;
    }

    FMOD::Studio::System* studio = nullptr;
    FMOD::System* core = nullptr;
    if (FMOD::Studio::System::create(&studio) != FMOD_OK || !studio || studio->getCoreSystem(&core) != FMOD_OK || !core)
        return false;
    core->setOutput(FMOD_OUTPUTTYPE_NOSOUND);
    if (studio->initialize(64, FMOD_STUDIO_INIT_NORMAL, FMOD_INIT_NORMAL, nullptr) != FMOD_OK)
    {
        studio->release();
        return false;
    }

    Array<LoadedBank> banks;
    HashSet<String> localizedBankFamilies;
    for (const String& file : bankFiles)
    {
        const String localizedFamily = GetLocalizedBankFamily(file);
        if (localizedFamily.HasChars() && localizedBankFamilies.Contains(localizedFamily))
        {
            LOG(Warning, "FMOD catalog skipped mutually exclusive localized bank variant '{0}'.", file);
            continue;
        }
        FMOD::Studio::Bank* bank = nullptr;
        StringAnsi fileAnsi(file);
        const FMOD_RESULT loadResult = studio->loadBankFile(fileAnsi.Get(), FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);
        if (loadResult == FMOD_ERR_EVENT_ALREADY_LOADED)
        {
            LOG(Warning, "FMOD catalog skipped mutually exclusive bank variant '{0}' because its bank identity is already represented.", file);
            continue;
        }
        if (loadResult != FMOD_OK || !bank)
        {
            LOG(Error, "FMOD catalog could not load '{0}': ({1}) {2}", file, (int32)loadResult, String(FMOD_ErrorString(loadResult)));
            studio->release();
            return false;
        }
        FMOD_GUID id = {};
        bank->getID(&id);
        LoadedBank value;
        value.File = String(StringUtils::GetFileName(file));
        value.ID = FromFmodStudioGuid(id);
        value.Bank = bank;
        banks.Add(value);
        if (localizedFamily.HasChars())
            localizedBankFamilies.Add(localizedFamily);
    }

    rapidjson_flax::StringBuffer buffer;
    PrettyJsonWriter writer(buffer);
    const auto writeString = [&writer](const String& value)
    {
        const StringAsUTF8<256> utf8(*value, value.Length());
        writer.String(utf8.Get(), utf8.Length());
    };
    struct MixerRecord { Guid ID; String Path; };
    HashSet<Guid> writtenEvents, writtenSnapshots, writtenBuses, writtenVcas;
    Array<MixerRecord> snapshots, buses, vcas;

    writer.StartObject();
    writer.JKEY("schema"); writer.Int(2);
    writer.JKEY("revision"); writeString(String::Format(TEXT("runtime-{0}"), DateTime::Now().Ticks));
    writer.JKEY("banks"); writer.StartArray();
    for (const LoadedBank& bank : banks)
    {
        writer.StartObject();
        writer.JKEY("id"); writeString(bank.ID.ToString());
        writer.JKEY("file"); writeString(bank.File);
        writer.JKEY("dependencies"); writer.StartArray(); writer.EndArray(0);
        writer.JKEY("events"); writer.StartArray();
        int eventCount = 0;
        bank.Bank->getEventCount(&eventCount);
        Array<FMOD::Studio::EventDescription*> events;
        events.Resize(eventCount);
        if (eventCount > 0)
            bank.Bank->getEventList(events.Get(), eventCount, &eventCount);
        int eventsWritten = 0;
        for (int i = 0; i < eventCount; i++)
        {
            auto* description = events[i];
            if (!description)
                continue;
            FMOD_GUID eventGuid = {};
            description->getID(&eventGuid);
            const Guid eventId = FromFmodStudioGuid(eventGuid);
            bool isSnapshot = false;
            description->isSnapshot(&isSnapshot);
            const String eventPath = ReadFmodPath(description);
            if (isSnapshot)
            {
                if (writtenSnapshots.Add(eventId)) snapshots.Add({ eventId, eventPath });
                continue;
            }
            if (!writtenEvents.Add(eventId))
                continue;
            bool is3D = false, oneShot = false;
            float minimum = 1.0f, maximum = 100.0f;
            int length = 0;
            description->is3D(&is3D);
            description->isOneshot(&oneShot);
            description->getMinMaxDistance(&minimum, &maximum);
            description->getLength(&length);
            writer.StartObject();
            writer.JKEY("id"); writeString(eventId.ToString());
            writer.JKEY("path"); writeString(eventPath);
            writer.JKEY("is3D"); writer.Bool(is3D);
            writer.JKEY("isOneShot"); writer.Bool(oneShot);
            writer.JKEY("minDistance"); writer.Double(minimum);
            writer.JKEY("maxDistance"); writer.Double(maximum);
            writer.JKEY("length"); writer.Double(length * 0.001);
            writer.JKEY("bankDependencies"); writer.StartArray(); writeString(bank.ID.ToString()); writer.EndArray(1);
            writer.JKEY("parameters"); writer.StartArray();
            int parameterCount = 0;
            description->getParameterDescriptionCount(&parameterCount);
            int parametersWritten = 0;
            for (int parameterIndex = 0; parameterIndex < parameterCount; parameterIndex++)
            {
                FMOD_STUDIO_PARAMETER_DESCRIPTION parameter = {};
                if (description->getParameterDescriptionByIndex(parameterIndex, &parameter) != FMOD_OK)
                    continue;
                writer.StartObject();
                writer.JKEY("name"); writeString(String(parameter.name));
                writer.JKEY("data1"); writer.Uint(parameter.id.data1);
                writer.JKEY("data2"); writer.Uint(parameter.id.data2);
                writer.JKEY("minimum"); writer.Double(parameter.minimum);
                writer.JKEY("maximum"); writer.Double(parameter.maximum);
                writer.JKEY("defaultValue"); writer.Double(parameter.defaultvalue);
                writer.JKEY("type"); writer.Int((int32)parameter.type);
                writer.JKEY("flags"); writer.Uint((uint32)parameter.flags);
                writer.EndObject();
                parametersWritten++;
            }
            writer.EndArray(parametersWritten);
            writer.EndObject();
            eventsWritten++;
        }
        writer.EndArray(eventsWritten);

        int busCount = 0;
        bank.Bank->getBusCount(&busCount);
        Array<FMOD::Studio::Bus*> bankBuses;
        bankBuses.Resize(busCount);
        if (busCount > 0) bank.Bank->getBusList(bankBuses.Get(), busCount, &busCount);
        for (int i = 0; i < busCount; i++)
        {
            FMOD_GUID id = {};
            if (bankBuses[i] && bankBuses[i]->getID(&id) == FMOD_OK)
            {
                const Guid value = FromFmodStudioGuid(id);
                if (writtenBuses.Add(value)) buses.Add({ value, ReadFmodPath(bankBuses[i]) });
            }
        }
        int vcaCount = 0;
        bank.Bank->getVCACount(&vcaCount);
        Array<FMOD::Studio::VCA*> bankVcas;
        bankVcas.Resize(vcaCount);
        if (vcaCount > 0) bank.Bank->getVCAList(bankVcas.Get(), vcaCount, &vcaCount);
        for (int i = 0; i < vcaCount; i++)
        {
            FMOD_GUID id = {};
            if (bankVcas[i] && bankVcas[i]->getID(&id) == FMOD_OK)
            {
                const Guid value = FromFmodStudioGuid(id);
                if (writtenVcas.Add(value)) vcas.Add({ value, ReadFmodPath(bankVcas[i]) });
            }
        }
        writer.EndObject();
    }
    writer.EndArray(banks.Count());

    const auto writeMixer = [&writer, &writeString](const char* key, const Array<MixerRecord>& values)
    {
        writer.Key(key); writer.StartArray();
        for (const MixerRecord& value : values)
        {
            writer.StartObject();
            writer.JKEY("id"); writeString(value.ID.ToString());
            writer.JKEY("path"); writeString(value.Path);
            writer.EndObject();
        }
        writer.EndArray(values.Count());
    };
    writeMixer("snapshots", snapshots);
    writeMixer("buses", buses);
    writeMixer("vcas", vcas);
    writer.EndObject();

    const String output = outputDirectory / TEXT("fmod-metadata.json");
    FileWriteStream* stream = FileWriteStream::Open(output);
    if (!stream)
    {
        studio->release();
        return false;
    }
    stream->WriteBytes(buffer.GetString(), (uint32)buffer.GetSize());
    stream->Close();
    Delete(stream);
    studio->release();
    LOG(Info, "Generated FMOD metadata catalog '{0}' from {1} banks and {2} events.", output, banks.Count(), writtenEvents.Count());
    return true;
#endif
}
