// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetPipelineBootstrap.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetDatabase/AssetMount.h"
#include "Engine/Content/AssetDatabase/AssetMountDescriptor.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    struct MandatoryRoleDescriptor
    {
        AssetSettingsRole Role;
        const Char* Name;
        const Char* TypeName;
    };

    const MandatoryRoleDescriptor MandatoryRoles[] =
    {
        { AssetSettingsRole::Project, TEXT("ProjectSettings"), TEXT("FlaxEditor.Content.Settings.GameSettings") },
        { AssetSettingsRole::Editor, TEXT("EditorSettings"), TEXT("FlaxEditor.Content.Settings.AssetEditorSettings") },
        { AssetSettingsRole::Build, TEXT("BuildSettings"), TEXT("FlaxEditor.Content.Settings.BuildSettings") },
        { AssetSettingsRole::AssetPipeline, TEXT("AssetPipelineSettings"), TEXT("FlaxEditor.Content.Settings.AssetPipelineSettings") },
    };

    int32 FindMandatoryRole(const StringView& typeName)
    {
        for (int32 i = 0; i < ARRAY_COUNT(MandatoryRoles); i++)
        {
            if (typeName == MandatoryRoles[i].TypeName)
                return i;
        }
        return -1;
    }

    bool ResolveProjectReference(const StringView& reference, const StringView& projectRoot, const StringView& engineRoot, String& projectFile)
    {
        if (reference.StartsWith(TEXT("$(EnginePath)")))
            projectFile = String(engineRoot) / reference.Substring(14);
        else if (reference.StartsWith(TEXT("$(ProjectPath)")))
            projectFile = String(projectRoot) / reference.Substring(15);
        else
            return true;
        StringUtils::PathRemoveRelativeParts(projectFile);
        return false;
    }

    void AddDiagnostic(AssetPipelineBootstrapSnapshot& snapshot, AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message, const StringView& remediation)
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        diagnostic.Remediation = remediation;
        snapshot.Diagnostics.Add(MoveTemp(diagnostic));
    }

    bool ReadJson(const StringView& path, JsonDocument& document, StringAnsi& canonical, AssetPipelineBootstrapSnapshot& snapshot)
    {
        StringAnsi source;
        if (File::ReadAllText(path, source))
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, path,
                TEXT("A bootstrap source file cannot be read."), TEXT("Restore the tracked source file and retry validation."));
            return true;
        }
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Canonicalize(source, canonical, error))
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, path,
                TEXT("A bootstrap source is not valid deterministic JSON."), TEXT("Repair duplicate keys, invalid numbers, or malformed JSON."));
            return true;
        }
        document.Parse(canonical.Get(), canonical.Length());
        if (document.HasParseError() || !document.IsObject())
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, path,
                TEXT("A bootstrap source root must be a JSON object."), TEXT("Restore the canonical authored settings document."));
            return true;
        }
        return false;
    }

    bool ReadRequiredInt(const JsonValue& object, const char* name, int32& value, const StringView& path,
        AssetPipelineBootstrapSnapshot& snapshot)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsInt())
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, path,
                String::Format(TEXT("Project marker field '{0}' is missing or is not an integer."), String(StringAnsiView(name))),
                TEXT("Write the complete versioned asset-system marker before committing migration."));
            return true;
        }
        value = member->value.GetInt();
        return false;
    }

    bool ReadRequiredString(const JsonValue& object, const char* name, String& value, const StringView& path,
        AssetPipelineBootstrapSnapshot& snapshot)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsString())
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, path,
                String::Format(TEXT("Project marker field '{0}' is missing or is not a string."), String(StringAnsiView(name))),
                TEXT("Write the complete versioned asset-system marker before committing migration."));
            return true;
        }
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    void ReadOptionalInt(const JsonValue& object, const char* name, int32& value, const StringView& path,
        AssetPipelineBootstrapSnapshot& snapshot)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd())
            return;
        if (!member->value.IsInt())
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, path,
                String::Format(TEXT("Asset pipeline setting '{0}' must be an integer."), String(StringAnsiView(name))),
                TEXT("Restore a valid value in the authored settings source."));
            return;
        }
        value = member->value.GetInt();
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

    void ValidateLimits(AssetPipelineBootstrapSnapshot& snapshot)
    {
        if (snapshot.DiskQuotaGigabytes < 1 || snapshot.MinimumFreeSpaceGigabytes < 0 ||
            snapshot.GarbageCollectionGracePeriodHours < 0 || snapshot.RetainedLastGoodCount < 0 ||
            snapshot.LogRetentionDays < 1 || snapshot.WorkerLimit < 0 || snapshot.MemoryLimitMegabytes < 128)
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, snapshot.SettingsPath,
                TEXT("Asset pipeline resource limits are outside their supported ranges."),
                TEXT("Restore positive storage retention values, a non-negative worker limit, and at least 128 MB per worker."));
        }
    }
}

