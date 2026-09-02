// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetOperations.h"
#include "AssetDatabaseServices.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Utilities/Crc.h"
#include <algorithm>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <sys/stat.h>
#endif

namespace
{
    constexpr int32 MaximumTrashEntries = 4096;
    constexpr int32 MaximumMetadataBatchEntries = 4096;
    CriticalSection MetadataBatchLocker;

    struct OperationRollbackPlan
    {
        Guid TransactionId;
        AssetOperationKind Kind = AssetOperationKind::Create;
        Guid AssetGuid;
        Guid SourceAssetGuid;
        String SourcePath;
        String DestinationPath;
        String StageSourcePath;
        String StageMetaPath;
        String SourceFragmentsPath;
        String DestinationFragmentsPath;
        String StageFragmentsPath;
    };

    enum class BatchRollbackKind : byte
    {
        Trash,
        Restore,
        Discard,
        Copy,
        ContentCopy,
    };

    struct BatchRollbackPlan
    {
        Guid TransactionId;
        BatchRollbackKind Kind = BatchRollbackKind::Trash;
        AssetTrashBatch Trash;
        String TrashRoot;
        String DiscardStageRoot;
    };

    struct MetadataBatchRollbackEntry
    {
        Guid AssetID;
        String SourcePath;
        String MetadataPath;
        String StagingPath;
        String BackupPath;
        bool ReplaceExistingMetadata = false;
    };

    struct MetadataBatchRollbackPlan
    {
        Guid TransactionId;
        Array<MetadataBatchRollbackEntry> Entries;
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    String MetaPath(const StringView& source)
    {
        return String(source) + TEXT(".meta");
    }

    bool IsMetaPath(const StringView& path)
    {
        return path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
    }

    bool EnsureDirectory(const StringView& path)
    {
        if (path.IsEmpty())
            return true;
        return FileSystem::CreateDirectory(path);
    }

    bool EnsureParent(const StringView& path)
    {
        return EnsureDirectory(StringUtils::GetDirectoryName(path));
    }

    bool PrepareRemappedSceneSource(const StringView& sourcePath, const StringView& stagingPath,
        const Guid& sourceSceneGuid, const Guid& destinationSceneGuid, String& error)
    {
        BytesContainer sourceBytes;
        if (File::ReadAllBytes(sourcePath, sourceBytes))
        {
            error = TEXT("Cannot read the external-actors scene source for cloning.");
            return true;
        }
        rapidjson_flax::Document document;
        document.Parse(sourceBytes.Get<char>(), sourceBytes.Length());
        CanonicalJsonError jsonError;
        if (document.HasParseError() || !document.IsObject())
        {
            error = TEXT("External-actors scene source is malformed or does not declare external actor storage.");
            return true;
        }
        const auto externalActors = document.FindMember("externalActors");
        if (externalActors == document.MemberEnd() || !externalActors->value.IsBool() ||
            !externalActors->value.GetBool() || CanonicalJsonWriter::Validate(document, jsonError))
        {
            error = TEXT("External-actors scene source is malformed or does not declare external actor storage.");
            return true;
        }

        Dictionary<Guid, Guid> remap;
        remap.Add(sourceSceneGuid, destinationSceneGuid);
        JsonTools::ChangeIds(document, remap);
        if (CanonicalJsonWriter::Validate(document, jsonError))
        {
            error = TEXT("Remapped external-actors scene source failed JSON validation.");
            return true;
        }
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        document.Accept(writer.GetWriter());
        if (File::WriteAllBytes(stagingPath, buffer.GetString(), static_cast<int32>(buffer.GetSize())))
        {
            error = TEXT("Cannot write the remapped external-actors scene source staging file.");
            return true;
        }
        return false;
    }

    bool MovePath(const StringView& destination, const StringView& source, bool overwrite)
    {
        return FileSystem::MoveFile(destination, source, overwrite);
    }

    bool DeletePath(const StringView& path)
    {
        return FileSystem::DeleteFile(path);
    }

    bool DeleteTree(const StringView& path)
    {
        return FileSystem::DeleteDirectory(path, true);
    }

    bool DeleteEmptyDirectory(const StringView& path)
    {
        return FileSystem::DeleteDirectory(path, false);
    }

    bool PrepareOperationScratch(const StringView& directory, AssetPipelineDiagnostic& diagnostic)
    {
        return EnsureDirectory(directory) && Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed,
            directory, TEXT("Cannot create the asset operation staging directory."));
    }

    bool PrepareBatchScratch(const StringView& directory, AssetPipelineDiagnostic& diagnostic)
    {
        return EnsureDirectory(directory) && Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed,
            directory, TEXT("Cannot create the batch asset operation staging directory."));
    }


    bool PathExists(const StringView& path)
    {
        return FileSystem::FileExists(path) || FileSystem::DirectoryExists(path);
    }

    bool IsFileSystemLink(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        const DWORD attributes = GetFileAttributesW(*value);
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#elif PLATFORM_LINUX || PLATFORM_MAC
        const StringAnsi value(path);
        struct stat status;
        return lstat(value.Get(), &status) == 0 && S_ISLNK(status.st_mode);
#else
        return false;
#endif
    }

    bool ContainsFileSystemLink(const StringView& path, bool isFolder)
    {
        if (IsFileSystemLink(path))
            return true;
        if (!isFolder)
            return false;
        Array<String> pending;
        pending.Add(String(path));
        while (pending.HasItems())
        {
            const String directory = pending.Last();
            pending.RemoveLast();
            Array<String> files;
            Array<String> directories;
            if (FileSystem::DirectoryGetFiles(files, directory, TEXT("*"), DirectorySearchOption::TopDirectoryOnly) ||
                FileSystem::GetChildDirectories(directories, directory))
                return true;
            for (const String& file : files)
            {
                if (IsFileSystemLink(file))
                    return true;
            }
            for (const String& child : directories)
            {
                if (IsFileSystemLink(child))
                    return true;
                pending.Add(child);
            }
        }
        return false;
    }

    bool MovePrimaryPath(const AssetTrashEntry& entry, const StringView& destination, const StringView& source)
    {
#if USE_EDITOR
        const bool failed = entry.IsFolder
            ? Content::RenameAssetFolder(source, destination)
            : (entry.AssetGuid.IsValid() ? Content::RenameAsset(source, destination) :
                MovePath(destination, source, false));
        return failed;
#else
        return MovePath(destination, source, false);
#endif
    }

    bool MoveEntry(const AssetTrashEntry& entry, bool toTrash)
    {
        const StringView source = toTrash ? StringView(entry.OriginalPath) : StringView(entry.TrashPath);
        const StringView destination = toTrash ? StringView(entry.TrashPath) : StringView(entry.OriginalPath);
        if (EnsureParent(destination) || MovePrimaryPath(entry, destination, source))
            return true;
        if (entry.OriginalMetaPath.HasChars())
        {
            const StringView metaSource = toTrash ? StringView(entry.OriginalMetaPath) : StringView(entry.TrashMetaPath);
            const StringView metaDestination = toTrash ? StringView(entry.TrashMetaPath) : StringView(entry.OriginalMetaPath);
            if (EnsureParent(metaDestination) || MovePath(metaDestination, metaSource, false))
                return true;
        }
        for (const AssetTrashFragment& fragment : entry.Fragments)
        {
            const StringView fragmentSource = toTrash ? StringView(fragment.OriginalPath) : StringView(fragment.TrashPath);
            const StringView fragmentDestination = toTrash ? StringView(fragment.TrashPath) : StringView(fragment.OriginalPath);
            if (EnsureParent(fragmentDestination) || MovePath(fragmentDestination, fragmentSource, false))
                return true;
        }
        return false;
    }

    bool RollbackMovedEntry(const AssetTrashEntry& entry, bool wasTrash)
    {
        bool failed = false;
        for (int32 i = entry.Fragments.Count() - 1; i >= 0; i--)
        {
            const StringView source = wasTrash ? StringView(entry.Fragments[i].TrashPath) : StringView(entry.Fragments[i].OriginalPath);
            const StringView destination = wasTrash ? StringView(entry.Fragments[i].OriginalPath) : StringView(entry.Fragments[i].TrashPath);
            if (!PathExists(destination) && FileSystem::DirectoryExists(source))
                failed |= EnsureParent(destination) || MovePath(destination, source, false);
            else if (PathExists(source))
                failed = true;
        }
        if (entry.OriginalMetaPath.HasChars())
        {
            const StringView source = wasTrash ? StringView(entry.TrashMetaPath) : StringView(entry.OriginalMetaPath);
            const StringView destination = wasTrash ? StringView(entry.OriginalMetaPath) : StringView(entry.TrashMetaPath);
            if (!PathExists(destination) && FileSystem::FileExists(source))
                failed |= EnsureParent(destination) || MovePath(destination, source, false);
            else if (PathExists(source))
                failed = true;
        }
        const StringView source = wasTrash ? StringView(entry.TrashPath) : StringView(entry.OriginalPath);
        const StringView destination = wasTrash ? StringView(entry.OriginalPath) : StringView(entry.TrashPath);
        const bool sourceExists = entry.IsFolder ? FileSystem::DirectoryExists(source) : FileSystem::FileExists(source);
        if (!PathExists(destination) && sourceExists)
            failed |= EnsureParent(destination) || MovePrimaryPath(entry, destination, source);
        else if (PathExists(source))
            failed = true;
        return failed;
    }

    bool RollbackCopiedEntry(const AssetTrashEntry& entry)
    {
        const bool primaryExists = PathExists(entry.TrashPath);
        const bool metadataExists = FileSystem::FileExists(entry.TrashMetaPath);
        bool fragmentsExist = false;
        for (const AssetTrashFragment& fragment : entry.Fragments)
            fragmentsExist |= FileSystem::DirectoryExists(fragment.TrashPath);
        if (!primaryExists && !metadataExists && !fragmentsExist)
            return false;
        if (!entry.AssetGuid.IsValid() || (primaryExists && !metadataExists))
            return true;
        if (metadataExists)
        {
            AssetMeta meta;
            AssetPipelineDiagnostic diagnostic;
            if (AssetMeta::Load(entry.TrashMetaPath, meta, diagnostic) || meta.ID != entry.AssetGuid)
                return true;
        }

        for (int32 i = entry.Fragments.Count() - 1; i >= 0; i--)
        {
            if (FileSystem::DirectoryExists(entry.Fragments[i].TrashPath) &&
                DeleteTree(entry.Fragments[i].TrashPath))
                return true;
        }
        if (entry.IsFolder && FileSystem::DirectoryExists(entry.TrashPath))
        {
            if (DeleteTree(entry.TrashPath))
                return true;
        }
        else if (FileSystem::FileExists(entry.TrashPath) && DeletePath(entry.TrashPath))
        {
            return true;
        }
        return FileSystem::FileExists(entry.TrashMetaPath) && DeletePath(entry.TrashMetaPath);
    }

    bool RollbackContentCopyEntry(const AssetTrashEntry& entry)
    {
        bool failed = false;
        for (int32 i = entry.Fragments.Count() - 1; i >= 0; i--)
        {
            if (FileSystem::DirectoryExists(entry.Fragments[i].TrashPath))
                failed |= DeleteTree(entry.Fragments[i].TrashPath);
        }
        if (entry.IsFolder && FileSystem::DirectoryExists(entry.TrashPath))
            failed |= DeleteTree(entry.TrashPath);
        else if (FileSystem::FileExists(entry.TrashPath))
            failed |= DeletePath(entry.TrashPath);
        if (FileSystem::FileExists(entry.TrashMetaPath))
            failed |= DeletePath(entry.TrashMetaPath);
        return failed;
    }

    bool RollbackBatch(const BatchRollbackPlan& plan)
    {
        if (plan.Kind == BatchRollbackKind::Discard)
        {
            if (!PathExists(plan.TrashRoot) && FileSystem::DirectoryExists(plan.DiscardStageRoot))
                return EnsureParent(plan.TrashRoot) || MovePath(plan.TrashRoot, plan.DiscardStageRoot, false);
            return FileSystem::DirectoryExists(plan.DiscardStageRoot);
        }
        bool failed = false;
        for (int32 i = plan.Trash.Entries.Count() - 1; i >= 0; i--)
        {
            if (plan.Kind == BatchRollbackKind::Copy)
                failed |= RollbackCopiedEntry(plan.Trash.Entries[i]);
            else if (plan.Kind == BatchRollbackKind::ContentCopy)
                failed |= RollbackContentCopyEntry(plan.Trash.Entries[i]);
            else
                failed |= RollbackMovedEntry(plan.Trash.Entries[i], plan.Kind == BatchRollbackKind::Trash);
        }
        return failed;
    }

    void CleanupEmptyTrashRoot(const BatchRollbackPlan& plan)
    {
        for (const AssetTrashEntry& entry : plan.Trash.Entries)
        {
            DeleteEmptyDirectory(StringUtils::GetDirectoryName(entry.TrashPath));
            for (const AssetTrashFragment& fragment : entry.Fragments)
                DeleteEmptyDirectory(StringUtils::GetDirectoryName(fragment.TrashPath));
        }
        DeleteEmptyDirectory(plan.TrashRoot);
    }

    bool EqualTrashEntry(const AssetTrashEntry& left, const AssetTrashEntry& right)
    {
        return left.AssetGuid == right.AssetGuid && left.IsFolder == right.IsFolder &&
            FileSystem::AreFilePathsEquivalent(left.OriginalPath, right.OriginalPath) &&
            FileSystem::AreFilePathsEquivalent(left.TrashPath, right.TrashPath) &&
            left.OriginalMetaPath == right.OriginalMetaPath && left.TrashMetaPath == right.TrashMetaPath &&
            left.Fragments.Count() == right.Fragments.Count() && [&left, &right]()
            {
                for (int32 i = 0; i < left.Fragments.Count(); i++)
                {
                    if (!FileSystem::AreFilePathsEquivalent(left.Fragments[i].OriginalPath, right.Fragments[i].OriginalPath) ||
                        !FileSystem::AreFilePathsEquivalent(left.Fragments[i].TrashPath, right.Fragments[i].TrashPath))
                        return false;
                }
                return true;
            }();
    }

    bool GetMetaSemanticHash(const AssetMeta& meta, uint64& hash, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi canonical;
        if (meta.ToJson(canonical, diagnostic))
            return true;
        hash = Crc::MemCrc32(canonical.Get(), canonical.Length());
        return false;
    }

    bool IsCreateKind(AssetOperationKind kind)
    {
        return kind == AssetOperationKind::Create || kind == AssetOperationKind::Import || kind == AssetOperationKind::Copy;
    }

    bool RollbackOperation(const OperationRollbackPlan& plan)
    {
        const String sourceMeta = MetaPath(plan.SourcePath);
        const String destinationMeta = MetaPath(plan.DestinationPath);
        bool failed = false;
        if (IsCreateKind(plan.Kind))
        {
            if (FileSystem::FileExists(plan.DestinationPath))
                failed |= DeletePath(plan.DestinationPath);
            if (FileSystem::FileExists(destinationMeta))
                failed |= DeletePath(destinationMeta);
            if (plan.DestinationFragmentsPath.HasChars() &&
                FileSystem::DirectoryExists(plan.DestinationFragmentsPath))
                failed |= DeleteTree(plan.DestinationFragmentsPath);
            return failed;
        }
        if (!FileSystem::FileExists(plan.SourcePath))
        {
            if (FileSystem::FileExists(plan.DestinationPath))
                failed |= MovePath(plan.SourcePath, plan.DestinationPath, false);
            else if (FileSystem::FileExists(plan.StageSourcePath))
                failed |= MovePath(plan.SourcePath, plan.StageSourcePath, false);
        }
        if (!FileSystem::FileExists(sourceMeta))
        {
            if (FileSystem::FileExists(destinationMeta))
                failed |= MovePath(sourceMeta, destinationMeta, false);
            else if (FileSystem::FileExists(plan.StageMetaPath))
                failed |= MovePath(sourceMeta, plan.StageMetaPath, false);
        }
        if (plan.SourceFragmentsPath.HasChars() && !FileSystem::DirectoryExists(plan.SourceFragmentsPath))
        {
            if (FileSystem::DirectoryExists(plan.DestinationFragmentsPath))
                failed |= MovePath(plan.SourceFragmentsPath, plan.DestinationFragmentsPath, false);
            else if (FileSystem::DirectoryExists(plan.StageFragmentsPath))
                failed |= MovePath(plan.SourceFragmentsPath, plan.StageFragmentsPath, false);
        }
        return failed;
    }

    void AddSelfWrite(AssetOperationCommit& commit, const StringView& path)
    {
        AssetOperationSelfWrite write;
        write.TransactionId = commit.TransactionId;
        write.Path = path;
        if (!HashFile(path, write.Content))
            commit.SelfWrites.Add(MoveTemp(write));
    }

    bool RestoreFileAtomic(const StringView& path, const BytesContainer& bytes)
    {
        const String staging = String(path) + TEXT(".rollback-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { FileSystem::DeleteFile(staging); };
        return File::WriteAllBytes(staging, bytes.Get(), bytes.Length()) ||
               FileSystem::MoveFile(path, staging, true);
    }

    String NormalizeMetadataBatchPath(const StringView& path)
    {
        String result(path);
        StringUtils::PathRemoveRelativeParts(result);
        FileSystem::NormalizePath(result);
        return result;
    }

    bool ValidateMetadataBatchPlan(const MetadataBatchRollbackPlan& plan, const StringView& directory,
        const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
        AssetPipelineDiagnostic& diagnostic)
    {
        const String transactionRoot = NormalizeMetadataBatchPath(String(libraryRoot) / TEXT("Temp/AssetOperations"));
        const String expectedDirectory = transactionRoot / plan.TransactionId.ToString(Guid::FormatType::N);
        const String stagingRoot = NormalizeMetadataBatchPath(String(libraryRoot) / TEXT("Temp/MetadataBatches"));
        if (!FileSystem::AreFilePathsEquivalent(directory, expectedDirectory))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, directory,
                TEXT("Metadata batch staging directory does not match its operation identity."));

        AssetSourceRootRegistry roots(projectRoot, libraryRoot);
        if (roots.RegisterProjectRoots(contentRoot, diagnostic))
            return true;
        HashSet<String> paths;
        for (int32 i = 0; i < plan.Entries.Count(); i++)
        {
            const MetadataBatchRollbackEntry& entry = plan.Entries[i];
            ResolvedAssetSourcePath resolved;
            const String expectedBackup = String(directory) / TEXT("metadata-backups") /
                String::Format(TEXT("{0}.meta"), i);
            if (!entry.AssetID.IsValid() || roots.Resolve(entry.SourcePath, resolved, diagnostic) ||
                resolved.Root.Kind != AssetSourceRootKind::ProjectContent ||
                !resolved.Root.HasPermission(AssetSourceRootPermission::GenericMutation) ||
                !FileSystem::AreFilePathsEquivalent(resolved.Path.AbsolutePath, entry.SourcePath) ||
                !FileSystem::AreFilePathsEquivalent(entry.MetadataPath, entry.SourcePath + TEXT(".meta")) ||
                !AssetPathPolicy::IsSameOrChild(entry.StagingPath, stagingRoot) ||
                FileSystem::AreFilePathsEquivalent(entry.StagingPath, stagingRoot) ||
                !FileSystem::AreFilePathsEquivalent(entry.BackupPath, expectedBackup))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, entry.SourcePath,
                    TEXT("Metadata batch rollback paths failed canonical root validation."));
            String sourceKey = NormalizeMetadataBatchPath(entry.SourcePath).ToLower();
            String stagingKey = NormalizeMetadataBatchPath(entry.StagingPath).ToLower();
            if (paths.Contains(sourceKey) || paths.Contains(stagingKey))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, entry.SourcePath,
                    TEXT("Metadata batch contains duplicate source or staging paths."));
            paths.Add(MoveTemp(sourceKey));
            paths.Add(MoveTemp(stagingKey));
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool RollbackMetadataBatch(const MetadataBatchRollbackPlan& plan)
    {
        bool failed = false;
        for (int32 i = plan.Entries.Count() - 1; i >= 0; i--)
        {
            const MetadataBatchRollbackEntry& entry = plan.Entries[i];
            if (FileSystem::FileExists(entry.MetadataPath))
            {
                AssetMeta active;
                AssetPipelineDiagnostic ignored;
                if (!AssetMeta::Load(entry.MetadataPath, active, ignored) && active.ID == entry.AssetID)
                    failed |= DeletePath(entry.MetadataPath);
                else if (FileSystem::FileExists(entry.BackupPath) || !entry.ReplaceExistingMetadata)
                    failed = true;
            }
            if (FileSystem::FileExists(entry.BackupPath))
            {
                if (FileSystem::FileExists(entry.MetadataPath))
                    failed = true;
                else
                    failed |= MovePath(entry.MetadataPath, entry.BackupPath, false);
            }
            if (FileSystem::FileExists(entry.StagingPath))
                failed |= DeletePath(entry.StagingPath);
        }
        return failed;
    }

}

