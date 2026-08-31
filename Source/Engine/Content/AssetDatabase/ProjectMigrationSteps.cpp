// Copyright (c) Wojciech Figat. All rights reserved.

#include "ProjectMigrationSteps.h"
#include "AssetMeta.h"
#include "AssetDatabase.h"
#include "AssetMountDescriptor.h"
#include "AssetPath.h"
#include "LegacyAssetMigrator.h"
#include "MigrationInventory.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Documents/AuthoredSourceDocument.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    void AddDiagnostic(Array<AssetPipelineDiagnostic>& diagnostics, AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message, const StringView& remediation = StringView::Empty)
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        diagnostic.Remediation = remediation;
        diagnostics.Add(MoveTemp(diagnostic));
    }

    bool HasErrors(const Array<AssetPipelineDiagnostic>& diagnostics)
    {
        for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
        {
            if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
                return true;
        }
        return false;
    }

    Guid StableGuid(const StringView& projectIdentity, const char* role)
    {
        ContentHasher hasher;
        const StringAnsi domain("flax-project-migration-role-v1");
        const StringAnsi identity(projectIdentity);
        hasher.Update(domain.Get(), domain.Length());
        hasher.Update(identity.Get(), identity.Length());
        hasher.Update(role, StringUtils::Length(role));
        const ContentHash hash = hasher.Finalize();
        Guid result(hash.Values[0], hash.Values[1], hash.Values[2], hash.Values[3]);
        if (!result.IsValid())
            result.D = 1;
        return result;
    }

    bool ReadCanonicalJson(const StringView& path, JsonDocument& document, StringAnsi& canonical, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi source;
        if (File::ReadAllText(path, source))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = path;
            diagnostic.Message = TEXT("Migration cannot read a required authored source.");
            return true;
        }
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Canonicalize(source, canonical, error))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = path;
            diagnostic.Message = TEXT("Migration cannot normalize a required authored source.");
            return true;
        }
        document.Parse(canonical.Get(), canonical.Length());
        if (document.HasParseError() || !document.IsObject())
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = path;
            diagnostic.Message = TEXT("Migration requires a deterministic JSON object source.");
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool WriteAtomic(const StringView& path, const StringAnsiView& bytes, AssetPipelineDiagnostic& diagnostic)
    {
        const String destination(path);
        const String staging = destination + TEXT(".migration.tmp");
        const String folder(StringUtils::GetDirectoryName(destination));
        if ((!FileSystem::DirectoryExists(folder) && FileSystem::CreateDirectory(folder)) ||
            File::WriteAllBytes(staging, bytes.Get(), bytes.Length()))
        {
            FileSystem::DeleteFile(staging);
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = path;
            diagnostic.Message = TEXT("Migration staging write failed.");
            return true;
        }
        StringAnsi verify;
        if (File::ReadAllText(staging, verify) || verify != bytes || FileSystem::MoveFile(destination, staging, true))
        {
            FileSystem::DeleteFile(staging);
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = path;
            diagnostic.Message = TEXT("Migration atomic replacement failed verification.");
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool EnsureMeta(const StringView& sourcePath, const Guid& id, const StringView& typeName, bool folder, AssetPipelineDiagnostic& diagnostic)
    {
        const String metaPath = String(sourcePath) + TEXT(".meta");
        if (FileSystem::FileExists(metaPath))
            return false;
        AssetMeta meta;
        meta.ID = id;
        meta.FolderAsset = folder;
        meta.AssetType = typeName;
        meta.SourceKind = folder ? AssetSourceKind::Folder : AssetSourceKind::ExistingJson;
        meta.Processor.ID = folder ? TEXT("Flax.Folder") : TEXT("Flax.ExistingJson");
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        return AssetMeta::SaveAtomic(metaPath, meta, diagnostic);
    }

    void FindRoleSources(const StringView& contentRoot, const StringView& typeName, Array<String>& sources, Array<AssetPipelineDiagnostic>& diagnostics)
    {
        Array<String> metas;
        FileSystem::DirectoryGetFiles(metas, String(contentRoot), TEXT("*.meta"), DirectorySearchOption::AllDirectories);
        if (metas.Count() > 1)
            std::sort(metas.Get(), metas.Get() + metas.Count());
        for (const String& metaPath : metas)
        {
            AssetMeta meta;
            AssetPipelineDiagnostic diagnostic;
            if (AssetMeta::Load(metaPath, meta, diagnostic))
            {
                diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
            if (!meta.FolderAsset && meta.AssetType == typeName)
                sources.Add(metaPath.Substring(0, metaPath.Length() - 5));
        }
    }

    bool MovePair(const StringView& sourcePath, const StringView& destinationPath, AssetPipelineDiagnostic& diagnostic)
    {
        const String source(sourcePath);
        const String destination(destinationPath);
        if (source == destination)
            return false;
        const String folder(StringUtils::GetDirectoryName(destination));
        if (!FileSystem::DirectoryExists(folder) && FileSystem::CreateDirectory(folder))
            return true;
        if (FileSystem::FileExists(source) && FileSystem::FileExists(destination))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = source;
            diagnostic.Message = TEXT("Migration role destination already exists.");
            return true;
        }
        if (FileSystem::FileExists(source) && FileSystem::MoveFile(destination, source))
            return true;
        const String sourceMeta = source + TEXT(".meta");
        const String destinationMeta = destination + TEXT(".meta");
        if (FileSystem::FileExists(sourceMeta) && !FileSystem::FileExists(destinationMeta) && FileSystem::MoveFile(destinationMeta, sourceMeta))
            return true;
        return !FileSystem::FileExists(destination) || !FileSystem::FileExists(destinationMeta);
    }

    bool EnsureUniqueRole(const StringView& contentRoot, const StringView& typeName, const StringView& destination,
        bool allowCreate, const Guid& createGuid, const StringAnsiView& createSource, Array<AssetPipelineDiagnostic>& diagnostics)
    {
        Array<String> sources;
        FindRoleSources(contentRoot, typeName, sources, diagnostics);
        if (sources.Count() > 1)
        {
            AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, contentRoot,
                TEXT("Migration cannot choose between duplicate mandatory settings roles."));
            return true;
        }
        AssetPipelineDiagnostic diagnostic;
        if (sources.Count() == 1)
        {
            if (MovePair(sources[0], destination, diagnostic))
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
                    diagnostic.SourcePath = sources[0];
                    diagnostic.Message = TEXT("Migration could not relocate a mandatory settings role.");
                }
                diagnostics.Add(MoveTemp(diagnostic));
                return true;
            }
            return false;
        }
        if (!allowCreate)
        {
            AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, contentRoot,
                String::Format(TEXT("Migration is missing mandatory settings role '{0}'."), typeName));
            return true;
        }
        if (WriteAtomic(destination, createSource, diagnostic) || EnsureMeta(destination, createGuid, typeName, false, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            return true;
        }
        return false;
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi ansi(value);
        object.AddMember(JsonValue(name, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
    }

    bool CreateSettingsSource(const Guid& id, const StringView& typeName, const JsonValue& data, StringAnsi& source)
    {
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        AddString(document, "ID", id.ToString(Guid::FormatType::N), allocator);
        AddString(document, "TypeName", typeName, allocator);
        JsonValue dataCopy(data, allocator);
        document.AddMember("Data", dataCopy, allocator);
        Array<StringAnsi> order;
        order.Add("ID");
        order.Add("TypeName");
        order.Add("Data");
        CanonicalJsonError error;
        return CanonicalJsonWriter::Write(document, source, error, &order);
    }

    void SetDataMember(JsonValue& data, const char* destinationName, const JsonValue& value, JsonAlloc& allocator)
    {
        const auto existing = data.FindMember(destinationName);
        if (existing == data.MemberEnd())
            data.AddMember(JsonValue(destinationName, allocator), JsonValue(value, allocator), allocator);
        else
            existing->value.CopyFrom(value, allocator);
    }

    void SetDataString(JsonValue& data, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi ansi(value);
        const auto existing = data.FindMember(name);
        if (existing == data.MemberEnd())
            data.AddMember(JsonValue(name, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
        else
            existing->value.SetString(ansi.Get(), ansi.Length(), allocator);
    }

    bool WriteSettingsDocument(const StringView& path, JsonDocument& document, AssetPipelineDiagnostic& diagnostic)
    {
        Array<StringAnsi> order;
        order.Add("ID");
        order.Add("TypeName");
        order.Add("EngineBuild");
        order.Add("Data");
        StringAnsi output;
        CanonicalJsonError error;
        return CanonicalJsonWriter::Write(document, output, error, &order) || WriteAtomic(path, output, diagnostic);
    }

    String ProjectIdentity(const JsonDocument& project)
    {
        const auto name = project.FindMember("Name");
        if (name != project.MemberEnd() && name->value.IsString())
            return String(StringAnsiView(name->value.GetString(), name->value.GetStringLength()));
        return TEXT("unnamed-project");
    }

    bool CollectDirectories(const StringView& root, Array<String>& directories)
    {
        Array<String> pending;
        pending.Add(String(root));
        for (int32 index = 0; index < pending.Count(); index++)
        {
            Array<String> children;
            if (FileSystem::GetChildDirectories(children, pending[index]))
                return true;
            for (String& child : children)
            {
                directories.Add(child);
                pending.Add(MoveTemp(child));
            }
        }
        return false;
    }

    bool DefaultMetaForSource(const StringView& sourcePath, AssetMeta& meta)
    {
        const String extension = FileSystem::GetExtension(sourcePath).ToLower();
        meta.ID = Guid::New();
        meta.FolderAsset = false;
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        if (extension == TEXT("png") || extension == TEXT("jpg") || extension == TEXT("jpeg") || extension == TEXT("tga") || extension == TEXT("bmp") || extension == TEXT("dds") || extension == TEXT("hdr") || extension == TEXT("exr"))
        {
            meta.AssetType = TEXT("FlaxEngine.Texture");
            meta.Processor.ID = TEXT("Flax.Texture");
        }
        else if (extension == TEXT("fbx") || extension == TEXT("obj") || extension == TEXT("gltf") || extension == TEXT("glb") || extension == TEXT("blend"))
        {
            meta.AssetType = TEXT("FlaxEngine.Model");
            meta.Processor.ID = TEXT("Flax.Model");
        }
        else if (extension == TEXT("wav") || extension == TEXT("mp3") || extension == TEXT("ogg"))
        {
            meta.AssetType = TEXT("FlaxEngine.AudioClip");
            meta.Processor.ID = TEXT("Flax.Audio");
        }
        else if (extension == TEXT("ttf") || extension == TEXT("otf"))
        {
            meta.AssetType = TEXT("FlaxEngine.FontAsset");
            meta.Processor.ID = TEXT("Flax.Font");
        }
        else if (extension == TEXT("txt"))
        {
            meta.AssetType = TEXT("FlaxEngine.RawDataAsset");
            meta.SourceKind = AssetSourceKind::TextDocument;
            meta.Processor.ID = TEXT("Flax.Text");
        }
        else
        {
            return true;
        }
        return false;
    }

    bool HasSiblingSource(const StringView& flaxPath)
    {
        const String folder(StringUtils::GetDirectoryName(flaxPath));
        const String stem(StringUtils::GetFileNameWithoutExtension(flaxPath));
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, folder, TEXT("*"), DirectorySearchOption::TopDirectoryOnly))
            return false;
        for (const String& file : files)
        {
            if (file == flaxPath || file.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase))
                continue;
            if (StringUtils::GetFileNameWithoutExtension(file).Compare(StringView(stem), StringSearchCase::IgnoreCase) == 0)
                return true;
        }
        return false;
    }

    void AddPathMapping(Dictionary<String, String>& mappings, const StringView& oldPath, const StringView& newPath)
    {
        String oldValue(oldPath);
        String newValue(newPath);
        oldValue.Replace('\\', '/');
        newValue.Replace('\\', '/');
        mappings[oldValue] = newValue;
        mappings[String(TEXT("Content/")) + oldValue] = String(TEXT("Content/")) + newValue;
    }

    bool RewriteJsonReferences(JsonValue& value, JsonAlloc& allocator, const Dictionary<Guid, AssetObjectId>& identities,
        const Dictionary<String, String>& paths, int32& objectReferences, int32& pathReferences)
    {
        if (value.IsArray())
        {
            for (JsonValue& item : value.GetArray())
                RewriteJsonReferences(item, allocator, identities, paths, objectReferences, pathReferences);
            return false;
        }
        if (!value.IsObject())
            return false;
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
        {
            JsonValue& child = member->value;
            if (child.IsString())
            {
                const String text(StringAnsiView(child.GetString(), child.GetStringLength()));
                String mappedPath;
                if (paths.TryGet(text, mappedPath))
                {
                    const StringAnsi mappedAnsi(mappedPath);
                    child.SetString(mappedAnsi.Get(), mappedAnsi.Length(), allocator);
                    pathReferences++;
                    continue;
                }
                Guid legacyGuid;
                AssetObjectId mappedObject;
                if (!Guid::Parse(text, legacyGuid) && identities.TryGet(legacyGuid, mappedObject) && mappedObject.LocalId > 1)
                {
                    child.SetObject();
                    const StringAnsi guid(mappedObject.Guid.ToString(Guid::FormatType::N).ToLower());
                    child.AddMember("Guid", JsonValue(guid.Get(), guid.Length(), allocator), allocator);
                    child.AddMember("LocalId", mappedObject.LocalId, allocator);
                    objectReferences++;
                    continue;
                }
            }
            RewriteJsonReferences(child, allocator, identities, paths, objectReferences, pathReferences);
        }
        return false;
    }

    bool BuildPhaseReport(const char* name, int32 files, int32 changed, int32 verified, StringAnsi& report)
    {
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        document.AddMember("schemaVersion", 1, allocator);
        document.AddMember("operation", JsonValue(name, allocator), allocator);
        document.AddMember("files", files, allocator);
        document.AddMember("changed", changed, allocator);
        document.AddMember("verified", verified, allocator);
        Array<StringAnsi> order;
        order.Add("schemaVersion");
        order.Add("operation");
        order.Add("files");
        order.Add("changed");
        order.Add("verified");
        CanonicalJsonError error;
        return CanonicalJsonWriter::Write(document, report, error, &order);
    }
}