AssetPipelineBootstrapSnapshot AssetPipelineBootstrap::Validate(const StringView& projectDescriptorPath, const StringView& contentRoot)
{
    AssetPipelineBootstrapSnapshot snapshot;
    JsonDocument project;
    StringAnsi canonicalProject;
    if (!ReadJson(projectDescriptorPath, project, canonicalProject, snapshot))
    {
        ReadRequiredInt(project, "AssetSystemVersion", snapshot.AssetSystemVersion, projectDescriptorPath, snapshot);
        if (snapshot.AssetSystemVersion < CurrentAssetSystemVersion)
        {
            snapshot.RequiresMigration = true;
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::MigrationFailed, projectDescriptorPath,
                String::Format(TEXT("Project asset-system version {0} requires one-way migration to version {1}."), snapshot.AssetSystemVersion, CurrentAssetSystemVersion),
                TEXT("Run the project migration preflight, create a verified backup, then commit the version marker last."));
        }
        else if (snapshot.AssetSystemVersion > CurrentAssetSystemVersion)
        {
            snapshot.ReadOnly = true;
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                String::Format(TEXT("Project asset-system version {0} is newer than supported version {1}."), snapshot.AssetSystemVersion, CurrentAssetSystemVersion),
                TEXT("Open the project with a compatible newer editor. This editor must keep it read-only."));
        }
        if (snapshot.AssetSystemVersion == CurrentAssetSystemVersion)
        {
            String projectId;
            if (!ReadRequiredString(project, "ProjectId", projectId, projectDescriptorPath, snapshot) &&
                (Guid::Parse(projectId, snapshot.ProjectId) || !snapshot.ProjectId.IsValid()))
            {
                AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                    TEXT("Project marker field 'ProjectId' must be a valid non-empty GUID."),
                    TEXT("Restore the immutable project identity created by migration."));
            }
            const char* mutableFields[] =
            {
                "Name", "Version", "Company", "Copyright", "GameTarget", "EditorTarget", "DefaultScene", "DefaultSceneSpawn",
            };
            for (const char* field : mutableFields)
            {
                if (project.FindMember(field) != project.MemberEnd())
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                        String::Format(TEXT("Asset-system v3 project descriptor duplicates mutable field '{0}'."), String(StringAnsiView(field))),
                        TEXT("Move mutable project values to the mandatory Project, Build, or Editor settings source."));
            }
        }
        ReadRequiredString(project, "SourceRoot", snapshot.SourceRoot, projectDescriptorPath, snapshot);
        ReadRequiredString(project, "IdentityModel", snapshot.IdentityModel, projectDescriptorPath, snapshot);
        ReadRequiredInt(project, "ArtifactLayoutVersion", snapshot.ArtifactLayoutVersion, projectDescriptorPath, snapshot);
        ReadRequiredInt(project, "SourceDocumentVersion", snapshot.SourceDocumentVersion, projectDescriptorPath, snapshot);
        if (snapshot.SourceRoot != TEXT("Content"))
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                TEXT("The only writable project source root must be exactly 'Content'."), TEXT("Remove aliases and restore SourceRoot to 'Content'."));
        if (snapshot.IdentityModel != TEXT("guid-local-id"))
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                TEXT("The project identity model must be exactly 'guid-local-id'."), TEXT("Complete identity migration before updating the project marker."));
        if (snapshot.AssetSystemVersion == CurrentAssetSystemVersion && snapshot.ArtifactLayoutVersion != CurrentArtifactLayoutVersion)
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                TEXT("The artifact layout version does not match this asset-system format."), TEXT("Rebuild Library using artifact layout version 2."));
        if (snapshot.AssetSystemVersion == CurrentAssetSystemVersion && snapshot.SourceDocumentVersion != CurrentSourceDocumentVersion)
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                TEXT("The source-document version does not match this asset-system format."), TEXT("Migrate authored documents before committing the project marker."));
    }

    AssetMountTable bootstrapMounts;
    bool projectMountReady = false;
    String normalizedContent(contentRoot);
    StringUtils::PathRemoveRelativeParts(normalizedContent);
    if (!FileSystem::DirectoryExists(normalizedContent) || StringUtils::GetFileName(normalizedContent) != TEXT("Content"))
    {
        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::SourceMissing, contentRoot,
            TEXT("The canonical project Content root is missing or has a non-canonical name."), TEXT("Restore the project Content directory; do not create Assets or Packages aliases."));
    }
    else
    {
        AssetPipelineDiagnostic mountDiagnostic;
        if (bootstrapMounts.InitializeProject(StringUtils::GetDirectoryName(projectDescriptorPath), normalizedContent, mountDiagnostic))
            snapshot.Diagnostics.Add(MoveTemp(mountDiagnostic));
        else
            projectMountReady = true;
    }

    const String settingsRoot = normalizedContent / TEXT("Settings");
    Array<String> metaFiles;
    if (!FileSystem::DirectoryExists(settingsRoot) || FileSystem::DirectoryGetFiles(metaFiles, settingsRoot, TEXT("*.meta"), DirectorySearchOption::TopDirectoryOnly))
    {
        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, settingsRoot,
            TEXT("The mandatory Content/Settings source folder is missing or unreadable."), TEXT("Restore the tracked Settings folder and its adjacent metadata."));
    }
    else
    {
        if (metaFiles.Count() > 1)
            std::sort(metaFiles.Get(), metaFiles.Get() + metaFiles.Count());
        Array<String> candidates[ARRAY_COUNT(MandatoryRoles)];
        Array<Guid> candidateGuids[ARRAY_COUNT(MandatoryRoles)];
        StringAnsi canonicalRoleData[ARRAY_COUNT(MandatoryRoles)];
        Array<String> mountSettingsCandidates;
        Array<Guid> mountSettingsGuids;
        for (const String& metaPath : metaFiles)
        {
            AssetMeta meta;
            AssetPipelineDiagnostic diagnostic;
            if (AssetMeta::Load(metaPath, meta, diagnostic))
            {
                snapshot.Diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
            const int32 roleIndex = meta.FolderAsset ? -1 : FindMandatoryRole(meta.AssetType);
            if (roleIndex != -1)
            {
                if (meta.MetaUpgradeRequired)
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::MetaUpgradeRequired, metaPath,
                        String::Format(TEXT("Mandatory {0} metadata requires a one-way schema upgrade."), MandatoryRoles[roleIndex].Name), TEXT("Upgrade metadata before committing the asset-system marker."));
                if (meta.SourceKind != AssetSourceKind::ExistingJson || meta.Processor.ID != TEXT("Flax.ExistingJson"))
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::ProcessorMissing, metaPath,
                        String::Format(TEXT("Mandatory {0} must use the built-in ExistingJson bootstrap processor."), MandatoryRoles[roleIndex].Name), TEXT("Restore the built-in processor; scripted importers cannot own bootstrap-critical roles."));
                candidates[roleIndex].Add(metaPath.Substring(0, metaPath.Length() - 5));
                candidateGuids[roleIndex].Add(meta.ID);
            }
            else if (!meta.FolderAsset && meta.AssetType == AssetMountDescriptorCodec::TypeName)
            {
                if (meta.SourceKind != AssetSourceKind::ExistingJson || meta.Processor.ID != TEXT("Flax.ExistingJson"))
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::ProcessorMissing, metaPath,
                        TEXT("Content mount settings must use the built-in ExistingJson bootstrap processor."),
                        TEXT("Restore the built-in processor; scripted importers cannot own mount declarations."));
                mountSettingsCandidates.Add(metaPath.Substring(0, metaPath.Length() - 5));
                mountSettingsGuids.Add(meta.ID);
            }
        }
        for (int32 roleIndex = 0; roleIndex < ARRAY_COUNT(MandatoryRoles); roleIndex++)
        {
            if (candidates[roleIndex].IsEmpty())
            {
                AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, settingsRoot,
                    String::Format(TEXT("Mandatory {0} role is missing."), MandatoryRoles[roleIndex].Name),
                    TEXT("Create exactly one authored owner under Content/Settings with adjacent metadata."));
            }
            else if (candidates[roleIndex].Count() != 1)
            {
                AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, settingsRoot,
                    String::Format(TEXT("Mandatory {0} role is duplicated."), MandatoryRoles[roleIndex].Name),
                    TEXT("Keep exactly one role owner and migrate references before deleting duplicates."));
                snapshot.Diagnostics.Last().Related = candidates[roleIndex];
            }
            else
            {
                AssetSettingsRoleInfo role;
                role.Role = MandatoryRoles[roleIndex].Role;
                role.TypeName = MandatoryRoles[roleIndex].TypeName;
                role.SourcePath = candidates[roleIndex][0];
                role.FileGuid = candidateGuids[roleIndex][0];
                snapshot.MandatoryRoles.Add(MoveTemp(role));

                JsonDocument roleDocument;
                StringAnsi canonicalRoleDocument;
                if (!ReadJson(candidates[roleIndex][0], roleDocument, canonicalRoleDocument, snapshot))
                {
                    const auto idMember = roleDocument.FindMember("ID");
                    Guid documentGuid;
                    if (idMember == roleDocument.MemberEnd() || !idMember->value.IsString() ||
                        Guid::Parse(StringAnsiView(idMember->value.GetString(), idMember->value.GetStringLength()), documentGuid) || documentGuid != candidateGuids[roleIndex][0])
                    {
                        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, candidates[roleIndex][0],
                            String::Format(TEXT("Mandatory {0} document identity does not match its adjacent metadata."), MandatoryRoles[roleIndex].Name),
                            TEXT("Repair the source/meta identity pair without changing the owning GUID."));
                    }
                    const auto typeMember = roleDocument.FindMember("TypeName");
                    if (typeMember == roleDocument.MemberEnd() || !typeMember->value.IsString() ||
                        String(StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength())) != MandatoryRoles[roleIndex].TypeName)
                    {
                        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, candidates[roleIndex][0],
                            String::Format(TEXT("Mandatory {0} document has the wrong TypeName."), MandatoryRoles[roleIndex].Name),
                            TEXT("Restore the built-in settings source type."));
                    }
                    const auto dataMember = roleDocument.FindMember("Data");
                    CanonicalJsonError dataError;
                    if (dataMember == roleDocument.MemberEnd() || !dataMember->value.IsObject() ||
                        CanonicalJsonWriter::Write(dataMember->value, canonicalRoleData[roleIndex], dataError))
                    {
                        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, candidates[roleIndex][0],
                            String::Format(TEXT("Mandatory {0} document has no deterministic Data object."), MandatoryRoles[roleIndex].Name),
                            TEXT("Restore the canonical authored settings payload."));
                    }
                    else if (snapshot.AssetSystemVersion == CurrentAssetSystemVersion)
                    {
                        const JsonValue& data = dataMember->value;
                        if (MandatoryRoles[roleIndex].Role == AssetSettingsRole::Project)
                        {
                            String productName;
                            if (ReadRequiredString(data, "ProductName", productName, candidates[roleIndex][0], snapshot) || productName.IsEmpty())
                                AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, candidates[roleIndex][0],
                                    TEXT("Mandatory Project settings must author a non-empty ProductName."), TEXT("Move the legacy project Name into Project Settings."));
                        }
                        else if (MandatoryRoles[roleIndex].Role == AssetSettingsRole::Build)
                        {
                            String gameTarget;
                            String editorTarget;
                            ReadRequiredString(data, "GameTarget", gameTarget, candidates[roleIndex][0], snapshot);
                            ReadRequiredString(data, "EditorTarget", editorTarget, candidates[roleIndex][0], snapshot);
                        }
                    }
                }
            }
        }

        if (mountSettingsCandidates.Count() > 1)
        {
            AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, settingsRoot,
                TEXT("Content mount settings role is duplicated."), TEXT("Keep exactly one explicit mount descriptor source."));
            snapshot.Diagnostics.Last().Related = mountSettingsCandidates;
        }
        else if (mountSettingsCandidates.Count() == 1)
        {
            StringAnsi source;
            Array<AssetMount> declaredMounts;
            AssetPipelineDiagnostic diagnostic;
            if (File::ReadAllText(mountSettingsCandidates[0], source) ||
                AssetMountDescriptorCodec::Parse(source, mountSettingsCandidates[0], StringUtils::GetDirectoryName(projectDescriptorPath), Globals::StartupFolder, declaredMounts, diagnostic))
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
                    diagnostic.SourcePath = mountSettingsCandidates[0];
                    diagnostic.Message = TEXT("Content mount settings cannot be read.");
                }
                snapshot.Diagnostics.Add(MoveTemp(diagnostic));
            }
            else
            {
                JsonDocument mountDocument;
                mountDocument.Parse(source.Get(), source.Length());
                Guid sourceGuid;
                const auto id = mountDocument.FindMember("ID");
                if (id == mountDocument.MemberEnd() || !id->value.IsString() ||
                    Guid::Parse(StringAnsiView(id->value.GetString(), id->value.GetStringLength()), sourceGuid) || sourceGuid != mountSettingsGuids[0])
                {
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, mountSettingsCandidates[0],
                        TEXT("Content mount settings identity does not match adjacent metadata."), TEXT("Repair the source/meta identity pair without changing its GUID."));
                }
                for (AssetMount& mount : declaredMounts)
                {
                    if (projectMountReady && bootstrapMounts.Register(mount, diagnostic))
                        snapshot.Diagnostics.Add(MoveTemp(diagnostic));
                }
            }
        }

        if (project.IsObject())
        {
            const auto references = project.FindMember("References");
            if (references != project.MemberEnd() && references->value.IsArray())
            {
                for (const JsonValue& reference : references->value.GetArray())
                {
                    if (!reference.IsObject())
                        continue;
                    String name;
                    if (ReadRequiredString(reference, "Name", name, projectDescriptorPath, snapshot))
                        continue;
                    String projectFile;
                    if (ResolveProjectReference(name, StringUtils::GetDirectoryName(projectDescriptorPath), Globals::StartupFolder, projectFile))
                        continue;
                    String expectedRoot;
                    AssetMountKind expectedKind;
                    if (name.StartsWith(TEXT("$(EnginePath)")))
                    {
                        expectedRoot = String(Globals::StartupFolder) / TEXT("Content");
                        expectedKind = AssetMountKind::EngineContent;
                    }
                    else
                    {
                        expectedRoot = String(StringUtils::GetDirectoryName(projectFile)) / TEXT("Content");
                        expectedKind = name.Contains(TEXT("/Plugins/"), StringSearchCase::IgnoreCase)
                            ? AssetMountKind::PluginContent
                            : AssetMountKind::ExternalReadOnlyContent;
                    }
                    if (!FileSystem::DirectoryExists(expectedRoot))
                        continue;
                    bool found = false;
                    for (const AssetMount& mount : bootstrapMounts.GetMounts())
                    {
                        if (mount.Kind == expectedKind && FileSystem::AreFilePathsEquivalent(mount.PhysicalRoot, expectedRoot))
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, projectDescriptorPath,
                            TEXT("A referenced project with source content has no matching explicit read-only content mount."),
                            TEXT("Declare it in the unique AssetMountSettings source; project references never imply recursive content discovery."));
                }
            }
        }
        snapshot.Mounts = bootstrapMounts.GetMounts();

        const int32 pipelineRole = static_cast<int32>(AssetSettingsRole::AssetPipeline);
        if (candidates[pipelineRole].Count() == 1)
        {
            snapshot.SettingsPath = candidates[pipelineRole][0];
            snapshot.SettingsGuid = candidateGuids[pipelineRole][0];
            JsonDocument settings;
            StringAnsi canonicalSettings;
            if (!ReadJson(snapshot.SettingsPath, settings, canonicalSettings, snapshot))
            {
                StringAnsi canonicalSettingsData;
                const auto idMember = settings.FindMember("ID");
                Guid documentGuid;
                if (idMember == settings.MemberEnd() || !idMember->value.IsString() ||
                    Guid::Parse(StringAnsiView(idMember->value.GetString(), idMember->value.GetStringLength()), documentGuid) || documentGuid != snapshot.SettingsGuid)
                {
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, snapshot.SettingsPath,
                        TEXT("AssetPipelineSettings document identity does not match its adjacent metadata."), TEXT("Repair the source/meta identity pair without changing the owning GUID."));
                }
                const auto typeMember = settings.FindMember("TypeName");
                if (typeMember == settings.MemberEnd() || !typeMember->value.IsString() ||
                    StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength()) != StringAnsiView("FlaxEditor.Content.Settings.AssetPipelineSettings"))
                {
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, snapshot.SettingsPath,
                        TEXT("AssetPipelineSettings document has the wrong TypeName."), TEXT("Restore the built-in settings source type."));
                }
                const auto dataMember = settings.FindMember("Data");
                if (dataMember == settings.MemberEnd() || !dataMember->value.IsObject())
                {
                    AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, snapshot.SettingsPath,
                        TEXT("AssetPipelineSettings document is missing its Data object."), TEXT("Restore the deterministic authored settings payload."));
                }
                else
                {
                    const JsonValue& data = dataMember->value;
                    CanonicalJsonError dataError;
                    if (CanonicalJsonWriter::Write(data, canonicalSettingsData, dataError))
                        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, snapshot.SettingsPath,
                            TEXT("AssetPipelineSettings Data cannot be normalized deterministically."), TEXT("Repair the authored settings payload."));
                    const auto duplicateVersion = data.FindMember("AssetSystemVersion");
                    if (duplicateVersion != data.MemberEnd() && snapshot.AssetSystemVersion >= CurrentAssetSystemVersion)
                    {
                        AddDiagnostic(snapshot, AssetPipelineDiagnosticCode::InvalidSettingsCombination, snapshot.SettingsPath,
                            TEXT("AssetPipelineSettings duplicates the asset-system version owned by the project marker."), TEXT("Remove AssetSystemVersion from the authored settings payload; the project descriptor is the only version authority."));
                    }
                    ReadOptionalInt(data, "DiskQuotaGigabytes", snapshot.DiskQuotaGigabytes, snapshot.SettingsPath, snapshot);
                    ReadOptionalInt(data, "MinimumFreeSpaceGigabytes", snapshot.MinimumFreeSpaceGigabytes, snapshot.SettingsPath, snapshot);
                    ReadOptionalInt(data, "GarbageCollectionGracePeriodHours", snapshot.GarbageCollectionGracePeriodHours, snapshot.SettingsPath, snapshot);
                    ReadOptionalInt(data, "RetainedLastGoodCount", snapshot.RetainedLastGoodCount, snapshot.SettingsPath, snapshot);
                    ReadOptionalInt(data, "LogRetentionDays", snapshot.LogRetentionDays, snapshot.SettingsPath, snapshot);
                    ReadOptionalInt(data, "WorkerLimit", snapshot.WorkerLimit, snapshot.SettingsPath, snapshot);
                    ReadOptionalInt(data, "MemoryLimitMegabytes", snapshot.MemoryLimitMegabytes, snapshot.SettingsPath, snapshot);
                    ValidateLimits(snapshot);
                }
                ContentHasher hasher;
                const StringAnsi markerTag("flax-asset-pipeline-bootstrap-v1\n");
                hasher.Update(markerTag.Get(), markerTag.Length());
                hasher.Update(snapshot.ProjectId.Values, sizeof(snapshot.ProjectId.Values));
                hasher.Update(&snapshot.AssetSystemVersion, sizeof(snapshot.AssetSystemVersion));
                const StringAnsi sourceRoot(snapshot.SourceRoot);
                hasher.Update(sourceRoot.Get(), sourceRoot.Length());
                const StringAnsi identityModel(snapshot.IdentityModel);
                hasher.Update(identityModel.Get(), identityModel.Length());
                hasher.Update(&snapshot.ArtifactLayoutVersion, sizeof(snapshot.ArtifactLayoutVersion));
                hasher.Update(&snapshot.SourceDocumentVersion, sizeof(snapshot.SourceDocumentVersion));
                for (int32 roleIndex = 0; roleIndex < ARRAY_COUNT(MandatoryRoles); roleIndex++)
                {
                    if (candidateGuids[roleIndex].Count() != 1)
                        continue;
                    const byte role = static_cast<byte>(MandatoryRoles[roleIndex].Role);
                    hasher.Update(&role, sizeof(role));
                    hasher.Update(candidateGuids[roleIndex][0].Values, sizeof(candidateGuids[roleIndex][0].Values));
                    hasher.Update(canonicalRoleData[roleIndex].Get(), canonicalRoleData[roleIndex].Length());
                }
                snapshot.SettingsFingerprint = String(hasher.Finalize().ToString());
            }
        }
    }

    snapshot.Valid = !snapshot.RequiresMigration && !snapshot.ReadOnly && !HasErrors(snapshot.Diagnostics);
    return snapshot;
}
