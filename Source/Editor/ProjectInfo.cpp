// Copyright (c) Wojciech Figat. All rights reserved.

#include "ProjectInfo.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/File.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Core/Math/Quaternion.h"
#include "Engine/Profiler/ProfilerMemory.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include <ThirdParty/pugixml/pugixml.hpp>
using namespace pugi;

Array<ProjectInfo*> ProjectInfo::ProjectsCache;

struct XmlCharAsChar
{
#if PLATFORM_TEXT_IS_CHAR16
    Char* Str = nullptr;
    
    XmlCharAsChar(const pugi::char_t* str)
    {
        if (!str)
            return;
        int32 length = 0;
        while (str[length])
            length++;
        Str = (Char*)Platform::Allocate(length * sizeof(Char), sizeof(Char));
        for (int32 i = 0; i <= length; i++)
            Str[i] = (Char)str[i];
    }

    ~XmlCharAsChar()
    {
        Platform::Free(Str);
    }
#else
    const Char* Str;

    XmlCharAsChar(const pugi::char_t* str)
        : Str(str)
    {
    }
#endif
};

void ShowProjectLoadError(const Char* errorMsg, const String& projectRootFolder)
{
    Platform::Error(String::Format(TEXT("Failed to load project. {0}\nPath: '{1}'"), errorMsg, projectRootFolder));
}

Vector3 GetVector3FromXml(const xml_node& parent, const PUGIXML_CHAR* name, const Vector3& defaultValue)
{
    const auto node = parent.child(name);
    if (!node.empty())
    {
        const auto x = node.child_value(PUGIXML_TEXT("X"));
        const auto y = node.child_value(PUGIXML_TEXT("Y"));
        const auto z = node.child_value(PUGIXML_TEXT("Z"));
        if (x && y && z)
        {
            XmlCharAsChar xs(x), ys(y), zs(z);
            Float3 v;
            if (!StringUtils::Parse(xs.Str, &v.X) && !StringUtils::Parse(ys.Str, &v.Y) && !StringUtils::Parse(zs.Str, &v.Z))
            {
                return (Vector3)v;
            }
        }
    }
    return defaultValue;
}

int32 GetIntFromXml(const xml_node& parent, const PUGIXML_CHAR* name, const int32 defaultValue)
{
    const auto node = parent.child_value(name);
    if (node)
    {
        XmlCharAsChar s(node);
        int32 v;
        if (!StringUtils::Parse(s.Str, &v))
        {
            return v;
        }
    }

    return defaultValue;
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

bool ProjectInfo::LoadOldProject(const String& projectPath)
{
    // Open Xml file
    xml_document doc;
    const xml_parse_result result = doc.load_file((const PUGIXML_CHAR*)*projectPath);
    if (result.status)
    {
        ShowProjectLoadError(TEXT("Xml file parsing error."), projectPath);
        return true;
    }

    // Get root node
    const xml_node root = doc.child(PUGIXML_TEXT("Project"));
    if (!root)
    {
        ShowProjectLoadError(TEXT("Missing Project root node in xml file."), projectPath);
        return true;
    }

    // Load data
    Name = (const Char*)root.child_value(PUGIXML_TEXT("Name"));
    ProjectPath = projectPath;
    ProjectFolderPath = StringUtils::GetDirectoryName(projectPath);
    DefaultScene = Guid::Empty;
    const auto defaultScene = root.child_value(PUGIXML_TEXT("DefaultSceneId"));
    if (defaultScene)
    {
        Guid legacyGuid;
        if (!Guid::Parse((const Char*)defaultScene, legacyGuid))
            DefaultScene = legacyGuid;
    }
    DefaultSceneSpawn.Position = GetVector3FromXml(root, PUGIXML_TEXT("DefaultSceneSpawnPos"), Vector3::Zero);
    DefaultSceneSpawn.Direction = Quaternion::Euler(GetVector3FromXml(root, PUGIXML_TEXT("DefaultSceneSpawnDir"), Vector3::Zero)) * Vector3::Forward;
    MinEngineVersion = ::Version(0, 0, GetIntFromXml(root, PUGIXML_TEXT("MinVersionSupported"), 0));

    // Always reference engine project
    auto& flaxReference = References.AddOne();
    flaxReference.Name = TEXT("$(EnginePath)/Flax.flaxproj");
    flaxReference.Project = Load(Globals::StartupFolder / TEXT("Flax.flaxproj"));
    if (!flaxReference.Project)
    {
        ShowProjectLoadError(TEXT("Failed to load Flax Engine project."), projectPath);
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