#if USE_EDITOR
bool AssetOperationService::PublishDefaultMetadataBatch(const Array<AssetDefaultMetadataBatchEntry>& entries)
{
    return PublishDefaultMetadataBatch(entries, AssetDefaultMetadataBatchFailurePoint::None);
}

bool AssetOperationService::PublishDefaultMetadataBatch(const Array<AssetDefaultMetadataBatchEntry>& entries,
    AssetDefaultMetadataBatchFailurePoint failurePoint)
{
    if (entries.IsEmpty() || entries.Count() > MaximumMetadataBatchEntries)
        return true;

    MetadataBatchLocker.Lock();
    SCOPE_EXIT { MetadataBatchLocker.Unlock(); };

    MetadataBatchRollbackPlan plan;
    plan.TransactionId = Guid::New();
    const String stagingRoot = NormalizeMetadataBatchPath(Globals::ProjectLibraryFolder /
        TEXT("Temp/AssetOperations"));
    const String transactionDirectory = stagingRoot /
        plan.TransactionId.ToString(Guid::FormatType::N);
    const String backupRoot = transactionDirectory / TEXT("metadata-backups");
    plan.Entries.Resize(entries.Count());
    for (int32 i = 0; i < entries.Count(); i++)
    {
        const AssetDefaultMetadataBatchEntry& input = entries[i];
        MetadataBatchRollbackEntry& entry = plan.Entries[i];
        entry.AssetID = input.AssetID;
        entry.SourcePath = NormalizeMetadataBatchPath(input.SourcePath);
        entry.MetadataPath = entry.SourcePath + TEXT(".meta");
        entry.StagingPath = NormalizeMetadataBatchPath(input.StagingPath);
        entry.BackupPath = backupRoot / String::Format(TEXT("{0}.meta"), i);
        entry.ReplaceExistingMetadata = input.ReplaceExistingMetadata;
    }

    AssetPipelineDiagnostic diagnostic;
    if (ValidateMetadataBatchPlan(plan, transactionDirectory, Globals::ProjectFolder,
            Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, diagnostic))
        return true;
    for (const MetadataBatchRollbackEntry& entry : plan.Entries)
    {
        if (!FileSystem::FileExists(entry.SourcePath) || !FileSystem::FileExists(entry.StagingPath) ||
            IsFileSystemLink(entry.SourcePath) || IsFileSystemLink(entry.MetadataPath) ||
            IsFileSystemLink(entry.StagingPath) ||
            FileSystem::FileExists(entry.MetadataPath) != entry.ReplaceExistingMetadata)
            return true;
        AssetMeta staged;
        if (AssetMeta::Load(entry.StagingPath, staged, diagnostic) || staged.ID != entry.AssetID)
            return true;
    }

    bool needsBackupRoot = false;
    for (const MetadataBatchRollbackEntry& entry : plan.Entries)
        needsBackupRoot |= entry.ReplaceExistingMetadata;
    if (EnsureDirectory(transactionDirectory))
        return true;
    if (needsBackupRoot && EnsureDirectory(backupRoot))
    {
        RollbackMetadataBatch(plan);
        DeleteTree(transactionDirectory);
        return true;
    }

    bool failed = false;
    for (int32 i = 0; i < plan.Entries.Count(); i++)
    {
        const MetadataBatchRollbackEntry& entry = plan.Entries[i];
        if (entry.ReplaceExistingMetadata && MovePath(entry.BackupPath, entry.MetadataPath, false))
        {
            failed = true;
            break;
        }
        if (MovePath(entry.MetadataPath, entry.StagingPath, false))
        {
            failed = true;
            break;
        }
        if (i == 0 && failurePoint == AssetDefaultMetadataBatchFailurePoint::AfterFirstMetadata)
        {
            failed = true;
            break;
        }
    }

    Array<String> sourcePaths;
    Array<Guid> assetIDs;
    sourcePaths.EnsureCapacity(plan.Entries.Count());
    assetIDs.EnsureCapacity(plan.Entries.Count());
    for (const MetadataBatchRollbackEntry& entry : plan.Entries)
    {
        sourcePaths.Add(entry.SourcePath);
        assetIDs.Add(entry.AssetID);
    }
    bool publicationAttempted = false;
    if (!failed)
    {
        publicationAttempted = true;
        failed = AssetOperationService::PublishDefaultMetadataBatch(assetIDs, sourcePaths);
    }
    if (failed)
    {
        const bool rollbackFailed = RollbackMetadataBatch(plan);
        const bool refreshFailed = publicationAttempted && AssetPipelineService::RefreshSources(sourcePaths, false);
        if (!rollbackFailed && !refreshFailed)
            DeleteTree(transactionDirectory);
        return true;
    }

    DeleteTree(transactionDirectory);
    return false;
}
#endif

AssetOperations::AssetOperations(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
    IAssetModificationProcessor& modificationProcessor, IAssetOperationDatabaseCallbacks& databaseCallbacks)
    : _projectRoot(projectRoot)
    , _contentRoot(contentRoot)
    , _libraryRoot(libraryRoot)
    , _stagingRoot(String(libraryRoot) / TEXT("Temp/AssetOperations"))
    , _trashRoot(String(libraryRoot) / TEXT("AssetOperations/Trash"))
    , _rootRegistry(projectRoot, libraryRoot)
    , _modificationProcessor(modificationProcessor)
    , _databaseCallbacks(databaseCallbacks)
{
    _rootRegistryValid = !_rootRegistry.RegisterProjectRoots(contentRoot, _rootRegistryDiagnostic);
}

