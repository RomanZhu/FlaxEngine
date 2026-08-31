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
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseFacade.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/AssetDatabase/AssetMountDescriptor.h"
#include "Engine/Content/AssetDatabase/AssetMutationService.h"
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

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    Guid NewProjectRoleId(const Guid& projectId, const char* role)
    {
        ContentHasher hasher;
        const StringAnsi domain("flax-new-project-bootstrap-v1");
        hasher.Update(domain.Get(), domain.Length());
        hasher.Update(projectId.Values, sizeof(projectId.Values));
        hasher.Update(role, StringUtils::Length(role));
        const ContentHash hash = hasher.Finalize();
        Guid result(hash.Values[0], hash.Values[1], hash.Values[2], hash.Values[3]);
        if (!result.IsValid())
            result.D = 1;
        return result;
    }

    bool ReadSettingsData(const StringView& path, JsonDocument& document, JsonValue*& data)
    {
        StringAnsi source;
        if (File::ReadAllText(path, source))
            return true;
        document.Parse(source.Get(), source.Length());
        const auto member = document.FindMember("Data");
        if (document.HasParseError() || !document.IsObject() || member == document.MemberEnd() || !member->value.IsObject())
            return true;
        data = &member->value;
        return false;
    }

    void SetString(JsonValue& data, const char* name, const StringView& value, JsonDocument::AllocatorType& allocator)
    {
        const StringAnsi ansi(value);
        const auto member = data.FindMember(name);
        if (member == data.MemberEnd())
            data.AddMember(JsonValue(name, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
        else
            member->value.SetString(ansi.Get(), ansi.Length(), allocator);
    }

    void SetAssetReference(JsonValue& data, const char* name, const Guid& value, JsonDocument::AllocatorType& allocator)
    {
        JsonValue reference;
        if (value.IsValid())
        {
            reference.SetObject();
            const StringAnsi guid(value.ToString(Guid::FormatType::N));
            reference.AddMember("guid", JsonValue(guid.Get(), guid.Length(), allocator), allocator);
            reference.AddMember("localId", 1, allocator);
        }
        else
        {
            reference.SetNull();
        }
        const auto member = data.FindMember(name);
        if (member == data.MemberEnd())
            data.AddMember(JsonValue(name, allocator), reference.Move(), allocator);
        else
            member->value = reference.Move();
    }

    Guid GetAssetReference(const JsonValue& data, const char* name)
    {
        const auto member = data.FindMember(name);
        if (member == data.MemberEnd() || !member->value.IsObject())
            return Guid::Empty;
        const auto guid = member->value.FindMember("guid");
        const auto localId = member->value.FindMember("localId");
        if (guid == member->value.MemberEnd() || localId == member->value.MemberEnd() ||
            !localId->value.IsInt64() || localId->value.GetInt64() != 1)
            return Guid::Empty;
        return JsonTools::GetGuid(guid->value);
    }

    void SetRay(JsonValue& data, const char* name, const Ray& value, JsonDocument::AllocatorType& allocator)
    {
        rapidjson_flax::StringBuffer buffer;
        CompactJsonWriter writerObject(buffer);
        auto& writer = *(JsonWriter*)&writerObject;
        writer.StartObject();
        writer.JKEY("Value");
        writer.Ray(value);
        writer.EndObject();
        JsonDocument temporary;
        temporary.Parse(buffer.GetString(), buffer.GetSize());
        const auto source = temporary.FindMember("Value");
        const auto destination = data.FindMember(name);
        if (destination == data.MemberEnd())
            data.AddMember(JsonValue(name, allocator), JsonValue(source->value, allocator), allocator);
        else
            destination->value.CopyFrom(source->value, allocator);
    }

    bool SerializeSettings(JsonDocument& document, StringAnsi& output)
    {
        Array<StringAnsi> order;
        order.Add("ID");
        order.Add("TypeName");
        order.Add("EngineBuild");
        order.Add("Data");
        CanonicalJsonError error;
        return CanonicalJsonWriter::Write(document, output, error, &order);
    }

    void BindProjectDatabaseCommit(const ProjectInfo& project, AssetMutationService& service)
    {
        if (Globals::ProjectFolder.HasChars() &&
            FileSystem::AreFilePathsEquivalent(project.ProjectFolderPath, Globals::ProjectFolder) &&
            AssetDatabase::Get().IsHardCutEnabled())
        {
            service.DatabaseCommitHook = [](const AssetMutationResult& pending)
            {
                return AssetDatabaseFacade::RefreshSources(pending.ChangedPaths);
            };
        }
    }

    bool WriteSettings(const ProjectInfo& project, const StringView& path, JsonDocument& document)
    {
        StringAnsi output;
        if (SerializeSettings(document, output))
            return true;
        AssetMutationService service(project.ProjectFolderPath, project.ProjectFolderPath / TEXT("Content"),
            project.ProjectFolderPath / TEXT("Library/AssetDatabase/MutationJournals"),
            project.ProjectFolderPath / TEXT("Library/AssetDatabase/Recovery"));
        BindProjectDatabaseCommit(project, service);
        AssetMutationResult result;
        return service.ReplaceContents(path, StringAnsiView(output), result);
    }

    bool CreateSettingsSource(const ProjectInfo& project, const StringView& path, const Guid& id,
        const StringView& typeName, JsonValue& data)
    {
        const String metaPath = String(path) + TEXT(".meta");
        if (FileSystem::FileExists(path) || FileSystem::FileExists(metaPath))
            return !FileSystem::FileExists(path) || !FileSystem::FileExists(metaPath);
        JsonDocument document;
        document.SetObject();
        SetString(document, "ID", id.ToString(Guid::FormatType::N), document.GetAllocator());
        SetString(document, "TypeName", typeName, document.GetAllocator());
        document.AddMember("Data", JsonValue(data, document.GetAllocator()), document.GetAllocator());
        StringAnsi output;
        if (SerializeSettings(document, output))
            return true;
        AssetMeta meta;
        meta.ID = id;
        meta.AssetType = typeName;
        meta.SourceKind = AssetSourceKind::ExistingJson;
        meta.Processor.ID = TEXT("Flax.ExistingJson");
        meta.Processor.SettingsVersion = 1;
        AssetMutationService service(project.ProjectFolderPath, project.ProjectFolderPath / TEXT("Content"),
            project.ProjectFolderPath / TEXT("Library/AssetDatabase/MutationJournals"),
            project.ProjectFolderPath / TEXT("Library/AssetDatabase/Recovery"));
        BindProjectDatabaseCommit(project, service);
        AssetMutationResult result;
        return service.CreateAsset(path, StringAnsiView(output), meta, result);
    }

    bool EnsureV3BootstrapSources(const ProjectInfo& project)
    {
        if (!project.ProjectId.IsValid())
            return true;
        const String content = project.ProjectFolderPath / TEXT("Content");
        const String settings = content / TEXT("Settings");
        if (!FileSystem::DirectoryExists(content))
            return true;

        const Guid settingsId = NewProjectRoleId(project.ProjectId, "settings-folder");
        const String settingsMetaPath = settings + TEXT(".meta");
        if (!FileSystem::DirectoryExists(settings) || !FileSystem::FileExists(settingsMetaPath))
        {
            AssetMeta meta;
            meta.ID = settingsId;
            meta.FolderAsset = true;
            meta.AssetType = TEXT("FlaxEngine.Folder");
            meta.SourceKind = AssetSourceKind::Folder;
            meta.Processor.ID = TEXT("Flax.Folder");
            AssetMutationService service(project.ProjectFolderPath, content,
                project.ProjectFolderPath / TEXT("Library/AssetDatabase/MutationJournals"),
                project.ProjectFolderPath / TEXT("Library/AssetDatabase/Recovery"));
            BindProjectDatabaseCommit(project, service);
            AssetMutationResult result;
            const bool failed = FileSystem::DirectoryExists(settings)
                ? service.RegisterExisting(settings, meta, false, result)
                : service.CreateFolder(settings, meta, result);
            if (failed)
                return true;
        }

        const Guid projectSettingsId = NewProjectRoleId(project.ProjectId, "project-settings");
        const Guid buildSettingsId = NewProjectRoleId(project.ProjectId, "build-settings");
        const Guid pipelineSettingsId = NewProjectRoleId(project.ProjectId, "asset-pipeline-settings");
        const Guid editorSettingsId = NewProjectRoleId(project.ProjectId, "editor-settings");
        const Guid mountSettingsId = NewProjectRoleId(project.ProjectId, "content-mount-settings");

        JsonDocument owner;
        owner.SetObject();
        SetString(owner, "ProductName", project.Name, owner.GetAllocator());
        SetString(owner, "Version", project.Version.ToString(), owner.GetAllocator());
        SetString(owner, "CompanyName", project.Company, owner.GetAllocator());
        SetString(owner, "CopyrightNotice", project.Copyright, owner.GetAllocator());
        SetAssetReference(owner, "FirstScene", project.DefaultScene, owner.GetAllocator());
        SetAssetReference(owner, "GameCooking", buildSettingsId, owner.GetAllocator());
        SetAssetReference(owner, "AssetPipeline", pipelineSettingsId, owner.GetAllocator());
        if (CreateSettingsSource(project, settings / TEXT("Project Settings.json"), projectSettingsId,
            TEXT("FlaxEditor.Content.Settings.GameSettings"), owner))
            return true;

        JsonDocument build;
        build.SetObject();
        SetString(build, "GameTarget", project.GameTarget, build.GetAllocator());
        SetString(build, "EditorTarget", project.EditorTarget, build.GetAllocator());
        if (CreateSettingsSource(project, settings / TEXT("Build Settings.json"), buildSettingsId,
            TEXT("FlaxEditor.Content.Settings.BuildSettings"), build))
            return true;

        JsonDocument pipeline;
        pipeline.SetObject();
        if (CreateSettingsSource(project, settings / TEXT("Asset Pipeline Settings.json"), pipelineSettingsId,
            TEXT("FlaxEditor.Content.Settings.AssetPipelineSettings"), pipeline))
            return true;

        JsonDocument editor;
        editor.SetObject();
        SetRay(editor, "DefaultSceneSpawn", project.DefaultSceneSpawn, editor.GetAllocator());
        if (CreateSettingsSource(project, settings / TEXT("Editor Settings.json"), editorSettingsId,
            TEXT("FlaxEditor.Content.Settings.AssetEditorSettings"), editor))
            return true;

        const String mountsPath = settings / TEXT("Content Mounts.json");
        const String mountsMetaPath = mountsPath + TEXT(".meta");
        if (!FileSystem::FileExists(mountsPath) && !FileSystem::FileExists(mountsMetaPath))
        {
            AssetMountSourceDescriptor engine;
            engine.MountId = NewProjectRoleId(project.ProjectId, "engine-content-mount");
            engine.LogicalPrefix = TEXT("EngineContent");
            engine.Root = TEXT("$(EnginePath)/Content");
            engine.Kind = AssetMountKind::EngineContent;
            Array<AssetMountSourceDescriptor> descriptors;
            descriptors.Add(MoveTemp(engine));
            StringAnsi source;
            AssetPipelineDiagnostic diagnostic;
            if (AssetMountDescriptorCodec::Write(mountSettingsId, descriptors, source, diagnostic))
                return true;
            JsonDocument mounts;
            mounts.Parse(source.Get(), source.Length());
            StringAnsi canonicalMounts;
            if (mounts.HasParseError() || SerializeSettings(mounts, canonicalMounts))
                return true;
            AssetMeta meta;
            meta.ID = mountSettingsId;
            meta.AssetType = AssetMountDescriptorCodec::TypeName;
            meta.SourceKind = AssetSourceKind::ExistingJson;
            meta.Processor.ID = TEXT("Flax.ExistingJson");
            AssetMutationService service(project.ProjectFolderPath, content,
                project.ProjectFolderPath / TEXT("Library/AssetDatabase/MutationJournals"),
                project.ProjectFolderPath / TEXT("Library/AssetDatabase/Recovery"));
            BindProjectDatabaseCommit(project, service);
            AssetMutationResult result;
            if (service.CreateAsset(mountsPath, StringAnsiView(canonicalMounts), meta, result))
                return true;
        }
        return !FileSystem::FileExists(mountsPath) || !FileSystem::FileExists(mountsMetaPath);
    }

    bool SaveV3MutableSettings(const ProjectInfo& project)
    {
        const String settings = project.ProjectFolderPath / TEXT("Content/Settings");
        JsonDocument projectDocument;
        JsonValue* projectData;
        if (ReadSettingsData(settings / TEXT("Project Settings.json"), projectDocument, projectData))
            return true;
        SetString(*projectData, "ProductName", project.Name, projectDocument.GetAllocator());
        SetString(*projectData, "Version", project.Version.ToString(), projectDocument.GetAllocator());
        SetString(*projectData, "CompanyName", project.Company, projectDocument.GetAllocator());
        SetString(*projectData, "CopyrightNotice", project.Copyright, projectDocument.GetAllocator());
        SetAssetReference(*projectData, "FirstScene", project.DefaultScene, projectDocument.GetAllocator());
        if (WriteSettings(project, settings / TEXT("Project Settings.json"), projectDocument))
            return true;

        JsonDocument buildDocument;
        JsonValue* buildData;
        if (ReadSettingsData(settings / TEXT("Build Settings.json"), buildDocument, buildData))
            return true;
        SetString(*buildData, "GameTarget", project.GameTarget, buildDocument.GetAllocator());
        SetString(*buildData, "EditorTarget", project.EditorTarget, buildDocument.GetAllocator());
        if (WriteSettings(project, settings / TEXT("Build Settings.json"), buildDocument))
            return true;

        JsonDocument editorDocument;
        JsonValue* editorData;
        if (ReadSettingsData(settings / TEXT("Editor Settings.json"), editorDocument, editorData))
            return true;
        SetRay(*editorData, "DefaultSceneSpawn", project.DefaultSceneSpawn, editorDocument.GetAllocator());
        return WriteSettings(project, settings / TEXT("Editor Settings.json"), editorDocument);
    }

    bool LoadV3MutableSettings(ProjectInfo& project)
    {
        const String settings = project.ProjectFolderPath / TEXT("Content/Settings");
        JsonDocument document;
        JsonValue* data;
        if (ReadSettingsData(settings / TEXT("Project Settings.json"), document, data))
            return true;
        project.Name = JsonTools::GetString(*data, "ProductName", String::Empty);
        const String version = JsonTools::GetString(*data, "Version", TEXT("1.0"));
        Version::Parse(*version, &project.Version);
        project.Company = JsonTools::GetString(*data, "CompanyName", String::Empty);
        project.Copyright = JsonTools::GetString(*data, "CopyrightNotice", String::Empty);
        project.DefaultScene = GetAssetReference(*data, "FirstScene");

        if (ReadSettingsData(settings / TEXT("Build Settings.json"), document, data))
            return true;
        project.GameTarget = JsonTools::GetString(*data, "GameTarget", String::Empty);
        project.EditorTarget = JsonTools::GetString(*data, "EditorTarget", String::Empty);

        if (ReadSettingsData(settings / TEXT("Editor Settings.json"), document, data))
            return true;
        project.DefaultSceneSpawn = JsonTools::GetRay(*data, "DefaultSceneSpawn", Ray(Vector3::Zero, Vector3::Forward));
        return false;
    }
}

bool ProjectInfo::SaveProject()
{
    if (AssetSystemReadOnly)
    {
        LOG(Error, "Cannot save project descriptor because asset-system version {0} is not writable in this editor. Supported mutable version: {1}.", AssetSystemVersion, CurrentAssetSystemVersion);
        return true;
    }
    if (AssetSystemVersion == CurrentAssetSystemVersion &&
        (EnsureV3BootstrapSources(*this) || SaveV3MutableSettings(*this)))
    {
        LOG(Error, "Cannot save asset-system v3 mandatory project settings.");
        return true;
    }

    // Serialize object to Json
    rapidjson_flax::StringBuffer buffer;
    PrettyJsonWriter writerObj(buffer);
    auto& stream = *(JsonWriter*)&writerObj;
    stream.StartObject();
    {
        if (AssetSystemVersion < CurrentAssetSystemVersion)
        {
            stream.JKEY("Name");
            stream.String(Name);

            stream.JKEY("Version");
            stream.String(Version.ToString());
        }

        stream.JKEY("AssetSystemVersion");
        stream.Int(AssetSystemVersion);

        if (AssetSystemVersion == CurrentAssetSystemVersion)
        {
            stream.JKEY("ProjectId");
            stream.Guid(ProjectId);
        }

        stream.JKEY("SourceRoot");
        stream.String(SourceRoot);

        stream.JKEY("IdentityModel");
        stream.String(IdentityModel);

        stream.JKEY("ArtifactLayoutVersion");
        stream.Int(ArtifactLayoutVersion);

        stream.JKEY("SourceDocumentVersion");
        stream.Int(SourceDocumentVersion);

        if (AssetSystemVersion < CurrentAssetSystemVersion)
        {
            stream.JKEY("Company");
            stream.String(Company);

            stream.JKEY("Copyright");
            stream.String(Copyright);

            stream.JKEY("GameTarget");
            stream.String(GameTarget);

            stream.JKEY("EditorTarget");
            stream.String(EditorTarget);
        }

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

        if (AssetSystemVersion < CurrentAssetSystemVersion && DefaultScene.IsValid())
        {
            stream.JKEY("DefaultScene");
            stream.Guid(DefaultScene);
        }

        if (AssetSystemVersion < CurrentAssetSystemVersion && DefaultSceneSpawn != Ray(Vector3::Zero, Vector3::Forward))
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

    // Parse properties
    ProjectPath = projectPath;
    ProjectFolderPath = StringUtils::GetDirectoryName(projectPath);
    AssetSystemVersion = JsonTools::GetInt(document, "AssetSystemVersion", 0);
    ProjectId = JsonTools::GetGuid(document, "ProjectId");
    SourceRoot = JsonTools::GetString(document, "SourceRoot", String::Empty);
    IdentityModel = JsonTools::GetString(document, "IdentityModel", String::Empty);
    ArtifactLayoutVersion = JsonTools::GetInt(document, "ArtifactLayoutVersion", 0);
    SourceDocumentVersion = JsonTools::GetInt(document, "SourceDocumentVersion", 0);
    AssetSystemReadOnly = AssetSystemVersion != CurrentAssetSystemVersion;
    if (AssetSystemVersion < CurrentAssetSystemVersion)
    {
        Name = JsonTools::GetString(document, "Name", String::Empty);
        const auto versionMember = document.FindMember("Version");
        if (versionMember != document.MemberEnd())
        {
            auto& version = versionMember->value;
            if (version.IsString())
                Version::Parse(version.GetText(), &Version);
            else if (version.IsObject())
            {
                Version = ::Version(
                    JsonTools::GetInt(version, "Major", 0),
                    JsonTools::GetInt(version, "Minor", 0),
                    JsonTools::GetInt(version, "Build", -1),
                    JsonTools::GetInt(version, "Revision", -1));
            }
        }
        Company = JsonTools::GetString(document, "Company", String::Empty);
        Copyright = JsonTools::GetString(document, "Copyright", String::Empty);
        GameTarget = JsonTools::GetString(document, "GameTarget", String::Empty);
        EditorTarget = JsonTools::GetString(document, "EditorTarget", String::Empty);
        DefaultScene = JsonTools::GetGuid(document, "DefaultScene");
        DefaultSceneSpawn = JsonTools::GetRay(document, "DefaultSceneSpawn", Ray(Vector3::Zero, Vector3::Forward));
    }
    else if (AssetSystemVersion == CurrentAssetSystemVersion && LoadV3MutableSettings(*this))
    {
        ShowProjectLoadError(TEXT("Failed to load mandatory asset-system v3 Project, Build, or Editor settings."), projectPath);
        return true;
    }
    if (Version.Revision() == 0)
        Version = ::Version(Version.Major(), Version.Minor(), Version.Build());
    if (Version.Build() == 0 && Version.Revision() == -1)
        Version = ::Version(Version.Major(), Version.Minor());
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
    const auto minEngineVersionMember = document.FindMember("MinEngineVersion");
    if (minEngineVersionMember != document.MemberEnd())
    {
        auto& minEngineVersion = minEngineVersionMember->value;
        if (minEngineVersion.IsString())
        {
            Version::Parse(minEngineVersion.GetText(), &MinEngineVersion);
        }
        else if (minEngineVersionMember->value.IsObject())
        {
            MinEngineVersion = ::Version(
                JsonTools::GetInt(minEngineVersion, "Major", 0),
                JsonTools::GetInt(minEngineVersion, "Minor", 0),
                JsonTools::GetInt(minEngineVersion, "Build", 0));
        }
    }

    // Validate properties
    if (Name.Length() == 0)
    {
        ShowProjectLoadError(TEXT("Missing project name."), projectPath);
        return true;
    }
    String markerError;
    if (AssetSystemVersion == CurrentAssetSystemVersion && !ValidateAssetSystemMarker(markerError))
    {
        ShowProjectLoadError(*markerError, projectPath);
        return true;
    }

    return false;
}

bool ProjectInfo::ValidateAssetSystemMarker(String& error) const
{
    error = String::Empty;
    if (AssetSystemVersion < CurrentAssetSystemVersion)
    {
        error = String::Format(TEXT("Asset-system version {0} requires one-way migration to version {1}."), AssetSystemVersion, CurrentAssetSystemVersion);
        return false;
    }
    if (AssetSystemVersion > CurrentAssetSystemVersion)
    {
        error = String::Format(TEXT("Asset-system version {0} is newer than supported version {1}."), AssetSystemVersion, CurrentAssetSystemVersion);
        return false;
    }
    if (!ProjectId.IsValid())
    {
        error = TEXT("Asset-system v3 project identity is missing or invalid.");
        return false;
    }
    if (SourceRoot != TEXT("Content"))
    {
        error = TEXT("Asset-system source root must be exactly 'Content'.");
        return false;
    }
    if (IdentityModel != TEXT("guid-local-id"))
    {
        error = TEXT("Asset-system identity model must be exactly 'guid-local-id'.");
        return false;
    }
    if (ArtifactLayoutVersion != CurrentArtifactLayoutVersion)
    {
        error = String::Format(TEXT("Artifact layout version must be {0}."), CurrentArtifactLayoutVersion);
        return false;
    }
    if (SourceDocumentVersion != CurrentSourceDocumentVersion)
    {
        error = String::Format(TEXT("Source document version must be {0}."), CurrentSourceDocumentVersion);
        return false;
    }
    return true;
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
        Guid::Parse((const Char*)defaultScene, DefaultScene);
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
