// Copyright (c) Wojciech Figat. All rights reserved.

#include "ProjectInfo.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/File.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Profiler/ProfilerMemory.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
Array<ProjectInfo*> ProjectInfo::ProjectsCache;

void ShowProjectLoadError(const Char* errorMsg, const String& projectRootFolder)
{
    Platform::Error(String::Format(TEXT("Failed to load project. {0}\nPath: '{1}'"), errorMsg, projectRootFolder));
}

bool ProjectInfo::SaveProject()
{
    // Serialize object to Json
    rapidjson_flax::StringBuffer buffer;
    PrettyJsonWriter writerObj(buffer);
    auto& stream = *(JsonWriter*)&writerObj;
    stream.StartObject();
    {
        stream.JKEY("Name");
        stream.String(Name);

        stream.JKEY("Version");
        stream.String(Version.ToString());

        stream.JKEY("AssetSystemVersion");
        stream.Int(AssetSystemVersion);

        stream.JKEY("ProjectSettingsIndexGuid");
        stream.Guid(ProjectSettingsIndexGuid);

        stream.JKEY("Company");
        stream.String(Company);

        stream.JKEY("Copyright");
        stream.String(Copyright);

        stream.JKEY("GameTarget");
        stream.String(GameTarget);

        stream.JKEY("EditorTarget");
        stream.String(EditorTarget);

        stream.JKEY("References");
        stream.StartArray();
        for (auto& reference : References)
        {
            stream.StartObject();
            stream.JKEY("Name");
            stream.String(reference.Name);
            stream.EndObject();
        }
        stream.EndArray();

        if (DefaultScene.IsValid())
        {
            stream.JKEY("DefaultScene");
            stream.Guid(DefaultScene);
        }

        if (DefaultSceneSpawn != Ray(Vector3::Zero, Vector3::Forward))
        {
            stream.JKEY("DefaultSceneSpawn");
            stream.Ray(DefaultSceneSpawn);
        }

        stream.JKEY("MinEngineVersion");
        stream.String(MinEngineVersion.ToString());

        if (EngineNickname.HasChars())
        {
            stream.JKEY("EngineNickname");
            stream.String(EngineNickname);
        }
    }
    stream.EndObject();

    // Write to file
    return File::WriteAllBytes(ProjectPath, (const byte*)buffer.GetString(), (int32)buffer.GetSize());
}