bool AssetOperations::Initialize(AssetPipelineDiagnostic& diagnostic)
{
    if (!_rootRegistryValid)
    {
        diagnostic = _rootRegistryDiagnostic;
        return true;
    }
    if (_projectRoot.IsEmpty() || _contentRoot.IsEmpty() || _libraryRoot.IsEmpty() ||
        EnsureDirectory(_trashRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, _libraryRoot,
            TEXT("Asset operations roots are invalid or cannot be created."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::NormalizeSource(const StringView& input, AssetPathPolicy::ProjectPath& result,
    AssetPipelineDiagnostic& diagnostic) const
{
    return _rootRegistry.ResolveForGenericMutation(input, result, diagnostic);
}

bool AssetOperations::ValidateExisting(const AssetOperationTarget& target, AssetPathPolicy::ProjectPath& normalized,
    AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const
{
    if (!target.ExpectedGuid.IsValid() || NormalizeSource(target.SourcePath, normalized, diagnostic))
        return true;
    if (!FileSystem::FileExists(normalized.AbsolutePath) || AssetMeta::Load(MetaPath(normalized.AbsolutePath), meta, diagnostic))
        return true;
    if (meta.ID != target.ExpectedGuid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, normalized.AbsolutePath,
            TEXT("Asset operation target GUID no longer matches the exact source path."));
    return false;
}

bool AssetOperations::AcquirePaths(const Array<String>& paths, Array<String>& acquired,
    AssetPipelineDiagnostic& diagnostic)
{
    acquired.Clear();
    for (const String& path : paths)
    {
        String key(path);
        key.Replace('\\', '/');
        key = key.ToLower();
        if (!acquired.Contains(key))
            acquired.Add(MoveTemp(key));
    }
    if (acquired.Count() > 1)
        std::sort(acquired.Get(), acquired.Get() + acquired.Count());
    _stateLocker.Lock();
    bool conflict;
    do
    {
        conflict = false;
        for (const String& key : acquired)
        {
            if (_lockedPaths.Contains(key))
            {
                conflict = true;
                break;
            }
        }
        if (conflict)
            _locksChanged.Wait(_stateLocker);
    } while (conflict);
    for (const String& key : acquired)
        _lockedPaths.Add(key);
    _stateLocker.Unlock();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void AssetOperations::ReleasePaths(const Array<String>& acquired)
{
    _stateLocker.Lock();
    for (const String& key : acquired)
        _lockedPaths.Remove(key);
    _locksChanged.NotifyAll();
    _stateLocker.Unlock();
}

bool AssetOperations::BeginImmediateTransaction(const StringView& path, AssetPipelineDiagnostic& diagnostic)
{
    _stateLocker.Lock();
    if (_editingDepth > 0 || _pendingCommits.HasItems())
    {
        _stateLocker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, path,
            TEXT("A recoverable Content transaction cannot start while an asset editing batch is pending."));
    }
    _immediateTransactions++;
    _stateLocker.Unlock();
    return false;
}

void AssetOperations::EndImmediateTransaction()
{
    _stateLocker.Lock();
    ASSERT(_immediateTransactions > 0);
    _immediateTransactions--;
    _locksChanged.NotifyAll();
    _stateLocker.Unlock();
}

bool AssetOperations::PublishCommit(AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic)
{
    Array<AssetOperationCommit> commits;
    _stateLocker.Lock();
    if (commit.Kind == AssetOperationKind::ImporterSettings &&
        (_editingDepth > 0 || _pendingCommits.HasItems()))
    {
        _stateLocker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, commit.SourcePath,
            TEXT("Importer settings cannot be published while another asset operation batch is pending."));
    }
    if (_editingDepth > 0)
    {
        _pendingCommits.Add(commit);
        _stateLocker.Unlock();
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    commits = MoveTemp(_pendingCommits);
    _pendingCommits.Clear();
    commits.Add(commit);
    _stateLocker.Unlock();
    if (!_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        _stateLocker.Lock();
        for (const AssetOperationCommit& published : commits)
            _selfWrites.Add(published.SelfWrites);
        _stateLocker.Unlock();
        return false;
    }

    if (commit.Kind != AssetOperationKind::ImporterSettings)
    {
        _stateLocker.Lock();
        Array<AssetOperationCommit> pending = MoveTemp(_pendingCommits);
        _pendingCommits = MoveTemp(commits);
        _pendingCommits.Add(pending);
        _stateLocker.Unlock();
    }
    return true;
}

bool AssetOperations::CreateFromBytes(AssetOperationKind kind, const StringView& destination, const Span<byte>& sourceData,
    const AssetMeta& meta, AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalized;
    if (sourceData.Length() == 0 || !meta.ID.IsValid() || NormalizeSource(destination, normalized, diagnostic))
        return true;
    StringAnsi metaJson;
    if (meta.ToJson(metaJson, diagnostic))
        return true;
    const String destinationMeta = MetaPath(normalized.AbsolutePath);
    Array<String> lockPaths;
    lockPaths.Add(normalized.AbsolutePath);
    lockPaths.Add(destinationMeta);
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    if (FileSystem::FileExists(normalized.AbsolutePath) || FileSystem::FileExists(destinationMeta) ||
        FileSystem::DirectoryExists(normalized.AbsolutePath) || EnsureParent(normalized.AbsolutePath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, normalized.AbsolutePath,
            TEXT("Asset operation destination is not an exact empty source-plus-meta target."));

    OperationRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = kind;
    plan.AssetGuid = meta.ID;
    plan.DestinationPath = normalized.AbsolutePath;
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    plan.StageSourcePath = transactionDirectory / TEXT("source.stage");
    plan.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    if (PrepareOperationScratch(transactionDirectory, diagnostic) ||
        File::WriteAllBytes(plan.StageSourcePath, sourceData.Get(), sourceData.Length()) ||
        AssetMeta::SaveAtomic(plan.StageMetaPath, meta, diagnostic) ||
        MovePath(normalized.AbsolutePath, plan.StageSourcePath, false) ||
        MovePath(destinationMeta, plan.StageMetaPath, false))
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, normalized.AbsolutePath,
                TEXT("Asset create/import transaction could not publish source and metadata."));
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    DeleteTree(transactionDirectory);
    commit = AssetOperationCommit();
    commit.TransactionId = plan.TransactionId;
    commit.Kind = kind;
    commit.AssetGuid = meta.ID;
    commit.DestinationPath = normalized.AbsolutePath;
    AddSelfWrite(commit, normalized.AbsolutePath);
    AddSelfWrite(commit, destinationMeta);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::CreateAsset(const StringView& destination, const Span<byte>& sourceData, const AssetMeta& meta,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalized;
    if (NormalizeSource(destination, normalized, diagnostic))
        return true;
    AssetOperationTarget target;
    target.SourcePath = destination;
    target.ExpectedGuid = meta.ID;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Create, target, destination, diagnostic))
        return true;
    AssetOperationCommit commit;
    return CreateFromBytes(AssetOperationKind::Create, destination, sourceData, meta, commit, diagnostic) ||
           PublishCommit(commit, diagnostic);
}

bool AssetOperations::ImportAsset(const StringView& externalSource, const StringView& destination, const AssetMeta& meta,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalized;
    if (NormalizeSource(destination, normalized, diagnostic))
        return true;
    BytesContainer bytes;
    if (!FileSystem::FileExists(externalSource) || File::ReadAllBytes(externalSource, bytes) || bytes.Length() > MAX_int32)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, externalSource,
            TEXT("Import source is missing, unreadable, or too large for this operation."));
    AssetOperationTarget target;
    target.SourcePath = destination;
    target.ExpectedGuid = meta.ID;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Import, target, destination, diagnostic))
        return true;
    AssetOperationCommit commit;
    return CreateFromBytes(AssetOperationKind::Import, destination,
               Span<byte>(bytes.Get(), static_cast<int32>(bytes.Length())), meta, commit, diagnostic) ||
           PublishCommit(commit, diagnostic);
}

bool AssetOperations::MoveExact(AssetOperationKind kind, const AssetOperationTarget& target, const StringView& destination,
    AssetTrashRecord* trash, AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath source;
    AssetPathPolicy::ProjectPath destinationPath;
    AssetMeta initialMeta;
    if (ValidateExisting(target, source, initialMeta, diagnostic))
        return true;
    String destinationAbsolute;
    if (kind == AssetOperationKind::Trash || kind == AssetOperationKind::Delete)
    {
        destinationAbsolute = destination;
    }
    else if (NormalizeSource(destination, destinationPath, diagnostic))
    {
        return true;
    }
    else
    {
        destinationAbsolute = destinationPath.AbsolutePath;
    }
    const bool pathsEqualIgnoringCase = source.AbsolutePath.Compare(destinationAbsolute, StringSearchCase::IgnoreCase) == 0;
#if PLATFORM_WINDOWS
    const bool caseOnlyRename = pathsEqualIgnoringCase && source.AbsolutePath != destinationAbsolute &&
        (kind == AssetOperationKind::Move || kind == AssetOperationKind::Rename);
#else
    const bool caseOnlyRename = false;
#endif
    if (pathsEqualIgnoringCase && !caseOnlyRename)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destinationAbsolute,
            TEXT("Asset move source and destination are the same exact path."));
    const String sourceMeta = MetaPath(source.AbsolutePath);
    const String destinationMeta = MetaPath(destinationAbsolute);
    const bool canMoveFragments = kind == AssetOperationKind::Trash || kind == AssetOperationKind::Delete;
    const String sourceFragments = canMoveFragments
        ? SceneFragmentStore::GetScenePath(_projectRoot, initialMeta.ID)
        : String::Empty;
    const bool hasFragments = sourceFragments.HasChars() && FileSystem::DirectoryExists(sourceFragments);
    const String destinationFragments = hasFragments
        ? String(StringUtils::GetDirectoryName(destinationAbsolute)) / TEXT("scene-fragments")
        : String::Empty;
    Array<String> lockPaths;
    lockPaths.Add(source.AbsolutePath);
    lockPaths.Add(sourceMeta);
    lockPaths.Add(destinationAbsolute);
    lockPaths.Add(destinationMeta);
    if (hasFragments)
    {
        lockPaths.Add(sourceFragments);
        lockPaths.Add(destinationFragments);
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };

    AssetMeta currentMeta;
    AssetPathPolicy::ProjectPath currentSource;
    if (ValidateExisting(target, currentSource, currentMeta, diagnostic))
        return true;
    if (hasFragments != (sourceFragments.HasChars() && FileSystem::DirectoryExists(sourceFragments)))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, sourceFragments,
            TEXT("Private scene fragments changed during move preparation."));
    if ((!caseOnlyRename && (FileSystem::FileExists(destinationAbsolute) || FileSystem::FileExists(destinationMeta) ||
        FileSystem::DirectoryExists(destinationAbsolute))) ||
        (hasFragments && (FileSystem::FileExists(destinationFragments) || FileSystem::DirectoryExists(destinationFragments))) ||
        EnsureParent(destinationAbsolute))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destinationAbsolute,
            TEXT("Asset move destination is not an exact empty source-plus-meta target."));

    OperationRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = kind;
    plan.AssetGuid = currentMeta.ID;
    plan.SourcePath = source.AbsolutePath;
    plan.DestinationPath = destinationAbsolute;
    plan.SourceFragmentsPath = sourceFragments;
    plan.DestinationFragmentsPath = destinationFragments;
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    plan.StageSourcePath = transactionDirectory / TEXT("source.stage");
    plan.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    plan.StageFragmentsPath = transactionDirectory / TEXT("fragments.stage");
    if (PrepareOperationScratch(transactionDirectory, diagnostic) ||
        (hasFragments && MovePath(plan.StageFragmentsPath, sourceFragments, false)) ||
        MovePath(plan.StageSourcePath, source.AbsolutePath, false) ||
        MovePath(plan.StageMetaPath, sourceMeta, false) ||
        MovePath(destinationAbsolute, plan.StageSourcePath, false) ||
        MovePath(destinationMeta, plan.StageMetaPath, false) ||
        (hasFragments && MovePath(destinationFragments, plan.StageFragmentsPath, false)))
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, source.AbsolutePath,
                TEXT("Asset move transaction could not publish source and metadata together."));
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    DeleteTree(transactionDirectory);

    AssetOperationCommit commit;
    commit.TransactionId = plan.TransactionId;
    commit.Kind = kind;
    commit.AssetGuid = currentMeta.ID;
    commit.SourcePath = source.AbsolutePath;
    commit.DestinationPath = destinationAbsolute;
    if (kind != AssetOperationKind::Trash && kind != AssetOperationKind::Delete)
    {
        AddSelfWrite(commit, destinationAbsolute);
        AddSelfWrite(commit, destinationMeta);
    }
    if (trash)
    {
        trash->TransactionId = plan.TransactionId;
        trash->AssetGuid = currentMeta.ID;
        trash->OriginalSourcePath = source.AbsolutePath;
        trash->OriginalMetaPath = sourceMeta;
        trash->TrashSourcePath = destinationAbsolute;
        trash->TrashMetaPath = destinationMeta;
        trash->OriginalFragmentsPath = sourceFragments;
        trash->TrashFragmentsPath = destinationFragments;
    }
    diagnostic = AssetPipelineDiagnostic();
    return PublishCommit(commit, diagnostic);
}

bool AssetOperations::MoveAsset(const AssetOperationTarget& target, const StringView& destination,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalizedSource;
    AssetPathPolicy::ProjectPath normalizedDestination;
    AssetMeta meta;
    if (ValidateExisting(target, normalizedSource, meta, diagnostic) ||
        NormalizeSource(destination, normalizedDestination, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Move, target, destination, diagnostic))
        return true;
    return MoveExact(AssetOperationKind::Move, target, destination, nullptr, diagnostic);
}