bool ProjectMigrationSteps::EstablishCanonicalSettings(const StringView& projectDescriptorPath, const StringView& contentRoot,
    Array<AssetPipelineDiagnostic>& diagnostics)
{
    JsonDocument project;
    StringAnsi canonicalProject;
    AssetPipelineDiagnostic diagnostic;
    if (ReadCanonicalJson(projectDescriptorPath, project, canonicalProject, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    const String identity = ProjectIdentity(project);
    const String settingsRoot = String(contentRoot) / TEXT("Settings");
    if (!FileSystem::DirectoryExists(settingsRoot) && FileSystem::CreateDirectory(settingsRoot))
        return true;
    if (EnsureMeta(settingsRoot, StableGuid(identity, "settings-folder"), TEXT("FlaxEngine.Folder"), true, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }

    JsonDocument emptyData;
    emptyData.SetObject();
    StringAnsi projectSource;
    StringAnsi buildSource;
    StringAnsi pipelineSource;
    StringAnsi editorSource;
    const Guid projectGuid = StableGuid(identity, "project-settings");
    const Guid buildGuid = StableGuid(identity, "build-settings");
    const Guid pipelineGuid = StableGuid(identity, "asset-pipeline-settings");
    const Guid editorGuid = StableGuid(identity, "editor-settings");
    if (CreateSettingsSource(projectGuid, TEXT("FlaxEditor.Content.Settings.GameSettings"), emptyData, projectSource) ||
        CreateSettingsSource(buildGuid, TEXT("FlaxEditor.Content.Settings.BuildSettings"), emptyData, buildSource) ||
        CreateSettingsSource(pipelineGuid, TEXT("FlaxEditor.Content.Settings.AssetPipelineSettings"), emptyData, pipelineSource) ||
        CreateSettingsSource(editorGuid, TEXT("FlaxEditor.Content.Settings.AssetEditorSettings"), emptyData, editorSource))
        return true;
    bool failed = false;
    failed |= EnsureUniqueRole(contentRoot, TEXT("FlaxEditor.Content.Settings.GameSettings"), settingsRoot / TEXT("Project Settings.json"), true, projectGuid, projectSource, diagnostics);
    failed |= EnsureUniqueRole(contentRoot, TEXT("FlaxEditor.Content.Settings.BuildSettings"), settingsRoot / TEXT("Build Settings.json"), true, buildGuid, buildSource, diagnostics);
    failed |= EnsureUniqueRole(contentRoot, TEXT("FlaxEditor.Content.Settings.AssetPipelineSettings"), settingsRoot / TEXT("Asset Pipeline Settings.json"), true, pipelineGuid, pipelineSource, diagnostics);
    failed |= EnsureUniqueRole(contentRoot, TEXT("FlaxEditor.Content.Settings.AssetEditorSettings"), settingsRoot / TEXT("Editor Settings.json"), true, editorGuid, editorSource, diagnostics);
    if (failed)
        return true;

    const String pipelinePath = settingsRoot / TEXT("Asset Pipeline Settings.json");
    JsonDocument pipeline;
    StringAnsi canonicalPipeline;
    if (ReadCanonicalJson(pipelinePath, pipeline, canonicalPipeline, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    const auto data = pipeline.FindMember("Data");
    if (data == pipeline.MemberEnd() || !data->value.IsObject())
        return true;
    data->value.RemoveMember("AssetSystemVersion");
    Array<StringAnsi> pipelineOrder;
    pipelineOrder.Add("ID");
    pipelineOrder.Add("TypeName");
    pipelineOrder.Add("EngineBuild");
    pipelineOrder.Add("Data");
    CanonicalJsonError canonicalError;
    StringAnsi normalizedPipeline;
    if (CanonicalJsonWriter::Write(pipeline, normalizedPipeline, canonicalError, &pipelineOrder) || WriteAtomic(pipelinePath, normalizedPipeline, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }

    // The v3 descriptor is bootstrap-only. Move all mutable project/editor/build values
    // into their mandatory authored settings owners before the marker is committed.
    const String projectSettingsPath = settingsRoot / TEXT("Project Settings.json");
    JsonDocument projectSettings;
    StringAnsi canonicalSettings;
    if (ReadCanonicalJson(projectSettingsPath, projectSettings, canonicalSettings, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    auto projectDataMember = projectSettings.FindMember("Data");
    if (projectDataMember == projectSettings.MemberEnd() || !projectDataMember->value.IsObject())
        return true;
    JsonValue& projectData = projectDataMember->value;
    const struct { const char* Source; const char* Destination; } projectFields[] =
    {
        { "Name", "ProductName" },
        { "Version", "Version" },
        { "Company", "CompanyName" },
        { "Copyright", "CopyrightNotice" },
        { "DefaultScene", "FirstScene" },
    };
    for (const auto& field : projectFields)
    {
        const auto value = project.FindMember(field.Source);
        if (value != project.MemberEnd())
            SetDataMember(projectData, field.Destination, value->value, projectSettings.GetAllocator());
    }
    if (projectData.FindMember("ProductName") == projectData.MemberEnd())
        SetDataString(projectData, "ProductName", identity, projectSettings.GetAllocator());
    if (projectData.FindMember("Version") == projectData.MemberEnd())
        SetDataString(projectData, "Version", TEXT("1.0"), projectSettings.GetAllocator());
    AssetMeta canonicalBuildMeta;
    AssetMeta canonicalPipelineMeta;
    if (AssetMeta::Load((settingsRoot / TEXT("Build Settings.json.meta")), canonicalBuildMeta, diagnostic) ||
        AssetMeta::Load((settingsRoot / TEXT("Asset Pipeline Settings.json.meta")), canonicalPipelineMeta, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    SetDataString(projectData, "GameCooking", canonicalBuildMeta.ID.ToString(Guid::FormatType::N), projectSettings.GetAllocator());
    SetDataString(projectData, "AssetPipeline", canonicalPipelineMeta.ID.ToString(Guid::FormatType::N), projectSettings.GetAllocator());
    if (WriteSettingsDocument(projectSettingsPath, projectSettings, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }

    const String buildSettingsPath = settingsRoot / TEXT("Build Settings.json");
    JsonDocument buildSettings;
    if (ReadCanonicalJson(buildSettingsPath, buildSettings, canonicalSettings, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    auto buildDataMember = buildSettings.FindMember("Data");
    if (buildDataMember == buildSettings.MemberEnd() || !buildDataMember->value.IsObject())
        return true;
    for (const char* field : { "GameTarget", "EditorTarget" })
    {
        const auto value = project.FindMember(field);
        if (value != project.MemberEnd())
            SetDataMember(buildDataMember->value, field, value->value, buildSettings.GetAllocator());
    }
    if (WriteSettingsDocument(buildSettingsPath, buildSettings, diagnostic))
    {
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }

    const auto spawn = project.FindMember("DefaultSceneSpawn");
    if (spawn != project.MemberEnd())
    {
        const String editorSettingsPath = settingsRoot / TEXT("Editor Settings.json");
        JsonDocument editorSettings;
        if (ReadCanonicalJson(editorSettingsPath, editorSettings, canonicalSettings, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            return true;
        }
        auto editorData = editorSettings.FindMember("Data");
        if (editorData == editorSettings.MemberEnd() || !editorData->value.IsObject())
            return true;
        SetDataMember(editorData->value, "DefaultSceneSpawn", spawn->value, editorSettings.GetAllocator());
        if (WriteSettingsDocument(editorSettingsPath, editorSettings, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            return true;
        }
    }

    Array<AssetMountSourceDescriptor> descriptors;
    const auto references = project.FindMember("References");
    if (references != project.MemberEnd() && references->value.IsArray())
    {
        for (const JsonValue& reference : references->value.GetArray())
        {
            if (!reference.IsObject())
                continue;
            const auto nameMember = reference.FindMember("Name");
            if (nameMember == reference.MemberEnd() || !nameMember->value.IsString())
                continue;
            const String name(StringAnsiView(nameMember->value.GetString(), nameMember->value.GetStringLength()));
            AssetMountSourceDescriptor descriptor;
            const StringAnsi stableName(name);
            descriptor.MountId = StableGuid(identity, stableName.Get());
            if (name.StartsWith(TEXT("$(EnginePath)")))
            {
                descriptor.LogicalPrefix = TEXT("EngineContent");
                descriptor.Root = TEXT("$(EnginePath)/Content");
                descriptor.Kind = AssetMountKind::EngineContent;
            }
            else if (name.StartsWith(TEXT("$(ProjectPath)")))
            {
                const String projectFile = String(StringUtils::GetDirectoryName(projectDescriptorPath)) / name.Substring(15);
                const String referencedRoot = String(StringUtils::GetDirectoryName(projectFile)) / TEXT("Content");
                if (!FileSystem::DirectoryExists(referencedRoot))
                    continue;
                const String suffix = String(ContentHash::Compute(*name, name.Length() * sizeof(Char)).ToString()).Substring(0, 16);
                const bool plugin = name.Contains(TEXT("/Plugins/"), StringSearchCase::IgnoreCase);
                descriptor.LogicalPrefix = (plugin ? TEXT("PluginContent/") : TEXT("ExternalContent/")) + suffix;
                descriptor.Root = String(StringUtils::GetDirectoryName(name)) + TEXT("/Content");
                descriptor.Kind = plugin ? AssetMountKind::PluginContent : AssetMountKind::ExternalReadOnlyContent;
            }
            else
            {
                continue;
            }
            bool duplicate = false;
            for (const AssetMountSourceDescriptor& existing : descriptors)
            {
                if (existing.LogicalPrefix == descriptor.LogicalPrefix)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                descriptors.Add(MoveTemp(descriptor));
        }
    }
    if (descriptors.Count() > 1)
    {
        std::sort(descriptors.Get(), descriptors.Get() + descriptors.Count(), [](const AssetMountSourceDescriptor& a, const AssetMountSourceDescriptor& b)
        {
            return a.LogicalPrefix < b.LogicalPrefix;
        });
    }
    Array<String> mountSources;
    FindRoleSources(contentRoot, AssetMountDescriptorCodec::TypeName, mountSources, diagnostics);
    if (mountSources.Count() > 1)
    {
        AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, settingsRoot,
            TEXT("Migration cannot choose between duplicate content mount settings roles."));
        return true;
    }
    Guid mountsGuid = StableGuid(identity, "content-mount-settings");
    if (mountSources.Count() == 1)
    {
        AssetMeta mountMeta;
        if (AssetMeta::Load(mountSources[0] + TEXT(".meta"), mountMeta, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            return true;
        }
        mountsGuid = mountMeta.ID;
    }
    StringAnsi mountSource;
    const String mountDestination = settingsRoot / TEXT("Content Mounts.json");
    if (AssetMountDescriptorCodec::Write(mountsGuid, descriptors, mountSource, diagnostic) ||
        (mountSources.Count() == 1 && MovePair(mountSources[0], mountDestination, diagnostic)) ||
        WriteAtomic(mountDestination, mountSource, diagnostic) ||
        EnsureMeta(mountDestination, mountsGuid, AssetMountDescriptorCodec::TypeName, false, diagnostic))
    {
        if (diagnostic.Code != AssetPipelineDiagnosticCode::None)
            diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    return false;
}

bool ProjectMigrationSteps::ClassifyAndConvertLegacyAssets(const StringView& contentRoot, const StringView& quarantineRoot,
    Array<AssetPipelineDiagnostic>& diagnostics)
{
    Array<String> legacy;
    if (FileSystem::DirectoryGetFiles(legacy, String(contentRoot), TEXT("*.flax"), DirectorySearchOption::AllDirectories))
        return true;
    if (legacy.Count() > 1)
        std::sort(legacy.Get(), legacy.Get() + legacy.Count());
    for (const String& path : legacy)
    {
        Array<FlaxStorage::Entry> entries;
        {
            const FlaxStorageReference storage = ContentStorageManager::GetStorage(path, true);
            if (!storage || storage->GetEntriesCount() == 0)
            {
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path, TEXT("Legacy cooked asset header is unreadable."));
                continue;
            }
            entries.Resize(storage->GetEntriesCount());
            for (int32 i = 0; i < entries.Count(); i++)
                storage->GetEntry(i, entries[i]);
        }
        ContentStorageManager::EnsureAccess(path);
        String relative = FileSystem::ConvertAbsolutePathToRelative(contentRoot, path);
        relative.Replace('\\', '/');
        const bool generated = StringUtils::GetFileNameWithoutExtension(path).StartsWith(TEXT("CSG_")) ||
            relative.Contains(TEXT("/SceneData/Terrain/"), StringSearchCase::IgnoreCase) || HasSiblingSource(path);
        if (generated)
        {
            const String destination = String(quarantineRoot) / relative;
            const String folder(StringUtils::GetDirectoryName(destination));
            if ((!FileSystem::DirectoryExists(folder) && FileSystem::CreateDirectory(folder)) || FileSystem::MoveFile(destination, path))
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path, TEXT("Generated legacy output could not be quarantined."));
            const String meta = path + TEXT(".meta");
            if (FileSystem::FileExists(meta))
                FileSystem::MoveFile(destination + TEXT(".meta"), meta);
            continue;
        }
        AssetRecord record;
        record.ID = entries[0].ID;
        record.SourceAssetID = entries[0].ID;
        record.TypeName = entries[0].TypeName;
        record.SourcePath = SourceFilePath(path);
        record.SourceKind = AssetSourceKind::LegacyBinary;
        String reason;
        String destination;
        const MigrationEligibility eligibility = MigrationInventory::Classify(record, reason, destination);
        if (eligibility != MigrationEligibility::ReadyToMigrate)
        {
            AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path, reason,
                TEXT("Provide an explicit lossless converter or recover the original source before resuming migration."));
            continue;
        }
        AssetPipelineDiagnostic diagnostic;
        if (LegacyAssetMigrator::ConvertFlax(path, destination, record.ID, record.TypeName, diagnostic) || FileSystem::DeleteFile(path))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        const String oldMeta = path + TEXT(".meta");
        if (FileSystem::FileExists(oldMeta))
            FileSystem::DeleteFile(oldMeta);
    }
    for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
            return true;
    }
    return false;
}

bool ProjectMigrationSteps::CompleteAndUpgradeMetadata(const StringView& projectRoot, const StringView& contentRoot,
    Array<AssetPipelineDiagnostic>& diagnostics)
{
    (void)projectRoot;
    Array<String> files;
    Array<String> directories;
    if (FileSystem::DirectoryGetFiles(files, String(contentRoot), TEXT("*"), DirectorySearchOption::AllDirectories) || CollectDirectories(contentRoot, directories))
        return true;
    files.Add(directories);
    for (const String& sourcePath : files)
    {
        if (sourcePath.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase))
            continue;
        const bool folder = FileSystem::DirectoryExists(sourcePath);
        const String metaPath = sourcePath + TEXT(".meta");
        AssetPipelineDiagnostic diagnostic;
        if (FileSystem::FileExists(metaPath))
        {
            AssetMeta meta;
            if (AssetMeta::Load(metaPath, meta, diagnostic))
            {
                diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
            if (meta.MetaUpgradeRequired && AssetMeta::SaveAtomic(metaPath, meta, diagnostic))
                diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        AssetMeta meta;
        if (folder)
        {
            meta.ID = Guid::New();
            meta.FolderAsset = true;
            meta.AssetType = TEXT("FlaxEngine.Folder");
            meta.SourceKind = AssetSourceKind::Folder;
            meta.Processor.ID = TEXT("Flax.Folder");
        }
        else
        {
            const String extension = FileSystem::GetExtension(sourcePath).ToLower();
            if (extension == TEXT("json") || extension == TEXT("scene") || extension == TEXT("prefab"))
            {
                JsonDocument document;
                StringAnsi canonical;
                if (ReadCanonicalJson(sourcePath, document, canonical, diagnostic))
                {
                    diagnostics.Add(MoveTemp(diagnostic));
                    continue;
                }
                const auto id = document.FindMember("ID");
                const auto type = document.FindMember("TypeName");
                if (id == document.MemberEnd() || !id->value.IsString() || Guid::Parse(StringAnsiView(id->value.GetString(), id->value.GetStringLength()), meta.ID) ||
                    type == document.MemberEnd() || !type->value.IsString())
                {
                    AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, sourcePath, TEXT("Authored JSON source has no stable ID or TypeName."));
                    continue;
                }
                meta.AssetType = String(StringAnsiView(type->value.GetString(), type->value.GetStringLength()));
                meta.SourceKind = AssetSourceKind::ExistingJson;
                meta.Processor.ID = TEXT("Flax.ExistingJson");
            }
            else if (DefaultMetaForSource(sourcePath, meta))
            {
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, sourcePath,
                    TEXT("No deterministic metadata/importer mapping exists for this source extension."));
                continue;
            }
        }
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic))
            diagnostics.Add(MoveTemp(diagnostic));
    }
    for (const AssetPipelineDiagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
            return true;
    }
    return false;
}

bool ProjectMigrationSteps::RewriteAndVerifySerializedReferences(const StringView& contentRoot, const StringView& legacyPreimageRoot,
    StringAnsi& report, Array<AssetPipelineDiagnostic>& diagnostics)
{
    Dictionary<Guid, String> currentPaths;
    Array<String> currentMetas;
    FileSystem::DirectoryGetFiles(currentMetas, String(contentRoot), TEXT("*.meta"), DirectorySearchOption::AllDirectories);
    for (const String& metaPath : currentMetas)
    {
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(metaPath, meta, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        String relative = FileSystem::ConvertAbsolutePathToRelative(contentRoot, metaPath.Substring(0, metaPath.Length() - 5));
        relative.Replace('\\', '/');
        currentPaths[meta.ID] = MoveTemp(relative);
    }

    Dictionary<Guid, AssetObjectId> identities;
    Dictionary<String, String> pathMappings;
    Array<String> legacyMetas;
    FileSystem::DirectoryGetFiles(legacyMetas, String(legacyPreimageRoot), TEXT("*.meta"), DirectorySearchOption::AllDirectories);
    for (const String& metaPath : legacyMetas)
    {
        JsonDocument document;
        StringAnsi canonical;
        AssetPipelineDiagnostic diagnostic;
        if (ReadCanonicalJson(metaPath, document, canonical, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        const auto guidMember = document.FindMember("guid");
        if (guidMember == document.MemberEnd() || !guidMember->value.IsString())
            continue;
        Guid rootGuid;
        if (Guid::Parse(StringAnsiView(guidMember->value.GetString(), guidMember->value.GetStringLength()), rootGuid))
            continue;
        identities[rootGuid] = AssetObjectId(rootGuid, 1);
        String currentRelative;
        if (currentPaths.TryGet(rootGuid, currentRelative))
        {
            String oldRelative = FileSystem::ConvertAbsolutePathToRelative(legacyPreimageRoot, metaPath.Substring(0, metaPath.Length() - 5));
            oldRelative.Replace('\\', '/');
            AddPathMapping(pathMappings, oldRelative, currentRelative);
        }
        auto objects = document.FindMember("subAssets");
        if (objects == document.MemberEnd())
            objects = document.FindMember("objects");
        if (objects == document.MemberEnd() || !objects->value.IsObject())
            continue;
        for (auto entry = objects->value.MemberBegin(); entry != objects->value.MemberEnd(); ++entry)
        {
            if (!entry->value.IsObject())
                continue;
            const auto oldGuid = entry->value.FindMember("guid");
            const auto localId = entry->value.FindMember("localId");
            if (oldGuid == entry->value.MemberEnd() || !oldGuid->value.IsString() ||
                localId == entry->value.MemberEnd() || !localId->value.IsInt64())
                continue;
            Guid legacyGuid;
            if (!Guid::Parse(StringAnsiView(oldGuid->value.GetString(), oldGuid->value.GetStringLength()), legacyGuid))
                identities[legacyGuid] = AssetObjectId(rootGuid, localId->value.GetInt64());
        }
    }

    Array<String> files;
    FileSystem::DirectoryGetFiles(files, String(contentRoot), TEXT("*"), DirectorySearchOption::AllDirectories);
    int32 inspected = 0;
    int32 changed = 0;
    int32 rewrittenObjects = 0;
    int32 rewrittenPaths = 0;
    for (const String& path : files)
    {
        if (path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase))
            continue;
        const String extension = FileSystem::GetExtension(path).ToLower();
        if (extension != TEXT("json") && extension != TEXT("scene") && extension != TEXT("prefab") &&
            extension != TEXT("material") && extension != TEXT("anim") && extension != TEXT("particle") &&
            extension != TEXT("terrain") && extension != TEXT("shader"))
            continue;
        JsonDocument document;
        StringAnsi canonical;
        AssetPipelineDiagnostic diagnostic;
        if (ReadCanonicalJson(path, document, canonical, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        inspected++;
        const int32 beforeObjects = rewrittenObjects;
        const int32 beforePaths = rewrittenPaths;
        RewriteJsonReferences(document, document.GetAllocator(), identities, pathMappings, rewrittenObjects, rewrittenPaths);
        StringAnsi rewritten;
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(document, rewritten, error))
        {
            AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path, TEXT("M5 cannot serialize rewritten references."));
            continue;
        }
        if ((beforeObjects != rewrittenObjects || beforePaths != rewrittenPaths) && rewritten != canonical)
        {
            if (WriteAtomic(path, rewritten, diagnostic))
                diagnostics.Add(MoveTemp(diagnostic));
            else
                changed++;
        }
    }
    if (HasErrors(diagnostics))
        return true;
    JsonDocument reportDocument;
    reportDocument.SetObject();
    JsonAlloc& allocator = reportDocument.GetAllocator();
    reportDocument.AddMember("schemaVersion", 1, allocator);
    reportDocument.AddMember("filesInspected", inspected, allocator);
    reportDocument.AddMember("filesChanged", changed, allocator);
    reportDocument.AddMember("objectReferencesRewritten", rewrittenObjects, allocator);
    reportDocument.AddMember("pathReferencesRewritten", rewrittenPaths, allocator);
    CanonicalJsonError error;
    return CanonicalJsonWriter::Write(reportDocument, report, error);
}

bool ProjectMigrationSteps::CanonicalizeAndVerifyAuthoredSources(const StringView& contentRoot, bool writeChanges,
    StringAnsi& report, Array<AssetPipelineDiagnostic>& diagnostics)
{
    Array<String> metas;
    FileSystem::DirectoryGetFiles(metas, String(contentRoot), TEXT("*.meta"), DirectorySearchOption::AllDirectories);
    if (metas.Count() > 1)
        std::sort(metas.Get(), metas.Get() + metas.Count());
    int32 inspected = 0;
    int32 changed = 0;
    int32 verified = 0;
    for (const String& metaPath : metas)
    {
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(metaPath, meta, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        if (meta.FolderAsset || (meta.SourceKind != AssetSourceKind::ExistingJson && meta.Processor.ID != TEXT("Flax.GraphDocument") &&
            meta.Processor.ID != TEXT("Flax.AuthoredObject")))
            continue;
        const String path = metaPath.Substring(0, metaPath.Length() - 5);
        JsonDocument document;
        StringAnsi canonical;
        if (ReadCanonicalJson(path, document, canonical, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        inspected++;
        const auto sourceId = document.FindMember("ID");
        if (sourceId != document.MemberEnd())
        {
            Guid id;
            if (!sourceId->value.IsString() || Guid::Parse(StringAnsiView(sourceId->value.GetString(), sourceId->value.GetStringLength()), id) || id != meta.ID)
            {
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path,
                    TEXT("Authored source identity does not match its metadata file GUID."));
                continue;
            }
        }

        StringAnsi serialized = canonical;
        if (meta.Processor.ID == TEXT("Flax.GraphDocument"))
        {
            GraphDocumentCodec codec;
            GraphDocumentSnapshot snapshot;
            if (codec.DecodeGraph(canonical, snapshot, diagnostic) ||
                GraphDocumentValidator::ValidateDocument(snapshot.Document, diagnostic) ||
                GraphDocumentCodec::ToCanonicalJson(snapshot.Document, serialized, diagnostic))
            {
                diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
            GraphDocumentSnapshot reparsed;
            if (codec.DecodeGraph(serialized, reparsed, diagnostic) || GraphDocumentValidator::ValidateDocument(reparsed.Document, diagnostic))
            {
                diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
        }
        else if (meta.Processor.ID == TEXT("Flax.AuthoredObject"))
        {
            AuthoredSourceDocument authored;
            String error;
            if (AuthoredSourceDocument::Parse(canonical, authored, error) || authored.ToCanonicalJson(serialized, error))
            {
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path, error);
                continue;
            }
            AuthoredSourceDocument reparsed;
            if (AuthoredSourceDocument::Parse(serialized, reparsed, error))
            {
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path, error);
                continue;
            }
            StringAnsi roundTrip;
            if (reparsed.ToCanonicalJson(roundTrip, error) || roundTrip != serialized)
            {
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path,
                    TEXT("Authored source is not stable across a canonical parse/serialize round trip."));
                continue;
            }
        }
        JsonDocument reparsedJson;
        reparsedJson.Parse(serialized.Get(), serialized.Length());
        if (reparsedJson.HasParseError() || !reparsedJson.IsObject())
        {
            AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, path,
                TEXT("Canonical authored source cannot be reparsed."));
            continue;
        }
        if (writeChanges && serialized != canonical)
        {
            if (WriteAtomic(path, serialized, diagnostic))
            {
                diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
            changed++;
        }
        verified++;
    }
    if (HasErrors(diagnostics))
        return true;
    return BuildPhaseReport(writeChanges ? "canonicalize-authored-sources" : "verify-authored-sources",
        inspected, changed, verified, report);
}

bool ProjectMigrationSteps::VerifyImportedDatabase(const StringView& contentRoot, StringAnsi& report,
    Array<AssetPipelineDiagnostic>& diagnostics)
{
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    HashSet<Guid> records;
    for (const AssetRecord& record : snapshot.Records)
        records.Add(record.ID);
    int32 dependencyCount = 0;
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.Status != AssetRecordStatus::Ready)
        {
            AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, record.SourcePath.Get(),
                String::Format(TEXT("Imported database record is not ready (status {0})."), static_cast<int32>(record.Status)));
            continue;
        }
        for (const Guid& dependency : record.BuildInputDependencies)
        {
            dependencyCount++;
            if (!records.Contains(dependency))
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, record.SourcePath.Get(),
                    TEXT("Imported build-input dependency does not resolve in the clean database snapshot."));
        }
        for (const Guid& reference : record.RuntimeReferences)
        {
            dependencyCount++;
            if (!records.Contains(reference))
                AddDiagnostic(diagnostics, AssetPipelineDiagnosticCode::MigrationFailed, record.SourcePath.Get(),
                    TEXT("Imported runtime reference does not resolve in the clean database snapshot."));
        }
    }
    StringAnsi authoredReport;
    if (CanonicalizeAndVerifyAuthoredSources(contentRoot, false, authoredReport, diagnostics) || HasErrors(diagnostics))
        return true;
    JsonDocument document;
    document.SetObject();
    JsonAlloc& allocator = document.GetAllocator();
    document.AddMember("schemaVersion", 1, allocator);
    document.AddMember("hostCookSucceeded", true, allocator);
    document.AddMember("databaseRevision", snapshot.Revision, allocator);
    document.AddMember("recordsVerified", snapshot.Records.Count(), allocator);
    document.AddMember("referencesVerified", dependencyCount, allocator);
    JsonDocument authored;
    authored.Parse(authoredReport.Get(), authoredReport.Length());
    document.AddMember("authoredSources", JsonValue(authored, allocator), allocator);
    CanonicalJsonError error;
    return CanonicalJsonWriter::Write(document, report, error);
}

bool ProjectMigrationSteps::WriteCandidateProjectMarker(const StringView& projectDescriptorPath, const StringView& outputPath,
    String& fingerprint, AssetPipelineDiagnostic& diagnostic)
{
    JsonDocument project;
    StringAnsi canonical;
    if (ReadCanonicalJson(projectDescriptorPath, project, canonical, diagnostic))
        return true;
    auto setInt = [&project](const char* name, int value)
    {
        const auto member = project.FindMember(name);
        if (member == project.MemberEnd())
            project.AddMember(JsonValue(name, project.GetAllocator()), value, project.GetAllocator());
        else
            member->value.SetInt(value);
    };
    auto setString = [&project](const char* name, const char* value)
    {
        const auto member = project.FindMember(name);
        if (member == project.MemberEnd())
            project.AddMember(JsonValue(name, project.GetAllocator()), JsonValue(value, project.GetAllocator()), project.GetAllocator());
        else
            member->value.SetString(value, project.GetAllocator());
    };
    setInt("AssetSystemVersion", 3);
    const String projectId = StableGuid(ProjectIdentity(project), "project-identity").ToString(Guid::FormatType::N);
    const StringAnsi projectIdAnsi(projectId);
    setString("ProjectId", projectIdAnsi.Get());
    setString("SourceRoot", "Content");
    setString("IdentityModel", "guid-local-id");
    setInt("ArtifactLayoutVersion", 2);
    setInt("SourceDocumentVersion", 1);
    Array<StringAnsi> order;
    const char* mutableFields[] =
    {
        "Name", "Version", "Company", "Copyright", "GameTarget", "EditorTarget", "DefaultScene", "DefaultSceneSpawn",
    };
    for (const char* field : mutableFields)
        project.RemoveMember(field);
    order.Add("ProjectId");
    order.Add("AssetSystemVersion");
    order.Add("SourceRoot");
    order.Add("IdentityModel");
    order.Add("ArtifactLayoutVersion");
    order.Add("SourceDocumentVersion");
    order.Add("MinEngineVersion");
    order.Add("EngineNickname");
    order.Add("References");
    CanonicalJsonError error;
    StringAnsi output;
    if (CanonicalJsonWriter::Write(project, output, error, &order) || WriteAtomic(outputPath, output, diagnostic))
        return true;
    fingerprint = String(ContentHash::Compute(output.Get(), output.Length()).ToString());
    return false;
}
