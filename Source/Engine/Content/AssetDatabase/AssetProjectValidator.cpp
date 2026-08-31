// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetProjectValidator.h"
#include "AssetMeta.h"
#include "AssetPath.h"
#include "SourceHashCache.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    void AddDiagnostic(AssetProjectValidationResult& result, AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message, const StringView& remediation = StringView::Empty)
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        diagnostic.Remediation = remediation;
        result.Diagnostics.Add(MoveTemp(diagnostic));
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

    bool IsMeta(const StringView& path)
    {
        return path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
    }

    String RelativePath(const StringView& root, const StringView& path)
    {
        String result = FileSystem::ConvertAbsolutePathToRelative(root, path);
        result.Replace('\\', '/');
        return result;
    }

    void HashText(ContentHasher& hasher, const StringView& value)
    {
        const StringAnsi ansi(value);
        const uint32 length = ansi.Length();
        hasher.Update(&length, sizeof(length));
        hasher.Update(ansi.Get(), ansi.Length());
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi ansi(value);
        object.AddMember(JsonValue(name, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
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

    void SortDiagnostics(Array<AssetPipelineDiagnostic>& diagnostics)
    {
        if (diagnostics.Count() < 2)
            return;
        std::sort(diagnostics.Get(), diagnostics.Get() + diagnostics.Count(), [](const AssetPipelineDiagnostic& a, const AssetPipelineDiagnostic& b)
        {
            const int32 path = a.SourcePath.Compare(b.SourcePath, StringSearchCase::IgnoreCase);
            if (path != 0)
                return path < 0;
            if (a.Code != b.Code)
                return static_cast<int32>(a.Code) < static_cast<int32>(b.Code);
            return a.Message < b.Message;
        });
    }

    void WriteReport(AssetProjectValidationResult& result, const StringView& projectRoot)
    {
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        document.AddMember("schemaVersion", 1, allocator);
        document.AddMember("valid", result.Valid, allocator);
        document.AddMember("requiresMigration", result.RequiresMigration, allocator);
        document.AddMember("readOnly", result.ReadOnly, allocator);
        document.AddMember("sourceFiles", result.SourceFiles, allocator);
        document.AddMember("sourceFolders", result.SourceFolders, allocator);
        document.AddMember("metadataFiles", result.MetadataFiles, allocator);
        AddString(document, "sourceTreeFingerprint", result.SourceTreeFingerprint, allocator);
        AddString(document, "settingsFingerprint", result.Bootstrap.SettingsFingerprint, allocator);
        JsonValue diagnostics(rapidjson::kArrayType);
        for (const AssetPipelineDiagnostic& diagnostic : result.Diagnostics)
        {
            JsonValue item(rapidjson::kObjectType);
            AddString(item, "code", GetAssetPipelineDiagnosticCodeName(diagnostic.Code), allocator);
            AddString(item, "path", RelativePath(projectRoot, diagnostic.SourcePath), allocator);
            AddString(item, "message", diagnostic.Message, allocator);
            AddString(item, "remediation", diagnostic.Remediation, allocator);
            diagnostics.PushBack(item, allocator);
        }
        document.AddMember("diagnostics", diagnostics, allocator);
        Array<StringAnsi> order;
        order.Add("schemaVersion");
        order.Add("valid");
        order.Add("requiresMigration");
        order.Add("readOnly");
        order.Add("sourceFiles");
        order.Add("sourceFolders");
        order.Add("metadataFiles");
        order.Add("sourceTreeFingerprint");
        order.Add("settingsFingerprint");
        order.Add("diagnostics");
        StringAnsi report;
        CanonicalJsonError error;
        if (!CanonicalJsonWriter::Write(document, report, error, &order))
            result.ReportJson = String(report);
    }
}

AssetProjectValidationResult AssetProjectValidator::Validate(const StringView& projectDescriptorPath, const StringView& contentRoot)
{
    AssetProjectValidationResult result;
    result.Bootstrap = AssetPipelineBootstrap::Validate(projectDescriptorPath, contentRoot);
    result.RequiresMigration = result.Bootstrap.RequiresMigration;
    result.ReadOnly = result.Bootstrap.ReadOnly;
    result.Diagnostics = result.Bootstrap.Diagnostics;

    String projectRoot(StringUtils::GetDirectoryName(projectDescriptorPath));
    String normalizedContent(contentRoot);
    StringUtils::PathRemoveRelativeParts(projectRoot);
    StringUtils::PathRemoveRelativeParts(normalizedContent);
    const Char* forbiddenRoots[] = { TEXT("Assets"), TEXT("Packages"), TEXT("ProjectSettings") };
    for (const Char* root : forbiddenRoots)
    {
        const String path = projectRoot / root;
        if (FileSystem::DirectoryExists(path))
            AddDiagnostic(result, AssetPipelineDiagnosticCode::PathCollision, path,
                TEXT("A legacy/Unity-compatible source or settings root is forbidden after migration."),
                TEXT("Move authored sources into Content, register explicit read-only mounts, and remove the alias before commit."));
    }

    Array<String> files;
    Array<String> directories;
    if (FileSystem::DirectoryGetFiles(files, normalizedContent, TEXT("*"), DirectorySearchOption::AllDirectories))
        AddDiagnostic(result, AssetPipelineDiagnosticCode::SourceMissing, normalizedContent, TEXT("Cannot enumerate the canonical Content source root."));
    if (CollectDirectories(normalizedContent, directories))
        AddDiagnostic(result, AssetPipelineDiagnosticCode::SourceBusy, normalizedContent, TEXT("Cannot enumerate all source folders deterministically."));
    if (files.Count() > 1)
        std::sort(files.Get(), files.Get() + files.Count());
    if (directories.Count() > 1)
        std::sort(directories.Get(), directories.Get() + directories.Count());
    result.SourceFolders = directories.Count();

    Array<AssetPathPolicy::ProjectPath> normalizedPaths;
    Dictionary<Guid, String> identities;
    SourceHashCache hashCache;
    ContentHasher treeHasher;
    const StringAnsi domain("flax-source-tree-v1");
    treeHasher.Update(domain.Get(), domain.Length());
    for (const String& directory : directories)
    {
        AssetPathPolicy::ProjectPath normalized;
        AssetPipelineDiagnostic diagnostic;
        if (AssetPathPolicy::TryNormalizeProjectPath(projectRoot, normalizedContent, projectRoot / TEXT("Library"), directory, normalized, diagnostic))
            result.Diagnostics.Add(MoveTemp(diagnostic));
        else
            normalizedPaths.Add(MoveTemp(normalized));
        HashText(treeHasher, RelativePath(normalizedContent, directory) + TEXT("/"));
        if (!FileSystem::FileExists(directory + TEXT(".meta")))
            AddDiagnostic(result, AssetPipelineDiagnosticCode::MissingMeta, directory,
                TEXT("A source folder is missing its universal adjacent metadata."), TEXT("Create metadata through the native asset mutation/import service."));
    }
    for (const String& file : files)
    {
        AssetPathPolicy::ProjectPath normalized;
        AssetPipelineDiagnostic diagnostic;
        if (AssetPathPolicy::TryNormalizeProjectPath(projectRoot, normalizedContent, projectRoot / TEXT("Library"), file, normalized, diagnostic))
            result.Diagnostics.Add(MoveTemp(diagnostic));
        else
            normalizedPaths.Add(MoveTemp(normalized));

        const String relative = RelativePath(normalizedContent, file);
        HashText(treeHasher, relative);
        ContentHash contentHash;
        SourceHashFileState state;
        if (hashCache.HashFile(file, contentHash, state, diagnostic))
            result.Diagnostics.Add(MoveTemp(diagnostic));
        else
            treeHasher.Update(contentHash.Bytes, sizeof(contentHash.Bytes));

        if (IsMeta(file))
        {
            result.MetadataFiles++;
            const String source = file.Substring(0, file.Length() - 5);
            if (!FileSystem::FileExists(source) && !FileSystem::DirectoryExists(source))
            {
                AddDiagnostic(result, AssetPipelineDiagnosticCode::InvalidMeta, file,
                    TEXT("Orphan metadata has no adjacent source file or folder."), TEXT("Restore the source or resolve the orphan explicitly before migration commit."));
                continue;
            }
            AssetMeta meta;
            if (AssetMeta::Load(file, meta, diagnostic))
            {
                result.Diagnostics.Add(MoveTemp(diagnostic));
                continue;
            }
            const String* owner = identities.TryGet(meta.ID);
            if (owner)
            {
                AssetPipelineDiagnostic duplicate;
                duplicate.Code = AssetPipelineDiagnosticCode::DuplicateGuid;
                duplicate.Stage = AssetPipelineDiagnosticStage::Migration;
                duplicate.AssetGuid = meta.ID;
                duplicate.SourcePath = file;
                duplicate.Message = TEXT("Two live sources claim the same file GUID.");
                duplicate.Remediation = TEXT("Select the identity owner and rewrite only references proven to target the reassigned source.");
                duplicate.Related.Add(*owner);
                duplicate.Related.Add(file);
                result.Diagnostics.Add(MoveTemp(duplicate));
            }
            else
            {
                identities.Add(meta.ID, file);
            }
            continue;
        }

        if (!FileSystem::FileExists(file + TEXT(".meta")))
            AddDiagnostic(result, AssetPipelineDiagnosticCode::MissingMeta, file,
                TEXT("A source file is missing its universal adjacent metadata."), TEXT("Create metadata through the native asset mutation/import service."));
        if (FileSystem::GetExtension(file).ToLower() == TEXT("flax"))
            AddDiagnostic(result, AssetPipelineDiagnosticCode::MigrationFailed, file,
                TEXT("Legacy cooked .flax output remains in the writable source mount."), TEXT("Convert authored data or recover the original source, then quarantine generated output under Library/Migration."));
        const String lowered = relative.ToLower();
        if (lowered.StartsWith(TEXT("library/")) || lowered.StartsWith(TEXT("temp/")) || lowered.StartsWith(TEXT("build/")) || lowered.StartsWith(TEXT("output/")))
            AddDiagnostic(result, AssetPipelineDiagnosticCode::LibraryPathInvalid, file,
                TEXT("Derived or produced output is nested inside the canonical source mount."), TEXT("Move derived state outside Content before migration commit."));
    }

    AssetPathPolicy::FindPortabilityCollisions(normalizedPaths, result.Diagnostics);
    result.SourceFiles = files.Count() - result.MetadataFiles;
    result.SourceTreeFingerprint = String(treeHasher.Finalize().ToString());
    SortDiagnostics(result.Diagnostics);
    result.Valid = result.Bootstrap.Valid && !HasErrors(result.Diagnostics);
    WriteReport(result, projectRoot);
    return result;
}