bool AssetOperations::RenameAsset(const AssetOperationTarget& target, const StringView& newFileName,
    AssetPipelineDiagnostic& diagnostic)
{
    if (newFileName.IsEmpty() || newFileName.Contains(TEXT("/")) || newFileName.Contains(TEXT("\\")))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, target.SourcePath,
            TEXT("Asset rename requires one portable file name, not a path."));
    AssetPathPolicy::ProjectPath normalized;
    AssetMeta meta;
    if (ValidateExisting(target, normalized, meta, diagnostic))
        return true;
    const String destination = String(StringUtils::GetDirectoryName(normalized.AbsolutePath)) / newFileName;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Rename, target, destination, diagnostic))
        return true;
    return MoveExact(AssetOperationKind::Rename, target, destination, nullptr, diagnostic);
}

bool AssetOperations::CopyAsset(const AssetOperationTarget& target, const StringView& destination, Guid& copiedGuid,
    AssetPipelineDiagnostic& diagnostic)
{
    return CopyAssetInternal(target, destination, copiedGuid, diagnostic, nullptr);
}

bool AssetOperations::CopyAssetInternal(const AssetOperationTarget& target, const StringView& destination,
    Guid& copiedGuid, AssetPipelineDiagnostic& diagnostic, AssetOperationCommit* deferredCommit,
    const Guid& requestedCopiedGuid)
{
    copiedGuid = Guid::Empty;
    AssetPathPolicy::ProjectPath source;
    AssetPathPolicy::ProjectPath destinationPath;
    AssetMeta sourceMeta;
    if (ValidateExisting(target, source, sourceMeta, diagnostic) || NormalizeSource(destination, destinationPath, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Copy, target, destination, diagnostic))
        return true;
    const Guid copiedAssetGuid = requestedCopiedGuid.IsValid() ? requestedCopiedGuid : Guid::New();
    const String sourceFragments = SceneFragmentStore::GetScenePath(_projectRoot, sourceMeta.ID);
    const bool hasFragments = FileSystem::DirectoryExists(sourceFragments);
    const String destinationFragments = hasFragments
        ? SceneFragmentStore::GetScenePath(_projectRoot, copiedAssetGuid)
        : String::Empty;
    const String sourceMetaPath = MetaPath(source.AbsolutePath);
    const String destinationMeta = MetaPath(destinationPath.AbsolutePath);
    Array<String> lockPaths;
    lockPaths.Add(source.AbsolutePath);
    lockPaths.Add(sourceMetaPath);
    lockPaths.Add(destinationPath.AbsolutePath);
    lockPaths.Add(destinationMeta);
    if (hasFragments)
    {
        lockPaths.Add(sourceFragments);
        lockPaths.Add(destinationFragments);
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    AssetMeta currentMeta;
    AssetPathPolicy::ProjectPath currentSource;
    if (ValidateExisting(target, currentSource, currentMeta, diagnostic))
        return true;
    if (hasFragments != FileSystem::DirectoryExists(sourceFragments))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, sourceFragments,
            TEXT("Private scene fragments changed during copy preparation."));
    if (FileSystem::FileExists(destinationPath.AbsolutePath) || FileSystem::FileExists(destinationMeta) ||
        FileSystem::DirectoryExists(destinationPath.AbsolutePath) ||
        (hasFragments && (FileSystem::FileExists(destinationFragments) || FileSystem::DirectoryExists(destinationFragments))) ||
        EnsureParent(destinationPath.AbsolutePath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destinationPath.AbsolutePath,
            TEXT("Asset copy destination is not an exact empty source-plus-meta target."));

    AssetMeta copiedMeta = currentMeta.CloneWithNewIdentities();
    copiedMeta.ID = copiedAssetGuid;
    OperationRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = AssetOperationKind::Copy;
    plan.AssetGuid = copiedMeta.ID;
    plan.SourceAssetGuid = currentMeta.ID;
    plan.SourcePath = source.AbsolutePath;
    plan.DestinationPath = destinationPath.AbsolutePath;
    plan.SourceFragmentsPath = hasFragments ? sourceFragments : String::Empty;
    plan.DestinationFragmentsPath = destinationFragments;
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    plan.StageSourcePath = transactionDirectory / TEXT("source.stage");
    plan.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    plan.StageFragmentsPath = transactionDirectory / TEXT("fragments.stage");
    String fragmentError;
    if (PrepareOperationScratch(transactionDirectory, diagnostic) ||
        (hasFragments && SceneFragmentStore::PrepareCloneDirectory(_projectRoot, currentMeta.ID, copiedMeta.ID,
            plan.StageFragmentsPath, fragmentError)) ||
        (hasFragments
            ? PrepareRemappedSceneSource(source.AbsolutePath, plan.StageSourcePath, currentMeta.ID,
                copiedMeta.ID, fragmentError)
            : FileSystem::CopyFile(plan.StageSourcePath, source.AbsolutePath)) ||
        AssetMeta::SaveAtomic(plan.StageMetaPath, copiedMeta, diagnostic) ||
        MovePath(destinationPath.AbsolutePath, plan.StageSourcePath, false) ||
        MovePath(destinationMeta, plan.StageMetaPath, false) ||
        (hasFragments && MovePath(destinationFragments, plan.StageFragmentsPath, false)))
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, destinationPath.AbsolutePath,
                fragmentError.HasChars() ? fragmentError :
                    TEXT("Asset copy transaction could not publish source, cloned metadata, and private fragments."));
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    DeleteTree(transactionDirectory);

    if (_databaseCallbacks.ClearCopiedState(currentMeta.ID, copiedMeta.ID, diagnostic))
        return true;
    AssetOperationCommit commit;
    commit.TransactionId = plan.TransactionId;
    commit.Kind = AssetOperationKind::Copy;
    commit.AssetGuid = copiedMeta.ID;
    commit.SourceAssetGuid = currentMeta.ID;
    commit.SourcePath = source.AbsolutePath;
    commit.DestinationPath = destinationPath.AbsolutePath;
    AddSelfWrite(commit, destinationPath.AbsolutePath);
    AddSelfWrite(commit, destinationMeta);
    copiedGuid = copiedMeta.ID;
    if (deferredCommit)
    {
        *deferredCommit = MoveTemp(commit);
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    return PublishCommit(commit, diagnostic);
}

bool AssetOperations::CopyAssets(const Array<AssetCopyEntryRequest>& requests, Array<Guid>& copiedGuids,
    AssetPipelineDiagnostic& diagnostic, const AssetOperationBatchOptions* options,
    AssetOperationBatchResult* result)
{
    copiedGuids.Clear();
    if (result)
        *result = AssetOperationBatchResult();
    const int32 maximumEntries = options ? options->MaximumEntries : MaximumTrashEntries;
    const auto isCancelled = [options]()
    {
        return options && options->Cancel && *options->Cancel;
    };
    if (requests.IsEmpty() || maximumEntries <= 0 || maximumEntries > MaximumTrashEntries ||
        requests.Count() > maximumEntries)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, StringView::Empty,
            TEXT("A native copy batch must contain a bounded non-empty entry set."));

    Array<AssetCopyEntryRequest> expandedRequests(requests);
    const auto hasEntry = [&expandedRequests](const StringView& source, const StringView& destination)
    {
        for (const AssetCopyEntryRequest& entry : expandedRequests)
        {
            if (FileSystem::AreFilePathsEquivalent(entry.SourcePath, source) &&
                FileSystem::AreFilePathsEquivalent(entry.DestinationPath, destination))
                return true;
        }
        return false;
    };
    Array<AssetCopyEntryRequest> selectedDirectories;
    for (int32 i = 0; i < requests.Count(); i++)
    {
        if (requests[i].Kind != AssetCopyEntryKind::Directory)
            continue;
        bool alreadyFlattened = false;
        for (int32 j = 0; j < requests.Count(); j++)
        {
            if (i != j && AssetPathPolicy::IsSameOrChild(requests[j].SourcePath, requests[i].SourcePath))
            {
                alreadyFlattened = true;
                break;
            }
        }
        if (!alreadyFlattened)
            selectedDirectories.Add(requests[i]);
    }
    for (int32 directoryIndex = 0; directoryIndex < selectedDirectories.Count(); directoryIndex++)
    {
        if (isCancelled())
        {
            if (result)
            {
                result->Cancelled = true;
                result->TotalEntries = expandedRequests.Count();
                result->FailureIndex = expandedRequests.Count();
                result->FailurePath = selectedDirectories[directoryIndex].SourcePath;
            }
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled,
                selectedDirectories[directoryIndex].SourcePath, TEXT("Native copy discovery was cancelled."));
        }
        const AssetCopyEntryRequest directory = selectedDirectories[directoryIndex];
        Array<String> files;
        Array<String> directories;
        if (FileSystem::DirectoryGetFiles(files, directory.SourcePath, TEXT("*"), DirectorySearchOption::TopDirectoryOnly) ||
            FileSystem::GetChildDirectories(directories, directory.SourcePath))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, directory.SourcePath,
                TEXT("A selected Content folder could not be enumerated."));
        if (files.Count() > 1)
            std::sort(files.Get(), files.Get() + files.Count());
        if (directories.Count() > 1)
            std::sort(directories.Get(), directories.Get() + directories.Count());

        const String directoryMeta = directory.SourcePath + TEXT(".meta");
        const String destinationMeta = directory.DestinationPath + TEXT(".meta");
        if (FileSystem::FileExists(directoryMeta) && !hasEntry(directoryMeta, destinationMeta))
        {
            AssetCopyEntryRequest& metadata = expandedRequests.AddOne();
            metadata.SourcePath = directoryMeta;
            metadata.DestinationPath = destinationMeta;
            metadata.Kind = AssetCopyEntryKind::MetadataSidecar;
        }
        for (const String& childDirectory : directories)
        {
            AssetCopyEntryRequest child;
            child.SourcePath = childDirectory;
            child.DestinationPath = directory.DestinationPath / StringUtils::GetFileName(childDirectory);
            child.Kind = AssetCopyEntryKind::Directory;
            if (!hasEntry(child.SourcePath, child.DestinationPath))
                expandedRequests.Add(child);
            selectedDirectories.Add(MoveTemp(child));
        }
        for (const String& file : files)
        {
            if (file.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase))
            {
                const String owner = file.Left(file.Length() - 5);
                if (FileSystem::FileExists(owner) || FileSystem::DirectoryExists(owner))
                    continue;
            }
            AssetCopyEntryRequest child;
            child.SourcePath = file;
            child.DestinationPath = directory.DestinationPath / StringUtils::GetFileName(file);
            const String metaPath = file + TEXT(".meta");
            if (FileSystem::FileExists(metaPath))
            {
                AssetMeta meta;
                if (AssetMeta::Load(metaPath, meta, diagnostic))
                {
                    if (result)
                    {
                        result->FailureIndex = expandedRequests.Count();
                        result->FailurePath = file;
                    }
                    return true;
                }
                child.ExpectedAssetGuid = meta.ID;
                child.Kind = AssetCopyEntryKind::CanonicalAsset;
            }
            else
            {
                child.Kind = AssetCopyEntryKind::File;
            }
            if (!hasEntry(child.SourcePath, child.DestinationPath))
                expandedRequests.Add(MoveTemp(child));
        }
        if (expandedRequests.Count() > maximumEntries)
        {
            if (result)
            {
                result->TotalEntries = expandedRequests.Count();
                result->FailureIndex = maximumEntries;
                result->FailurePath = directory.SourcePath;
            }
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, directory.SourcePath,
                TEXT("Recursive native copy discovery exceeded its bounded entry limit."));
        }
    }
    if (result)
        result->TotalEntries = expandedRequests.Count();

    Array<AssetOperationTarget> targets;
    Array<String> destinations;
    Array<Guid> plannedGuids;
    Array<AssetMeta> metadataClones;
    Array<uint64> sourceSizes;
    Array<int64> sourceWriteTicks;
    Array<String> reservedDestinations;
    targets.EnsureCapacity(expandedRequests.Count());
    destinations.EnsureCapacity(expandedRequests.Count());
    plannedGuids.EnsureCapacity(expandedRequests.Count());
    metadataClones.EnsureCapacity(expandedRequests.Count());
    sourceSizes.EnsureCapacity(expandedRequests.Count());
    sourceWriteTicks.EnsureCapacity(expandedRequests.Count());
    BatchRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = BatchRollbackKind::Copy;
    plan.Trash.TransactionId = plan.TransactionId;
    const auto reserveDestination = [this, &reservedDestinations, &diagnostic](const StringView& path)
    {
        for (const String& previous : reservedDestinations)
        {
            if (FileSystem::AreFilePathsEquivalent(previous, path))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, path,
                    TEXT("A native copy batch repeats a destination path."));
        }
        reservedDestinations.Add(String(path));
        return false;
    };
    for (int32 i = 0; i < expandedRequests.Count(); i++)
    {
        const AssetCopyEntryRequest& request = expandedRequests[i];
        if (result)
        {
            result->FailureIndex = i;
            result->FailurePath = request.SourcePath;
        }
        if (isCancelled())
        {
            if (result)
                result->Cancelled = true;
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request.SourcePath,
                TEXT("Native copy preparation was cancelled."));
        }
        AssetOperationTarget target;
        target.SourcePath = request.SourcePath;
        target.ExpectedGuid = request.ExpectedAssetGuid;
        AssetPathPolicy::ProjectPath source;
        AssetPathPolicy::ProjectPath destination;
        AssetMeta sourceMeta;
        AssetMeta metadataClone;
        Guid copiedGuid = Guid::Empty;
        String sourceFragments;
        String destinationFragments;
        bool hasFragments = false;
        uint64 sourceSize = 0;
        int64 sourceWriteTime = 0;
        if (NormalizeSource(request.DestinationPath, destination, diagnostic))
            return true;

        if (request.Kind == AssetCopyEntryKind::CanonicalAsset)
        {
            if (ValidateExisting(target, source, sourceMeta, diagnostic))
                return true;
            copiedGuid = Guid::New();
            sourceFragments = SceneFragmentStore::GetScenePath(_projectRoot, sourceMeta.ID);
            destinationFragments = SceneFragmentStore::GetScenePath(_projectRoot, copiedGuid);
            hasFragments = FileSystem::DirectoryExists(sourceFragments);
            if (PathExists(destination.AbsolutePath) || PathExists(MetaPath(destination.AbsolutePath)) ||
                (hasFragments && PathExists(destinationFragments)))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destination.AbsolutePath,
                    TEXT("A canonical copy destination is not empty."));
            if (reserveDestination(destination.AbsolutePath) || reserveDestination(MetaPath(destination.AbsolutePath)) ||
                (hasFragments && reserveDestination(destinationFragments)))
                return true;
        }
        else
        {
            plan.Kind = BatchRollbackKind::ContentCopy;
            if (request.Kind != AssetCopyEntryKind::File && request.Kind != AssetCopyEntryKind::Directory &&
                request.Kind != AssetCopyEntryKind::MetadataSidecar)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, request.SourcePath,
                    TEXT("A native copy entry kind is unsupported."));
            if (request.ExpectedAssetGuid.IsValid())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, request.SourcePath,
                    TEXT("Only canonical copy entries may specify an expected asset GUID."));
            if (NormalizeSource(request.SourcePath, source, diagnostic))
                return true;
            const bool isDirectory = request.Kind == AssetCopyEntryKind::Directory;
            if (isDirectory ? !FileSystem::DirectoryExists(source.AbsolutePath) : !FileSystem::FileExists(source.AbsolutePath))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, source.AbsolutePath,
                    TEXT("A native copy source is missing or has the wrong filesystem type."));
            if (ContainsFileSystemLink(source.AbsolutePath, isDirectory))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, source.AbsolutePath,
                    TEXT("Native copy batches reject filesystem links."));
            if (isDirectory && AssetPathPolicy::IsSameOrChild(destination.AbsolutePath, source.AbsolutePath))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destination.AbsolutePath,
                    TEXT("A folder cannot be copied into itself or a descendant."));
            if (PathExists(destination.AbsolutePath) || reserveDestination(destination.AbsolutePath))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destination.AbsolutePath,
                    TEXT("A native copy destination is not empty."));
            if (request.Kind == AssetCopyEntryKind::MetadataSidecar)
            {
                if (!IsMetaPath(source.AbsolutePath) || !IsMetaPath(destination.AbsolutePath) ||
                    AssetMeta::Load(source.AbsolutePath, sourceMeta, diagnostic))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, source.AbsolutePath,
                        TEXT("A metadata copy entry must name a valid source and destination sidecar."));
                metadataClone = sourceMeta.CloneWithNewIdentities();
                copiedGuid = metadataClone.ID;
            }
            sourceSize = isDirectory ? 0 : FileSystem::GetFileSize(source.AbsolutePath);
            sourceWriteTime = FileSystem::GetFileLastEditTime(source.AbsolutePath).Ticks;
            target.SourcePath = source.AbsolutePath;
            target.ExpectedGuid = sourceMeta.ID;
            if (_modificationProcessor.ValidateOperation(AssetOperationKind::Copy, target,
                destination.AbsolutePath, diagnostic))
                return true;
        }

        const String destinationParent = StringUtils::GetDirectoryName(destination.AbsolutePath);
        bool destinationParentExists = FileSystem::DirectoryExists(destinationParent);
        for (int32 j = 0; !destinationParentExists && j < destinations.Count(); j++)
        {
            destinationParentExists = expandedRequests[j].Kind == AssetCopyEntryKind::Directory &&
                FileSystem::AreFilePathsEquivalent(destinations[j], destinationParent);
        }
        if (!destinationParentExists)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, destination.AbsolutePath,
                TEXT("Native copy entries require an existing or earlier planned destination folder."));

        targets.Add(MoveTemp(target));
        destinations.Add(destination.AbsolutePath);
        plannedGuids.Add(copiedGuid);
        metadataClones.Add(MoveTemp(metadataClone));
        sourceSizes.Add(sourceSize);
        sourceWriteTicks.Add(sourceWriteTime);
        AssetTrashEntry& entry = plan.Trash.Entries.AddOne();
        entry.AssetGuid = copiedGuid;
        entry.OriginalPath = source.AbsolutePath;
        entry.TrashPath = destination.AbsolutePath;
        entry.IsFolder = request.Kind == AssetCopyEntryKind::Directory;
        if (request.Kind == AssetCopyEntryKind::CanonicalAsset)
        {
            entry.OriginalMetaPath = MetaPath(source.AbsolutePath);
            entry.TrashMetaPath = MetaPath(destination.AbsolutePath);
        }
        if (hasFragments)
        {
            AssetTrashFragment& fragment = entry.Fragments.AddOne();
            fragment.OriginalPath = sourceFragments;
            fragment.TrashPath = destinationFragments;
        }
    }

    if (BeginImmediateTransaction(plan.Trash.Entries[0].OriginalPath, diagnostic))
        return true;
    SCOPE_EXIT { EndImmediateTransaction(); };
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    if (PrepareBatchScratch(transactionDirectory, diagnostic))
        return true;

    Array<AssetOperationCommit> commits;
    commits.EnsureCapacity(expandedRequests.Count());
    bool publicationAttempted = false;
    const auto failAndRollback = [this, &plan, &transactionDirectory, &diagnostic, &commits,
        &publicationAttempted, result](
        const AssetPipelineDiagnostic& failure)
    {
        const bool rollbackFailed = RollbackBatch(plan);
        if (result && !rollbackFailed)
            result->RolledBackEntries = result->CompletedEntries;
        if (!rollbackFailed)
        {
            if (publicationAttempted && commits.HasItems())
            {
                Array<AssetOperationCommit> rollbackCommits;
                rollbackCommits.EnsureCapacity(commits.Count());
                for (const AssetOperationCommit& published : commits)
                {
                    AssetOperationCommit& rollback = rollbackCommits.AddOne();
                    rollback.TransactionId = plan.TransactionId;
                    rollback.Kind = AssetOperationKind::Delete;
                    rollback.AssetGuid = published.AssetGuid;
                    rollback.SourcePath = published.DestinationPath;
                }
                AssetPipelineDiagnostic ignored;
                _databaseCallbacks.RefreshCommitted(rollbackCommits, ignored);
            }
            DeleteTree(transactionDirectory);
        }
        diagnostic = failure;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    };

    for (int32 i = 0; i < expandedRequests.Count(); i++)
    {
        if (result)
        {
            result->FailureIndex = i;
            result->FailurePath = expandedRequests[i].SourcePath;
        }
        if (isCancelled())
        {
            if (result)
                result->Cancelled = true;
            Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, expandedRequests[i].SourcePath,
                TEXT("Native copy commit was cancelled."));
            const AssetPipelineDiagnostic failure = diagnostic;
            return failAndRollback(failure);
        }
        Guid copiedGuid = Guid::Empty;
        AssetOperationCommit commit;
        if (expandedRequests[i].Kind == AssetCopyEntryKind::CanonicalAsset)
        {
            if (CopyAssetInternal(targets[i], destinations[i], copiedGuid, diagnostic, &commit, plannedGuids[i]) ||
                copiedGuid != plannedGuids[i])
            {
                const AssetPipelineDiagnostic failure = diagnostic;
                return failAndRollback(failure);
            }
        }
        else
        {
            const bool isDirectory = expandedRequests[i].Kind == AssetCopyEntryKind::Directory;
            const String& source = plan.Trash.Entries[i].OriginalPath;
            if ((isDirectory ? !FileSystem::DirectoryExists(source) : !FileSystem::FileExists(source)) ||
                PathExists(destinations[i]) ||
                FileSystem::GetFileLastEditTime(source).Ticks != sourceWriteTicks[i] ||
                (!isDirectory && FileSystem::GetFileSize(source) != sourceSizes[i]))
            {
                Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, source,
                    TEXT("A native copy source or destination changed after preflight."));
                const AssetPipelineDiagnostic failure = diagnostic;
                return failAndRollback(failure);
            }
            bool copyFailed = false;
            if (isDirectory)
                copyFailed = EnsureDirectory(destinations[i]);
            else if (expandedRequests[i].Kind == AssetCopyEntryKind::MetadataSidecar)
                copyFailed = AssetMeta::SaveAtomic(destinations[i], metadataClones[i], diagnostic);
            else
                copyFailed = FileSystem::CopyFile(destinations[i], source);
            if (copyFailed)
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                    Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, destinations[i],
                        TEXT("A native copy batch entry could not be committed."));
                const AssetPipelineDiagnostic failure = diagnostic;
                return failAndRollback(failure);
            }
            copiedGuid = plannedGuids[i];
            commit.Kind = AssetOperationKind::Copy;
            commit.AssetGuid = copiedGuid;
            commit.SourcePath = source;
            commit.DestinationPath = destinations[i];
            if (!isDirectory)
                AddSelfWrite(commit, destinations[i]);
        }
        commit.TransactionId = plan.TransactionId;
        for (AssetOperationSelfWrite& write : commit.SelfWrites)
            write.TransactionId = plan.TransactionId;
        commits.Add(MoveTemp(commit));
        if (result)
            result->CompletedEntries = i + 1;
    }

    {
        const AssetPipelineDiagnostic failure = diagnostic;
        return failAndRollback(failure);
    }
    publicationAttempted = true;
    if (_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        const AssetPipelineDiagnostic failure = diagnostic;
        return failAndRollback(failure);
    }
    {
        const AssetPipelineDiagnostic failure = diagnostic;
        return failAndRollback(failure);
    }

    _stateLocker.Lock();
    for (const AssetOperationCommit& commit : commits)
        _selfWrites.Add(commit.SelfWrites);
    _stateLocker.Unlock();
    copiedGuids = MoveTemp(plannedGuids);
    DeleteTree(transactionDirectory);
    if (result)
    {
        result->FailureIndex = -1;
        result->FailurePath.Clear();
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::WriteImporterSettings(const AssetOperationTarget& target,
    const AssetImporterSettingsRevision& expected, int32 settingsVersion, const StringAnsiView& settingsJson,
    AssetPipelineDiagnostic& diagnostic, AssetMetaWriteFailurePoint failurePoint, bool* wasChanged,
    bool* wasConflict)
{
    if (wasChanged)
        *wasChanged = false;
    if (wasConflict)
        *wasConflict = false;
    if (expected.SourceRevision == 0 || expected.ImporterID.IsEmpty() ||
        expected.StoredSettingsVersion < 1 || settingsVersion < 1)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, target.SourcePath,
            TEXT("Importer settings revision is invalid."));
    if (IsAssetEditing())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, target.SourcePath,
            TEXT("Importer settings cannot be saved inside an asset editing batch."));

    AssetPathPolicy::ProjectPath source;
    if (NormalizeSource(target.SourcePath, source, diagnostic))
        return true;
    const String metaPath = MetaPath(source.AbsolutePath);
    Array<String> lockPaths;
    lockPaths.Add(source.AbsolutePath);
    lockPaths.Add(metaPath);
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };

    AssetMeta current;
    AssetPathPolicy::ProjectPath currentSource;
    if (ValidateExisting(target, currentSource, current, diagnostic))
    {
        if (wasConflict && diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated)
            *wasConflict = true;
        return true;
    }
    if (_databaseCallbacks.ValidateImporterSettingsRevision(target, expected, diagnostic))
    {
        if (wasConflict && diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated)
            *wasConflict = true;
        return true;
    }
    uint64 currentSemanticHash;
    if (GetMetaSemanticHash(current, currentSemanticHash, diagnostic))
        return true;
    if (currentSemanticHash != expected.MetaSemanticHash || current.Processor.ID != expected.ImporterID ||
        current.Processor.SettingsVersion != expected.StoredSettingsVersion)
    {
        if (wasConflict)
            *wasConflict = true;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, source.AbsolutePath,
            TEXT("Importer settings metadata changed after the editor revision was captured."));
    }

    StringAnsi canonicalSettings;
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Canonicalize(settingsJson, canonicalSettings, jsonError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, source.AbsolutePath, jsonError.Message);
    rapidjson_flax::Document settingsDocument;
    settingsDocument.Parse(canonicalSettings.Get(), canonicalSettings.Length());
    if (settingsDocument.HasParseError() || !settingsDocument.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, source.AbsolutePath,
            TEXT("Importer settings must be a JSON object."));
    if (current.Processor.SettingsVersion == settingsVersion && current.Processor.SettingsJson == canonicalSettings)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    ReleasePaths(acquired);
    acquired.Clear();
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::ImporterSettings, target,
        StringView::Empty, diagnostic))
        return true;
    AcquirePaths(lockPaths, acquired, diagnostic);
    AssetMeta authorizedCurrent;
    if (ValidateExisting(target, currentSource, authorizedCurrent, diagnostic))
    {
        if (wasConflict && diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated)
            *wasConflict = true;
        return true;
    }
    if (_databaseCallbacks.ValidateImporterSettingsRevision(target, expected, diagnostic))
    {
        if (wasConflict && diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated)
            *wasConflict = true;
        return true;
    }
    if (GetMetaSemanticHash(authorizedCurrent, currentSemanticHash, diagnostic))
        return true;
    if (currentSemanticHash != expected.MetaSemanticHash || authorizedCurrent.Processor.ID != expected.ImporterID ||
        authorizedCurrent.Processor.SettingsVersion != expected.StoredSettingsVersion)
    {
        if (wasConflict)
            *wasConflict = true;
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, source.AbsolutePath,
            TEXT("Importer settings metadata changed while the save was authorized."));
    }
    current = MoveTemp(authorizedCurrent);

    AssetMeta updated = current;
    updated.Processor.SettingsVersion = settingsVersion;
    updated.Processor.SettingsJson = MoveTemp(canonicalSettings);
    BytesContainer previousMeta;
    if (File::ReadAllBytes(metaPath, previousMeta))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, metaPath,
            TEXT("Cannot capture importer metadata before atomic replacement."));
    if (AssetMeta::SaveAtomic(metaPath, updated, diagnostic, nullptr, failurePoint))
        return true;

    AssetOperationCommit commit;
    commit.TransactionId = Guid::New();
    commit.Kind = AssetOperationKind::ImporterSettings;
    commit.AssetGuid = current.ID;
    commit.SourceAssetGuid = current.ID;
    commit.SourcePath = source.AbsolutePath;
    AddSelfWrite(commit, metaPath);
    if (!PublishCommit(commit, diagnostic))
    {
        if (wasChanged)
            *wasChanged = true;
        return false;
    }

    const AssetPipelineDiagnostic publicationDiagnostic = diagnostic;
    if (RestoreFileAtomic(metaPath, previousMeta))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = metaPath;
        diagnostic.Message = String::Format(TEXT("Importer settings database publication failed and the prior metadata could not be restored. {0}"), publicationDiagnostic.Message);
        return true;
    }
    AssetOperationCommit rollback;
    rollback.TransactionId = commit.TransactionId;
    AddSelfWrite(rollback, metaPath);
    _stateLocker.Lock();
    _selfWrites.Add(rollback.SelfWrites);
    _stateLocker.Unlock();
    diagnostic = publicationDiagnostic;
    return true;
}