bool ProjectInfo::LoadProject(const String& projectPath)
{
    // Load Json file
    StringAnsi fileData;
    if (File::ReadAllText(projectPath, fileData))
    {
        ShowProjectLoadError(TEXT("Failed to read file contents."), projectPath);
        return true;
    }

    // Parse Json data
    rapidjson_flax::Document document;
    document.Parse(fileData.Get(), fileData.Length());
    if (document.HasParseError())
    {
        ShowProjectLoadError(TEXT("Failed to parse project contents. Ensure to have valid Json format."), projectPath);
        return true;
    }

    const auto unsupported = [&projectPath](const Char* reason)
    {
        const String message = String::Format(TEXT("Unsupported current project format: {0} Run the separate offline migrator from the old branch before opening this project."), reason);
        ShowProjectLoadError(*message, projectPath);
        return true;
    };
    CanonicalJsonError canonicalError;
    if (!document.IsObject() || CanonicalJsonWriter::Validate(document, canonicalError))
        return unsupported(TEXT("the manifest is not a unique-key JSON object."));
    const auto requireString = [&document](const char* name, bool allowEmpty = false)
    {
        const auto member = document.FindMember(name);
        return member != document.MemberEnd() && member->value.IsString() &&
            (allowEmpty || member->value.GetStringLength() != 0);
    };
    if (!requireString("Name") || !requireString("Version") || !requireString("Company", true) ||
        !requireString("Copyright", true) || !requireString("GameTarget") || !requireString("EditorTarget") ||
        !requireString("MinEngineVersion"))
        return unsupported(TEXT("a required current string field is missing or invalid."));
    const auto assetSystemVersion = document.FindMember("AssetSystemVersion");
    if (assetSystemVersion == document.MemberEnd() || !assetSystemVersion->value.IsInt() || assetSystemVersion->value.GetInt() != 2)
        return unsupported(TEXT("AssetSystemVersion must be exactly 2."));
    const auto settingsGuid = document.FindMember("ProjectSettingsIndexGuid");
    Guid parsedSettingsGuid;
    if (settingsGuid == document.MemberEnd() || !settingsGuid->value.IsString() || settingsGuid->value.GetStringLength() != 32 ||
        Guid::Parse(StringAnsiView(settingsGuid->value.GetString(), settingsGuid->value.GetStringLength()), parsedSettingsGuid) ||
        !parsedSettingsGuid.IsValid() || StringAnsi(parsedSettingsGuid.ToString(Guid::FormatType::N).ToLower()) !=
            StringAnsiView(settingsGuid->value.GetString(), settingsGuid->value.GetStringLength()))
        return unsupported(TEXT("ProjectSettingsIndexGuid must be a lowercase 32-hex GUID."));
    ::Version parsedVersion;
    const auto versionText = document.FindMember("Version");
    const auto minimumText = document.FindMember("MinEngineVersion");
    if (::Version::Parse(String(StringAnsiView(versionText->value.GetString(), versionText->value.GetStringLength())), &parsedVersion) ||
        ::Version::Parse(String(StringAnsiView(minimumText->value.GetString(), minimumText->value.GetStringLength())), &parsedVersion))
        return unsupported(TEXT("Version and MinEngineVersion must be parseable strings."));
    const auto referencesValue = document.FindMember("References");
    if (referencesValue == document.MemberEnd() || !referencesValue->value.IsArray())
        return unsupported(TEXT("References must be an array."));
    for (const auto& reference : referencesValue->value.GetArray())
    {
        if (!reference.IsObject())
            return unsupported(TEXT("every project reference requires a nonempty Name."));
        const auto name = reference.FindMember("Name");
        if (name == reference.MemberEnd() || !name->value.IsString() || name->value.GetStringLength() == 0)
            return unsupported(TEXT("every project reference requires a nonempty Name."));
    }
    const auto defaultSceneValue = document.FindMember("DefaultScene");
    if (defaultSceneValue != document.MemberEnd())
    {
        if (!defaultSceneValue->value.IsString())
            return unsupported(TEXT("DefaultScene must use the current GUID-only form."));
        Guid parsedGuid;
        if (defaultSceneValue->value.GetStringLength() != 32 ||
            Guid::Parse(StringAnsiView(defaultSceneValue->value.GetString(), defaultSceneValue->value.GetStringLength()), parsedGuid) || !parsedGuid.IsValid() ||
            StringAnsi(parsedGuid.ToString(Guid::FormatType::N).ToLower()) != StringAnsiView(defaultSceneValue->value.GetString(), defaultSceneValue->value.GetStringLength()))
            return unsupported(TEXT("DefaultScene must contain one canonical GUID."));
    }
    const auto engineNickname = document.FindMember("EngineNickname");
    if (engineNickname != document.MemberEnd() && (!engineNickname->value.IsString() || engineNickname->value.GetStringLength() == 0))
        return unsupported(TEXT("EngineNickname must be a nonempty string when present."));
    const auto defaultSpawn = document.FindMember("DefaultSceneSpawn");
    if (defaultSpawn != document.MemberEnd())
    {
        const auto validVector = [](const rapidjson_flax::Value& value)
        {
            if (!value.IsObject())
                return false;
            const auto x = value.FindMember("X");
            const auto y = value.FindMember("Y");
            const auto z = value.FindMember("Z");
            return x != value.MemberEnd() && x->value.IsNumber() && y != value.MemberEnd() && y->value.IsNumber() &&
                z != value.MemberEnd() && z->value.IsNumber();
        };
        if (!defaultSpawn->value.IsObject())
            return unsupported(TEXT("DefaultSceneSpawn must use the current Ray object form."));
        const auto position = defaultSpawn->value.FindMember("Position");
        const auto direction = defaultSpawn->value.FindMember("Direction");
        if (position == defaultSpawn->value.MemberEnd() || direction == defaultSpawn->value.MemberEnd() ||
            !validVector(position->value) || !validVector(direction->value))
            return unsupported(TEXT("DefaultSceneSpawn must use the current Ray object form."));
    }
    const auto configuration = document.FindMember("Configuration");
    if (configuration != document.MemberEnd() && !configuration->value.IsObject())
        return unsupported(TEXT("Configuration must be an object when present."));

    // Parse properties
    Name = JsonTools::GetString(document, "Name", String::Empty);
    ProjectPath = projectPath;
    ProjectFolderPath = StringUtils::GetDirectoryName(projectPath);
    const auto versionMember = document.FindMember("Version");
    if (versionMember != document.MemberEnd())
    {
        auto& version = versionMember->value;
        if (version.IsString())
        {
            Version::Parse(version.GetText(), &Version);
        }
    }
    if (Version.Revision() == 0)
        Version = ::Version(Version.Major(), Version.Minor(), Version.Build());
    if (Version.Build() == 0 && Version.Revision() == -1)
        Version = ::Version(Version.Major(), Version.Minor());
    AssetSystemVersion = JsonTools::GetInt(document, "AssetSystemVersion", 0);
    ProjectSettingsIndexGuid = JsonTools::GetGuid(document, "ProjectSettingsIndexGuid");
    Company = JsonTools::GetString(document, "Company", String::Empty);
    Copyright = JsonTools::GetString(document, "Copyright", String::Empty);
    GameTarget = JsonTools::GetString(document, "GameTarget", String::Empty);
    EditorTarget = JsonTools::GetString(document, "EditorTarget", String::Empty);
    EngineNickname = JsonTools::GetString(document, "EngineNickname", String::Empty);
    const auto referencesMember = document.FindMember("References");
    if (referencesMember != document.MemberEnd())
    {
        const auto& references = referencesMember->value.GetArray();
        References.Resize(references.Size());
        for (int32 i = 0; i < References.Count(); i++)
        {
            auto& reference = References[i];
            auto& value = references[i];
            reference.Name = JsonTools::GetString(value, "Name", String::Empty);

            String referencePath;
            if (reference.Name.StartsWith(TEXT("$(EnginePath)")))
            {
                // Relative to engine root
                referencePath = Globals::StartupFolder / reference.Name.Substring(14);
            }
            else if (reference.Name.StartsWith(TEXT("$(ProjectPath)")))
            {
                // Relative to project root
                referencePath = ProjectFolderPath / reference.Name.Substring(15);
            }
            else if (FileSystem::IsRelative(reference.Name))
            {
                // Relative to workspace
                referencePath = Globals::StartupFolder / reference.Name;
            }
            else
            {
                // Absolute
                referencePath = reference.Name;
            }
            StringUtils::PathRemoveRelativeParts(referencePath);

            // Load referenced project
            reference.Project = Load(referencePath);
            if (reference.Project == nullptr)
            {
                LOG(Error, "Failed to load referenced project ({0}, from {1})", reference.Name, referencePath);
                return true;
            }
        }
    }
    DefaultScene = Guid::Empty;
    const auto defaultSceneMember = document.FindMember("DefaultScene");
    if (defaultSceneMember != document.MemberEnd())
    {
        const auto& value = defaultSceneMember->value;
        if (value.IsString() && value.GetStringLength() == 32)
        {
            Guid guid;
            if (Guid::Parse(StringAnsiView(value.GetString(), value.GetStringLength()), guid) || !guid.IsValid())
            {
                ShowProjectLoadError(TEXT("Invalid DefaultScene GUID."), projectPath);
                return true;
            }
            DefaultScene = guid;
        }
        else
        {
            ShowProjectLoadError(TEXT("DefaultScene must contain one asset GUID."), projectPath);
            return true;
        }
    }
    DefaultSceneSpawn = JsonTools::GetRay(document, "DefaultSceneSpawn", Ray(Vector3::Zero, Vector3::Forward));
    const auto minEngineVersionMember = document.FindMember("MinEngineVersion");
    if (minEngineVersionMember != document.MemberEnd())
    {
        auto& minEngineVersion = minEngineVersionMember->value;
        if (minEngineVersion.IsString())
        {
            Version::Parse(minEngineVersion.GetText(), &MinEngineVersion);
        }
    }

    // Validate properties
    if (Name.Length() == 0)
    {
        ShowProjectLoadError(TEXT("Missing project name."), projectPath);
        return true;
    }

    return false;
}

ProjectInfo* ProjectInfo::Load(const String& path)
{
    // Try to reuse loaded file
    for (int32 i = 0; i < ProjectsCache.Count(); i++)
    {
        if (ProjectsCache[i]->ProjectPath == path)
            return ProjectsCache[i];
    }

    // Load
    PROFILE_MEM(Editor);
    auto project = New<ProjectInfo>();
    if (project->LoadProject(path))
    {
        Delete(project);
        return nullptr;
    }

    // Cache project
    ProjectsCache.Add(project);
    return project;
}