bool AssetOperations::TrashAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
    AssetPipelineDiagnostic& diagnostic)
{
    trash = AssetTrashRecord();
    AssetPathPolicy::ProjectPath source;
    AssetMeta meta;
    if (ValidateExisting(target, source, meta, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Trash, target, StringView::Empty, diagnostic))
        return true;
    const String trashDirectory = _trashRoot / Guid::New().ToString(Guid::FormatType::N);
    const String destination = trashDirectory / StringUtils::GetFileName(source.AbsolutePath);
    return MoveExact(AssetOperationKind::Trash, target, destination, &trash, diagnostic);
}

bool AssetOperations::DeleteAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
    AssetPipelineDiagnostic& diagnostic)
{
    trash = AssetTrashRecord();
    AssetPathPolicy::ProjectPath source;
    AssetMeta meta;
    if (ValidateExisting(target, source, meta, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Delete, target, StringView::Empty, diagnostic))
        return true;
    const String trashDirectory = _trashRoot / Guid::New().ToString(Guid::FormatType::N);
    const String destination = trashDirectory / StringUtils::GetFileName(source.AbsolutePath);
    return MoveExact(AssetOperationKind::Delete, target, destination, &trash, diagnostic);
}

bool AssetOperations::RestoreAsset(const AssetTrashRecord& trash, AssetPipelineDiagnostic& diagnostic)
{
    const bool hasFragments = trash.TrashFragmentsPath.HasChars();
    if (!trash.AssetGuid.IsValid() || !FileSystem::FileExists(trash.TrashSourcePath) ||
        !FileSystem::FileExists(trash.TrashMetaPath) ||
        (hasFragments && (trash.OriginalFragmentsPath.IsEmpty() || !FileSystem::DirectoryExists(trash.TrashFragmentsPath))))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, trash.TrashSourcePath,
            TEXT("Asset trash record is incomplete or its recoverable files are missing."));
    AssetMeta meta;
    if (AssetMeta::Load(trash.TrashMetaPath, meta, diagnostic) || meta.ID != trash.AssetGuid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, trash.TrashMetaPath,
            TEXT("Asset trash metadata no longer matches the exact restore target."));
    AssetPathPolicy::ProjectPath normalizedDestination;
    if (NormalizeSource(trash.OriginalSourcePath, normalizedDestination, diagnostic))
        return true;
    AssetOperationTarget callbackTarget;
    callbackTarget.SourcePath = trash.OriginalSourcePath;
    callbackTarget.ExpectedGuid = trash.AssetGuid;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Restore, callbackTarget,
        trash.OriginalSourcePath, diagnostic))
        return true;

    Array<String> lockPaths;
    lockPaths.Add(trash.TrashSourcePath);
    lockPaths.Add(trash.TrashMetaPath);
    lockPaths.Add(trash.OriginalSourcePath);
    lockPaths.Add(trash.OriginalMetaPath);
    if (hasFragments)
    {
        lockPaths.Add(trash.TrashFragmentsPath);
        lockPaths.Add(trash.OriginalFragmentsPath);
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    if (FileSystem::FileExists(trash.OriginalSourcePath) || FileSystem::FileExists(trash.OriginalMetaPath) ||
        (hasFragments && (FileSystem::FileExists(trash.OriginalFragmentsPath) ||
            FileSystem::DirectoryExists(trash.OriginalFragmentsPath))) ||
        EnsureParent(trash.OriginalSourcePath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, trash.OriginalSourcePath,
            TEXT("Asset restore destination is no longer empty."));
    AssetMeta currentMeta;
    if (AssetMeta::Load(trash.TrashMetaPath, currentMeta, diagnostic) || currentMeta.ID != trash.AssetGuid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, trash.TrashMetaPath,
            TEXT("Asset trash metadata changed during restore validation."));

    OperationRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = AssetOperationKind::Restore;
    plan.AssetGuid = trash.AssetGuid;
    plan.SourcePath = trash.TrashSourcePath;
    plan.DestinationPath = trash.OriginalSourcePath;
    plan.SourceFragmentsPath = trash.TrashFragmentsPath;
    plan.DestinationFragmentsPath = trash.OriginalFragmentsPath;
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    plan.StageSourcePath = transactionDirectory / TEXT("source.stage");
    plan.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    plan.StageFragmentsPath = transactionDirectory / TEXT("fragments.stage");
    if (PrepareOperationScratch(transactionDirectory, diagnostic) ||
        (hasFragments && MovePath(plan.StageFragmentsPath, trash.TrashFragmentsPath, false)) ||
        MovePath(plan.StageSourcePath, trash.TrashSourcePath, false) ||
        MovePath(plan.StageMetaPath, trash.TrashMetaPath, false) ||
        MovePath(trash.OriginalSourcePath, plan.StageSourcePath, false) ||
        MovePath(trash.OriginalMetaPath, plan.StageMetaPath, false) ||
        (hasFragments && MovePath(trash.OriginalFragmentsPath, plan.StageFragmentsPath, false)))
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, trash.OriginalSourcePath,
                TEXT("Asset restore transaction could not publish source and metadata."));
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    {
        RollbackOperation(plan);
        DeleteTree(transactionDirectory);
        return true;
    }
    DeleteTree(transactionDirectory);
    AssetOperationCommit commit;
    commit.TransactionId = plan.TransactionId;
    commit.Kind = AssetOperationKind::Restore;
    commit.AssetGuid = trash.AssetGuid;
    commit.SourcePath = trash.TrashSourcePath;
    commit.DestinationPath = trash.OriginalSourcePath;
    AddSelfWrite(commit, trash.OriginalSourcePath);
    AddSelfWrite(commit, trash.OriginalMetaPath);
    return PublishCommit(commit, diagnostic);
}

bool AssetOperations::TrashEntries(const Array<AssetTrashEntryRequest>& requests, AssetTrashBatch& trash,
    AssetPipelineDiagnostic& diagnostic, const AssetOperationBatchOptions* options,
    AssetOperationBatchResult* result)
{
    trash = AssetTrashBatch();
    if (result)
        *result = AssetOperationBatchResult();
    const int32 maximumEntries = options ? options->MaximumEntries : MaximumTrashEntries;
    const auto isCancelled = [options]()
    {
        return options && options->Cancel && *options->Cancel;
    };
    if (requests.IsEmpty() || maximumEntries <= 0 || maximumEntries > MaximumTrashEntries ||
        requests.Count() > maximumEntries)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, StringView::Empty,
            TEXT("A recoverable Content trash batch must contain a bounded non-empty entry set."));
    if (result)
        result->TotalEntries = requests.Count();

    const Guid transactionId = Guid::New();
    const String trashRoot = _trashRoot / transactionId.ToString(Guid::FormatType::N);
    const auto prepare = [this, &requests, &transactionId, &trashRoot, &isCancelled, result](AssetTrashBatch& output,
        AssetPipelineDiagnostic& prepareDiagnostic)
    {
        output = AssetTrashBatch();
        output.TransactionId = transactionId;
        for (int32 i = 0; i < requests.Count(); i++)
        {
            const AssetTrashEntryRequest& request = requests[i];
            if (result)
            {
                result->FailureIndex = i;
                result->FailurePath = request.SourcePath;
            }
            if (isCancelled())
            {
                if (result)
                    result->Cancelled = true;
                return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request.SourcePath,
                    TEXT("Native trash preparation was cancelled."));
            }
            AssetPathPolicy::ProjectPath source;
            if (NormalizeSource(request.SourcePath, source, prepareDiagnostic))
                return true;
            if (FileSystem::AreFilePathsEquivalent(source.AbsolutePath, _contentRoot))
                return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, source.AbsolutePath,
                    TEXT("The canonical Content root cannot be staged for deletion."));
            const bool isFile = FileSystem::FileExists(source.AbsolutePath);
            const bool isFolder = FileSystem::DirectoryExists(source.AbsolutePath);
            if (request.IsFolder ? (!isFolder || isFile) : (!isFile || isFolder))
                return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::SourceMissing, source.AbsolutePath,
                    TEXT("The recoverable Content entry type no longer matches the selected source."));
            if (ContainsFileSystemLink(source.AbsolutePath, request.IsFolder))
                return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, source.AbsolutePath,
                    TEXT("Recoverable Content trash rejects filesystem links and reparse points."));
            for (const AssetTrashEntry& previous : output.Entries)
            {
                if (AssetPathPolicy::IsSameOrChild(source.AbsolutePath, previous.OriginalPath) ||
                    AssetPathPolicy::IsSameOrChild(previous.OriginalPath, source.AbsolutePath))
                    return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::PathCollision, source.AbsolutePath,
                        TEXT("Recoverable Content trash entries must be distinct top-level paths."));
            }

            AssetTrashEntry entry;
            entry.IsFolder = request.IsFolder;
            entry.OriginalPath = source.AbsolutePath;
            const String entryRoot = trashRoot / String::Format(TEXT("{0}"), i);
            entry.TrashPath = entryRoot / StringUtils::GetFileName(source.AbsolutePath);
            const String metaPath = MetaPath(source.AbsolutePath);
            if (FileSystem::FileExists(metaPath))
            {
                if (ContainsFileSystemLink(metaPath, false))
                    return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, metaPath,
                        TEXT("Recoverable Content trash rejects linked metadata sidecars."));
                AssetMeta meta;
                if (AssetMeta::Load(metaPath, meta, prepareDiagnostic))
                    return true;
                if (meta.FolderAsset != request.IsFolder ||
                    (request.ExpectedAssetGuid.IsValid() && meta.ID != request.ExpectedAssetGuid))
                    return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, metaPath,
                        TEXT("The selected Content entry metadata identity or folder kind changed."));
                entry.AssetGuid = meta.ID;
                entry.OriginalMetaPath = metaPath;
                entry.TrashMetaPath = entry.TrashPath + TEXT(".meta");
            }
            else if (request.ExpectedAssetGuid.IsValid())
            {
                return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::SourceMissing, metaPath,
                    TEXT("The selected asset metadata sidecar is missing."));
            }

            Array<String> sceneMetaPaths;
            if (request.IsFolder)
            {
                if (FileSystem::DirectoryGetFiles(sceneMetaPaths, source.AbsolutePath, TEXT("*.scene.meta"),
                    DirectorySearchOption::AllDirectories))
                    return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::SourceBusy, source.AbsolutePath,
                        TEXT("Cannot enumerate scene metadata below the selected Content folder."));
            }
            else if (source.AbsolutePath.EndsWith(TEXT(".scene"), StringSearchCase::IgnoreCase) &&
                entry.OriginalMetaPath.HasChars())
            {
                sceneMetaPaths.Add(entry.OriginalMetaPath);
            }
            for (const String& sceneMetaPath : sceneMetaPaths)
            {
                if (!sceneMetaPath.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase))
                    continue;
                const String scenePath = sceneMetaPath.Substring(0, sceneMetaPath.Length() - 5);
                if (!scenePath.EndsWith(TEXT(".scene"), StringSearchCase::IgnoreCase) ||
                    !FileSystem::FileExists(scenePath))
                    continue;
                AssetMeta sceneMeta;
                if (AssetMeta::Load(sceneMetaPath, sceneMeta, prepareDiagnostic))
                    return true;
                const String fragments = SceneFragmentStore::GetScenePath(_projectRoot, sceneMeta.ID);
                if (!FileSystem::DirectoryExists(fragments))
                    continue;
                if (ContainsFileSystemLink(fragments, true))
                    return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, fragments,
                        TEXT("Recoverable Content trash rejects linked private scene fragments."));
                const String trashFragments = trashRoot / TEXT("ExternalActors") /
                    sceneMeta.ID.ToString(Guid::FormatType::N);
                bool alreadyAdded = false;
                for (const AssetTrashFragment& fragment : entry.Fragments)
                    alreadyAdded |= FileSystem::AreFilePathsEquivalent(fragment.TrashPath, trashFragments);
                if (alreadyAdded)
                    continue;
                for (const AssetTrashEntry& previous : output.Entries)
                {
                    for (const AssetTrashFragment& fragment : previous.Fragments)
                    {
                        if (FileSystem::AreFilePathsEquivalent(fragment.TrashPath, trashFragments))
                            return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::PathCollision, fragments,
                                TEXT("Selected scenes repeat a private fragment owner identity."));
                    }
                }
                AssetTrashFragment fragment;
                fragment.OriginalPath = fragments;
                fragment.TrashPath = trashFragments;
                entry.Fragments.Add(MoveTemp(fragment));
            }
            if (PathExists(entry.TrashPath) || PathExists(entry.TrashMetaPath))
                return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::PathCollision, entry.TrashPath,
                    TEXT("The native recovery path already exists."));
            for (const AssetTrashFragment& fragment : entry.Fragments)
            {
                if (PathExists(fragment.TrashPath))
                    return Fail(prepareDiagnostic, AssetPipelineDiagnosticCode::PathCollision, fragment.TrashPath,
                        TEXT("A private scene fragment recovery path already exists."));
            }
            output.Entries.Add(MoveTemp(entry));
        }
        return false;
    };

    AssetTrashBatch prepared;
    if (prepare(prepared, diagnostic))
        return true;
    for (int32 i = 0; i < prepared.Entries.Count(); i++)
    {
        const AssetTrashEntry& entry = prepared.Entries[i];
        if (result)
        {
            result->FailureIndex = i;
            result->FailurePath = entry.OriginalPath;
        }
        AssetOperationTarget target;
        target.SourcePath = entry.OriginalPath;
        target.ExpectedGuid = entry.AssetGuid;
        if (_modificationProcessor.ValidateOperation(AssetOperationKind::Trash, target, StringView::Empty, diagnostic))
            return true;
    }
    if (BeginImmediateTransaction(prepared.Entries[0].OriginalPath, diagnostic))
        return true;
    SCOPE_EXIT { EndImmediateTransaction(); };

    Array<String> lockPaths;
    lockPaths.Add(trashRoot);
    for (const AssetTrashEntry& entry : prepared.Entries)
    {
        lockPaths.Add(entry.OriginalPath);
        lockPaths.Add(entry.TrashPath);
        if (entry.OriginalMetaPath.HasChars())
        {
            lockPaths.Add(entry.OriginalMetaPath);
            lockPaths.Add(entry.TrashMetaPath);
        }
        for (const AssetTrashFragment& fragment : entry.Fragments)
        {
            lockPaths.Add(fragment.OriginalPath);
            lockPaths.Add(fragment.TrashPath);
        }
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    AssetTrashBatch current;
    if (prepare(current, diagnostic) || current.Entries.Count() != prepared.Entries.Count())
        return true;
    for (int32 i = 0; i < prepared.Entries.Count(); i++)
    {
        if (!EqualTrashEntry(prepared.Entries[i], current.Entries[i]))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, prepared.Entries[i].OriginalPath,
                TEXT("The recoverable Content entry set changed during transaction preparation."));
    }

    BatchRollbackPlan plan;
    plan.TransactionId = transactionId;
    plan.Kind = BatchRollbackKind::Trash;
    plan.Trash = prepared;
    plan.TrashRoot = trashRoot;
    const String transactionDirectory = _stagingRoot / transactionId.ToString(Guid::FormatType::N);
    if (PrepareBatchScratch(transactionDirectory, diagnostic))
        return true;
    for (int32 i = 0; i < plan.Trash.Entries.Count(); i++)
    {
        const AssetTrashEntry& entry = plan.Trash.Entries[i];
        if (result)
        {
            result->FailureIndex = i;
            result->FailurePath = entry.OriginalPath;
        }
        if (isCancelled())
        {
            const bool rollbackFailed = RollbackBatch(plan);
            if (!rollbackFailed)
            {
                CleanupEmptyTrashRoot(plan);
                DeleteTree(transactionDirectory);
            }
            if (result)
            {
                result->Cancelled = true;
                result->RolledBackEntries = rollbackFailed ? 0 : result->CompletedEntries;
            }
            Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, entry.OriginalPath,
                TEXT("Native trash commit was cancelled."));
            if (rollbackFailed)
                diagnostic.Related.Add(transactionDirectory);
            return true;
        }
        if (MoveEntry(entry, true))
        {
            const bool rollbackFailed = RollbackBatch(plan);
            if (result && !rollbackFailed)
                result->RolledBackEntries = result->CompletedEntries;
            if (!rollbackFailed)
            {
                CleanupEmptyTrashRoot(plan);
                DeleteTree(transactionDirectory);
            }
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                Fail(diagnostic, rollbackFailed ? AssetPipelineDiagnosticCode::MigrationFailed :
                    AssetPipelineDiagnosticCode::LibraryCreationFailed, entry.OriginalPath,
                    rollbackFailed ? TEXT("Content trash rollback failed; temporary operation data remains.") :
                        TEXT("Content trash staging failed and was rolled back."));
            if (rollbackFailed)
                diagnostic.Related.Add(transactionDirectory);
            return true;
        }
        if (result)
            result->CompletedEntries = i + 1;
    }
    {
        const bool rollbackFailed = RollbackBatch(plan);
        if (!rollbackFailed)
        {
            CleanupEmptyTrashRoot(plan);
            DeleteTree(transactionDirectory);
        }
        else
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }

    Array<AssetOperationCommit> commits;
    for (const AssetTrashEntry& entry : plan.Trash.Entries)
    {
        AssetOperationCommit commit;
        commit.TransactionId = transactionId;
        commit.Kind = AssetOperationKind::Trash;
        commit.AssetGuid = entry.AssetGuid;
        commit.SourcePath = entry.OriginalPath;
        commit.DestinationPath = entry.TrashPath;
        commits.Add(MoveTemp(commit));
    }
    if (_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        const AssetPipelineDiagnostic publicationDiagnostic = diagnostic;
        const bool rollbackFailed = RollbackBatch(plan);
        if (result && !rollbackFailed)
            result->RolledBackEntries = result->CompletedEntries;
        if (!rollbackFailed)
        {
            Array<AssetOperationCommit> rollbackCommits;
            for (const AssetTrashEntry& entry : plan.Trash.Entries)
            {
                AssetOperationCommit rollbackCommit;
                rollbackCommit.TransactionId = transactionId;
                rollbackCommit.Kind = AssetOperationKind::Restore;
                rollbackCommit.AssetGuid = entry.AssetGuid;
                rollbackCommit.SourcePath = entry.TrashPath;
                rollbackCommit.DestinationPath = entry.OriginalPath;
                rollbackCommits.Add(MoveTemp(rollbackCommit));
            }
            AssetPipelineDiagnostic ignored;
            _databaseCallbacks.RefreshCommitted(rollbackCommits, ignored);
            CleanupEmptyTrashRoot(plan);
            DeleteTree(transactionDirectory);
        }
        diagnostic = publicationDiagnostic;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    {
        const AssetPipelineDiagnostic commitDiagnostic = diagnostic;
        bool rollbackFailed = RollbackBatch(plan);
        if (result && !rollbackFailed)
            result->RolledBackEntries = result->CompletedEntries;
        if (!rollbackFailed)
        {
            Array<AssetOperationCommit> rollbackCommits;
            for (const AssetTrashEntry& entry : plan.Trash.Entries)
            {
                AssetOperationCommit rollbackCommit;
                rollbackCommit.TransactionId = transactionId;
                rollbackCommit.Kind = AssetOperationKind::Restore;
                rollbackCommit.AssetGuid = entry.AssetGuid;
                rollbackCommit.SourcePath = entry.TrashPath;
                rollbackCommit.DestinationPath = entry.OriginalPath;
                rollbackCommits.Add(MoveTemp(rollbackCommit));
            }
            AssetPipelineDiagnostic rollbackDiagnostic;
            rollbackFailed = _databaseCallbacks.RefreshCommitted(rollbackCommits, rollbackDiagnostic);
        }
        if (!rollbackFailed)
        {
            CleanupEmptyTrashRoot(plan);
            DeleteTree(transactionDirectory);
        }
        diagnostic = commitDiagnostic;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    // A committed plan makes interrupted cleanup recoverable, so cleanup failure is not an operation failure.
    DeleteTree(transactionDirectory);
    trash = MoveTemp(prepared);
    if (result)
    {
        result->FailureIndex = -1;
        result->FailurePath.Clear();
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::RestoreEntries(const AssetTrashBatch& trash, AssetPipelineDiagnostic& diagnostic)
{
    if (!trash.TransactionId.IsValid() || trash.Entries.IsEmpty() || trash.Entries.Count() > MaximumTrashEntries)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, StringView::Empty,
            TEXT("The recoverable Content trash record is invalid."));
    const String trashRoot = _trashRoot / trash.TransactionId.ToString(Guid::FormatType::N);
    const String fragmentsRoot = SceneFragmentStore::GetRootPath(_projectRoot);
    const auto validate = [this, &trash, &trashRoot, &fragmentsRoot](AssetPipelineDiagnostic& validateDiagnostic)
    {
        if (!FileSystem::DirectoryExists(trashRoot))
            return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::SourceMissing, trashRoot,
                TEXT("The native Content trash directory is missing."));
        for (int32 i = 0; i < trash.Entries.Count(); i++)
        {
            const AssetTrashEntry& entry = trash.Entries[i];
            AssetPathPolicy::ProjectPath original;
            if (NormalizeSource(entry.OriginalPath, original, validateDiagnostic))
                return true;
            if (!FileSystem::AreFilePathsEquivalent(original.AbsolutePath, entry.OriginalPath) ||
                FileSystem::AreFilePathsEquivalent(original.AbsolutePath, _contentRoot) ||
                !AssetPathPolicy::IsSameOrChild(entry.TrashPath, trashRoot) ||
                FileSystem::AreFilePathsEquivalent(entry.TrashPath, trashRoot) ||
                PathExists(entry.OriginalPath) ||
                (entry.IsFolder ? !FileSystem::DirectoryExists(entry.TrashPath) : !FileSystem::FileExists(entry.TrashPath)) ||
                ContainsFileSystemLink(entry.TrashPath, entry.IsFolder))
                return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, entry.OriginalPath,
                    TEXT("The exact Content trash source or restore destination changed."));
            for (int32 j = 0; j < i; j++)
            {
                if (AssetPathPolicy::IsSameOrChild(entry.OriginalPath, trash.Entries[j].OriginalPath) ||
                    AssetPathPolicy::IsSameOrChild(trash.Entries[j].OriginalPath, entry.OriginalPath))
                    return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PathCollision, entry.OriginalPath,
                        TEXT("Content trash records must contain distinct top-level paths."));
            }
            if (entry.OriginalMetaPath.HasChars())
            {
                if (!FileSystem::AreFilePathsEquivalent(entry.OriginalMetaPath, MetaPath(entry.OriginalPath)) ||
                    !FileSystem::AreFilePathsEquivalent(entry.TrashMetaPath, entry.TrashPath + TEXT(".meta")) ||
                    PathExists(entry.OriginalMetaPath) || !FileSystem::FileExists(entry.TrashMetaPath) ||
                    ContainsFileSystemLink(entry.TrashMetaPath, false))
                    return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, entry.OriginalMetaPath,
                        TEXT("The exact Content metadata trash record changed."));
                AssetMeta meta;
                if (AssetMeta::Load(entry.TrashMetaPath, meta, validateDiagnostic))
                    return true;
                if (meta.ID != entry.AssetGuid || meta.FolderAsset != entry.IsFolder)
                    return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, entry.TrashMetaPath,
                        TEXT("Content trash metadata no longer matches its captured identity."));
            }
            else if (entry.TrashMetaPath.HasChars() || entry.AssetGuid.IsValid())
            {
                return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, entry.TrashPath,
                    TEXT("The Content trash record has an incomplete metadata identity."));
            }
            for (const AssetTrashFragment& fragment : entry.Fragments)
            {
                const String& originalFragments = fragment.OriginalPath;
                const String& trashFragments = fragment.TrashPath;
                if (!AssetPathPolicy::IsSameOrChild(originalFragments, fragmentsRoot) ||
                    FileSystem::AreFilePathsEquivalent(originalFragments, fragmentsRoot) ||
                    !AssetPathPolicy::IsSameOrChild(trashFragments, trashRoot) ||
                    PathExists(originalFragments) || !FileSystem::DirectoryExists(trashFragments) ||
                    ContainsFileSystemLink(trashFragments, true))
                    return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, originalFragments,
                        TEXT("The exact private scene fragment trash record changed."));
            }
        }
        return false;
    };
    if (validate(diagnostic))
        return true;
    for (const AssetTrashEntry& entry : trash.Entries)
    {
        AssetOperationTarget target;
        target.SourcePath = entry.OriginalPath;
        target.ExpectedGuid = entry.AssetGuid;
        if (_modificationProcessor.ValidateOperation(AssetOperationKind::Restore, target,
            entry.OriginalPath, diagnostic))
            return true;
    }
    if (BeginImmediateTransaction(trash.Entries[0].OriginalPath, diagnostic))
        return true;
    SCOPE_EXIT { EndImmediateTransaction(); };
    Array<String> lockPaths;
    lockPaths.Add(trashRoot);
    for (const AssetTrashEntry& entry : trash.Entries)
    {
        lockPaths.Add(entry.OriginalPath);
        lockPaths.Add(entry.TrashPath);
        if (entry.OriginalMetaPath.HasChars())
        {
            lockPaths.Add(entry.OriginalMetaPath);
            lockPaths.Add(entry.TrashMetaPath);
        }
        for (const AssetTrashFragment& fragment : entry.Fragments)
        {
            lockPaths.Add(fragment.OriginalPath);
            lockPaths.Add(fragment.TrashPath);
        }
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    if (validate(diagnostic))
        return true;

    BatchRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = BatchRollbackKind::Restore;
    plan.Trash = trash;
    plan.TrashRoot = trashRoot;
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    if (PrepareBatchScratch(transactionDirectory, diagnostic))
        return true;
    for (const AssetTrashEntry& entry : plan.Trash.Entries)
    {
        if (MoveEntry(entry, false))
        {
            const bool rollbackFailed = RollbackBatch(plan);
            if (!rollbackFailed)
                DeleteTree(transactionDirectory);
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                Fail(diagnostic, rollbackFailed ? AssetPipelineDiagnosticCode::MigrationFailed :
                    AssetPipelineDiagnosticCode::LibraryCreationFailed, entry.OriginalPath,
                    rollbackFailed ? TEXT("Content restore rollback failed; temporary operation data remains.") :
                        TEXT("Content restore failed and was rolled back."));
            if (rollbackFailed)
                diagnostic.Related.Add(transactionDirectory);
            return true;
        }
    }
    {
        if (!RollbackBatch(plan))
            DeleteTree(transactionDirectory);
        else
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    Array<AssetOperationCommit> commits;
    for (const AssetTrashEntry& entry : plan.Trash.Entries)
    {
        AssetOperationCommit commit;
        commit.TransactionId = plan.TransactionId;
        commit.Kind = AssetOperationKind::Restore;
        commit.AssetGuid = entry.AssetGuid;
        commit.SourcePath = entry.TrashPath;
        commit.DestinationPath = entry.OriginalPath;
        if (!entry.IsFolder)
            AddSelfWrite(commit, entry.OriginalPath);
        if (entry.OriginalMetaPath.HasChars())
            AddSelfWrite(commit, entry.OriginalMetaPath);
        commits.Add(MoveTemp(commit));
    }
    if (_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        const AssetPipelineDiagnostic publicationDiagnostic = diagnostic;
        const bool rollbackFailed = RollbackBatch(plan);
        if (!rollbackFailed)
        {
            Array<AssetOperationCommit> rollbackCommits;
            for (const AssetTrashEntry& entry : plan.Trash.Entries)
            {
                AssetOperationCommit rollbackCommit;
                rollbackCommit.TransactionId = plan.TransactionId;
                rollbackCommit.Kind = AssetOperationKind::Trash;
                rollbackCommit.AssetGuid = entry.AssetGuid;
                rollbackCommit.SourcePath = entry.OriginalPath;
                rollbackCommit.DestinationPath = entry.TrashPath;
                rollbackCommits.Add(MoveTemp(rollbackCommit));
            }
            AssetPipelineDiagnostic ignored;
            _databaseCallbacks.RefreshCommitted(rollbackCommits, ignored);
            DeleteTree(transactionDirectory);
        }
        diagnostic = publicationDiagnostic;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    {
        const AssetPipelineDiagnostic commitDiagnostic = diagnostic;
        bool rollbackFailed = RollbackBatch(plan);
        if (!rollbackFailed)
        {
            Array<AssetOperationCommit> rollbackCommits;
            for (const AssetTrashEntry& entry : plan.Trash.Entries)
            {
                AssetOperationCommit rollbackCommit;
                rollbackCommit.TransactionId = plan.TransactionId;
                rollbackCommit.Kind = AssetOperationKind::Trash;
                rollbackCommit.AssetGuid = entry.AssetGuid;
                rollbackCommit.SourcePath = entry.OriginalPath;
                rollbackCommit.DestinationPath = entry.TrashPath;
                rollbackCommits.Add(MoveTemp(rollbackCommit));
            }
            AssetPipelineDiagnostic rollbackDiagnostic;
            rollbackFailed = _databaseCallbacks.RefreshCommitted(rollbackCommits, rollbackDiagnostic);
        }
        if (!rollbackFailed)
            DeleteTree(transactionDirectory);
        diagnostic = commitDiagnostic;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    // A committed plan makes interrupted cleanup recoverable, so cleanup failure is not an operation failure.
    DeleteTree(transactionDirectory);
    CleanupEmptyTrashRoot(plan);
    _stateLocker.Lock();
    for (const AssetOperationCommit& commit : commits)
        _selfWrites.Add(commit.SelfWrites);
    _stateLocker.Unlock();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::DiscardTrash(const AssetTrashBatch& trash, AssetPipelineDiagnostic& diagnostic)
{
    if (!trash.TransactionId.IsValid() || trash.Entries.IsEmpty() || trash.Entries.Count() > MaximumTrashEntries)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, StringView::Empty,
            TEXT("The Content trash record cannot be discarded because it is invalid."));
    const String trashRoot = _trashRoot / trash.TransactionId.ToString(Guid::FormatType::N);
    const String fragmentsRoot = SceneFragmentStore::GetRootPath(_projectRoot);
    const auto validate = [this, &trash, &trashRoot, &fragmentsRoot](AssetPipelineDiagnostic& validateDiagnostic)
    {
        if (!FileSystem::DirectoryExists(trashRoot) || ContainsFileSystemLink(trashRoot, true))
            return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::SourceMissing, trashRoot,
                TEXT("The native Content trash directory is missing or unsafe."));
        for (const AssetTrashEntry& entry : trash.Entries)
        {
            AssetPathPolicy::ProjectPath original;
            if (NormalizeSource(entry.OriginalPath, original, validateDiagnostic))
                return true;
            if (!FileSystem::AreFilePathsEquivalent(original.AbsolutePath, entry.OriginalPath) ||
                FileSystem::AreFilePathsEquivalent(original.AbsolutePath, _contentRoot) ||
                !AssetPathPolicy::IsSameOrChild(entry.TrashPath, trashRoot) ||
                FileSystem::AreFilePathsEquivalent(entry.TrashPath, trashRoot) || PathExists(entry.OriginalPath) ||
                (entry.IsFolder ? !FileSystem::DirectoryExists(entry.TrashPath) : !FileSystem::FileExists(entry.TrashPath)))
                return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, entry.TrashPath,
                    TEXT("Content trash cannot be discarded after its original or recovery paths changed."));
            if (entry.OriginalMetaPath.HasChars())
            {
                if (!FileSystem::AreFilePathsEquivalent(entry.OriginalMetaPath, MetaPath(entry.OriginalPath)) ||
                    !FileSystem::AreFilePathsEquivalent(entry.TrashMetaPath, entry.TrashPath + TEXT(".meta")) ||
                    PathExists(entry.OriginalMetaPath) || !FileSystem::FileExists(entry.TrashMetaPath))
                    return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, entry.TrashMetaPath,
                        TEXT("Content metadata trash cannot be discarded after its recovery paths changed."));
            }
            else if (entry.TrashMetaPath.HasChars() || entry.AssetGuid.IsValid())
            {
                return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, entry.TrashPath,
                    TEXT("The Content trash record has an incomplete metadata identity."));
            }
            for (const AssetTrashFragment& fragment : entry.Fragments)
            {
                if (!AssetPathPolicy::IsSameOrChild(fragment.OriginalPath, fragmentsRoot) ||
                    FileSystem::AreFilePathsEquivalent(fragment.OriginalPath, fragmentsRoot) ||
                    !AssetPathPolicy::IsSameOrChild(fragment.TrashPath, trashRoot) ||
                    FileSystem::AreFilePathsEquivalent(fragment.TrashPath, trashRoot) ||
                    PathExists(fragment.OriginalPath) || !FileSystem::DirectoryExists(fragment.TrashPath))
                    return Fail(validateDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, fragment.TrashPath,
                        TEXT("Private scene fragment trash cannot be discarded after its recovery paths changed."));
            }
        }
        return false;
    };
    if (validate(diagnostic))
        return true;
    Array<String> lockPaths;
    lockPaths.Add(trashRoot);
    for (const AssetTrashEntry& entry : trash.Entries)
    {
        lockPaths.Add(entry.OriginalPath);
        lockPaths.Add(entry.TrashPath);
        if (entry.OriginalMetaPath.HasChars())
        {
            lockPaths.Add(entry.OriginalMetaPath);
            lockPaths.Add(entry.TrashMetaPath);
        }
        for (const AssetTrashFragment& fragment : entry.Fragments)
        {
            lockPaths.Add(fragment.OriginalPath);
            lockPaths.Add(fragment.TrashPath);
        }
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    if (validate(diagnostic))
        return true;

    BatchRollbackPlan plan;
    plan.TransactionId = Guid::New();
    plan.Kind = BatchRollbackKind::Discard;
    plan.Trash = trash;
    plan.TrashRoot = trashRoot;
    const String transactionDirectory = _stagingRoot / plan.TransactionId.ToString(Guid::FormatType::N);
    plan.DiscardStageRoot = transactionDirectory / TEXT("discard.stage");
    if (PrepareBatchScratch(transactionDirectory, diagnostic))
        return true;
    if (MovePath(plan.DiscardStageRoot, trashRoot, false))
    {
        const bool rollbackFailed = RollbackBatch(plan);
        if (!rollbackFailed)
            DeleteTree(transactionDirectory);
        Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, trashRoot,
            TEXT("Content trash discard could not enter its recoverable staging boundary."));
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    {
        const AssetPipelineDiagnostic appliedDiagnostic = diagnostic;
        const bool rollbackFailed = RollbackBatch(plan);
        if (!rollbackFailed)
            DeleteTree(transactionDirectory);
        diagnostic = appliedDiagnostic;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    {
        const AssetPipelineDiagnostic commitDiagnostic = diagnostic;
        const bool rollbackFailed = RollbackBatch(plan);
        if (!rollbackFailed)
            DeleteTree(transactionDirectory);
        diagnostic = commitDiagnostic;
        if (rollbackFailed)
            diagnostic.Related.Add(transactionDirectory);
        return true;
    }
    // A committed discard owns the staged directory. Startup recovery retries cleanup if this delete is interrupted.
    DeleteTree(transactionDirectory);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::IsAssetEditing() const
{
    _stateLocker.Lock();
    const bool result = _editingDepth > 0;
    _stateLocker.Unlock();
    return result;
}

void AssetOperations::StartAssetEditing()
{
    _stateLocker.Lock();
    while (_immediateTransactions > 0)
        _locksChanged.Wait(_stateLocker);
    _editingDepth++;
    _stateLocker.Unlock();
}

bool AssetOperations::StopAssetEditing(AssetPipelineDiagnostic& diagnostic)
{
    Array<AssetOperationCommit> commits;
    _stateLocker.Lock();
    if (_editingDepth == 0)
    {
        _stateLocker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, StringView::Empty,
            TEXT("StopAssetEditing has no matching StartAssetEditing scope."));
    }
    _editingDepth--;
    if (_editingDepth != 0)
    {
        _stateLocker.Unlock();
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    commits = MoveTemp(_pendingCommits);
    _pendingCommits.Clear();
    _stateLocker.Unlock();
    if (commits.IsEmpty())
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    if (!_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        _stateLocker.Lock();
        for (const AssetOperationCommit& published : commits)
            _selfWrites.Add(published.SelfWrites);
        _stateLocker.Unlock();
        return false;
    }

    // Publishing is the boundary that makes a committed filesystem batch visible. Keep a failed
    // batch pending so a later editing scope or explicit retry cannot silently lose it.
    _stateLocker.Lock();
    Array<AssetOperationCommit> pending = MoveTemp(_pendingCommits);
    _pendingCommits = MoveTemp(commits);
    _pendingCommits.Add(pending);
    _stateLocker.Unlock();
    return true;
}

void AssetOperations::RegisterSelfWrite(const StringView& path, const ContentHash& content)
{
    AssetOperationSelfWrite write;
    write.TransactionId = Guid::New();
    write.Path = path;
    write.Content = content;
    _stateLocker.Lock();
    _selfWrites.Add(MoveTemp(write));
    _stateLocker.Unlock();
}

void AssetOperations::DrainSelfWrites(Array<AssetOperationSelfWrite>& result)
{
    _stateLocker.Lock();
    result = MoveTemp(_selfWrites);
    _selfWrites.Clear();
    _stateLocker.Unlock();
}
