// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseScanner.h"
#include "AssetMeta.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/Crc.h"

namespace
{
    bool IsMeta(const StringView& path)
    {
        return path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
    }

    bool ReadLegacyEntries(const StringView& path, Array<FlaxStorage::Entry>& entries)
    {
        bool failed = false;
        {
            const FlaxStorageReference storage = ContentStorageManager::GetStorage(path, true);
            if (!storage || storage->GetEntriesCount() < 1)
            {
                failed = true;
            }
            else
            {
                entries.Resize(storage->GetEntriesCount());
                for (int32 i = 0; i < entries.Count(); i++)
                    storage->GetEntry(i, entries[i]);
            }
        }
        ContentStorageManager::EnsureAccess(path);
        return failed;
    }

    bool RequiresMetadata(const StringView& path)
    {
        const String extension = FileSystem::GetExtension(path).ToLower();
        const Char* supported[] =
        {
            TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"), TEXT("bmp"), TEXT("gif"), TEXT("tiff"), TEXT("tif"),
            TEXT("dds"), TEXT("hdr"), TEXT("raw"), TEXT("exr"),
            TEXT("obj"), TEXT("fbx"), TEXT("x"), TEXT("dae"), TEXT("gltf"), TEXT("glb"), TEXT("blend"),
            TEXT("bvh"), TEXT("ase"), TEXT("ply"), TEXT("dxf"), TEXT("ifc"), TEXT("nff"), TEXT("smd"),
            TEXT("vta"), TEXT("mdl"), TEXT("md2"), TEXT("md3"), TEXT("md5mesh"), TEXT("q3o"), TEXT("q3s"),
            TEXT("ac"), TEXT("stl"), TEXT("lwo"), TEXT("lws"), TEXT("lxo"),
            TEXT("wav"), TEXT("mp3"), TEXT("ogg"),
            TEXT("ttf"), TEXT("otf"),
            TEXT("mp4"), TEXT("webm"), TEXT("mov"), TEXT("mkv"), TEXT("txt"),
            TEXT("shader"),
            TEXT("materialfunction"), TEXT("animgraphfunction"), TEXT("animgraph"),
            TEXT("visualscript"), TEXT("behaviortree"), TEXT("particlefunction"), TEXT("material"),
            TEXT("particleemitter"), TEXT("particlesystem"), TEXT("collisiondata"),
            TEXT("materialinstance"), TEXT("sceneanimation"), TEXT("skeletonmask"),
            TEXT("scene"), TEXT("prefab")
        };
        for (const Char* value : supported)
        {
            if (extension == value)
                return true;
        }
        return false;
    }

    bool IsExcluded(const StringView& path, const StringView& contentRoot, const StringView& libraryRoot)
    {
        if (AssetPathPolicy::IsSameOrChild(path, libraryRoot))
            return true;
        String normalized(path);
        FileSystem::NormalizePath(normalized);
        normalized = normalized.ToLower();
        String root(contentRoot);
        FileSystem::NormalizePath(root);
        root = root.ToLower();
        String relative = normalized;
        if (relative.StartsWith(root))
            relative = relative.Substring(root.Length());
        relative = TEXT("/") + relative + TEXT("/");
        const Char* excluded[] =
        {
            TEXT("/library/"), TEXT("/cache/"), TEXT("/output/"), TEXT("/generated/"),
            TEXT("/migrationbackup/"), TEXT("/.asset-pipeline/"), TEXT("/.git/")
        };
        for (const Char* segment : excluded)
        {
            if (relative.Contains(segment))
                return true;
        }
        return false;
    }

    AssetPipelineDiagnostic MakeDiagnostic(AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        AssetPipelineDiagnostic result;
        result.Code = code;
        result.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        result.SourcePath = path;
        result.Message = message;
        return result;
    }

    AssetRecord MakeMainRecord(const AssetMeta& meta, const StringView& sourcePath, const StringView& metaPath, const AssetPathPolicy::ProjectPath& normalizedPath, uint64 semanticHash, AssetRecordStatus status)
    {
        AssetRecord result;
        result.ID = meta.ID;
        result.SourceAssetID = meta.ID;
        result.TypeName = meta.AssetType;
        result.CanonicalPath = CanonicalAssetPath(sourcePath);
        result.SourcePath = SourceFilePath(sourcePath);
        result.MetaPath = MetaFilePath(metaPath);
        result.ProcessorID = meta.Processor.ID;
        result.PortabilityKey = normalizedPath.PortabilityKey;
        result.MetaSemanticHash = semanticHash;
        result.SourceKind = meta.SourceKind;
        result.Status = status;
        return result;
    }

    void AddMetaRecords(const AssetMeta& meta, const StringView& sourcePath, const StringView& metaPath, const AssetPathPolicy::ProjectPath& normalizedPath, uint64 semanticHash, AssetRecordStatus status, Array<AssetRecord>& records)
    {
        records.Add(MakeMainRecord(meta, sourcePath, metaPath, normalizedPath, semanticHash, status));
        for (const auto& entry : meta.SubAssets)
        {
            AssetRecord subAsset;
            subAsset.ID = entry.Value.ID;
            subAsset.SourceAssetID = meta.ID;
            subAsset.TypeName = entry.Value.TypeName;
            subAsset.CanonicalPath = CanonicalAssetPath(sourcePath);
            subAsset.SourcePath = SourceFilePath(sourcePath);
            subAsset.MetaPath = MetaFilePath(metaPath);
            subAsset.SubAsset = SubAssetKey(entry.Key);
            subAsset.ProcessorID = meta.Processor.ID;
            subAsset.PortabilityKey = normalizedPath.PortabilityKey;
            subAsset.MetaSemanticHash = semanticHash;
            subAsset.SourceKind = meta.SourceKind;
            subAsset.Status = entry.Value.Removed ? AssetRecordStatus::MissingSource : status;
            records.Add(MoveTemp(subAsset));
        }
    }

    void AddRecordWithDuplicateCheck(AssetRecord&& record, Array<AssetRecord>& records, Dictionary<Guid, int32>& recordIndices, Array<AssetPipelineDiagnostic>& diagnostics)
    {
        const int32* existingIndex = recordIndices.TryGet(record.ID);
        if (existingIndex)
        {
            AssetRecord& existing = records[*existingIndex];
            existing.Status = AssetRecordStatus::DuplicateGuid;
            AssetPipelineDiagnostic diagnostic = MakeDiagnostic(AssetPipelineDiagnosticCode::DuplicateGuid, record.SourcePath.Get(), TEXT("The GUID is declared by more than one source or subasset."));
            diagnostic.AssetGuid = record.ID;
            diagnostic.Related.Add(existing.SourcePath.Get());
            diagnostic.Related.Add(record.SourcePath.Get());
            diagnostics.Add(MoveTemp(diagnostic));
            return;
        }
        recordIndices.Add(record.ID, records.Count());
        records.Add(MoveTemp(record));
    }

    void RetainPreviousMalformedRecords(const AssetDatabaseSnapshot& previous, const StringView& metaPath, Array<AssetRecord>& records, Dictionary<Guid, int32>& recordIndices)
    {
        for (const AssetRecord& old : previous.Records)
        {
            if (old.MetaPath.Get() != metaPath)
                continue;
            AssetRecord retained = old;
            retained.Status = AssetRecordStatus::MalformedMeta;
            if (!recordIndices.ContainsKey(retained.ID))
            {
                recordIndices.Add(retained.ID, records.Count());
                records.Add(MoveTemp(retained));
            }
        }
    }
}

bool AssetDatabaseScanResult::HasBlockingDiagnostics() const
{
    for (const AssetPipelineDiagnostic& diagnostic : Diagnostics)
    {
        if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Error)
            return true;
    }
    return false;
}

bool AssetDatabaseScanner::Scan(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const AssetDatabaseScanOptions& options, AssetDatabase& database, AssetDatabaseScanResult& result)
{
    Array<AssetRecord> records;
    if (Collect(projectRoot, contentRoot, libraryRoot, options, database.GetSnapshot(), records, result))
        return true;
    AssetPipelineDiagnostic publishDiagnostic;
    if (database.PublishFullSnapshot(records, publishDiagnostic))
    {
        result.Diagnostics.Add(publishDiagnostic);
        return true;
    }
    result.Revision = database.GetRevision();
    return false;
}

bool AssetDatabaseScanner::Collect(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const AssetDatabaseScanOptions& options, const AssetDatabaseSnapshot& previous, Array<AssetRecord>& records, AssetDatabaseScanResult& result)
{
    result = AssetDatabaseScanResult();
    records.Clear();
    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, String(contentRoot), TEXT("*"), DirectorySearchOption::AllDirectories))
    {
        result.Diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::SourceMissing, contentRoot, TEXT("Cannot enumerate the project Content root.")));
        return true;
    }
    if (files.Count() > options.MaximumFiles)
    {
        result.Diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::SourceBusy, contentRoot, TEXT("Content scan exceeds the configured bounded file count.")));
        return true;
    }
    return CollectFromFiles(projectRoot, contentRoot, libraryRoot, files, options, previous, records, result);
}

bool AssetDatabaseScanner::CollectFromFiles(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const Array<String>& files, const AssetDatabaseScanOptions& options, const AssetDatabaseSnapshot& previous, Array<AssetRecord>& records, AssetDatabaseScanResult& result)
{
    result = AssetDatabaseScanResult();
    records.Clear();
    SourceHashCache localHashCache;
    SourceHashCache& hashCache = options.HashCache ? *options.HashCache : localHashCache;
    HashSet<String> fileSet;
    for (const String& file : files)
    {
        if (!IsExcluded(file, contentRoot, libraryRoot))
        {
            fileSet.Add(file);
            AssetDatabaseFileState state;
            ContentHash hash;
            AssetPipelineDiagnostic hashDiagnostic;
            if (hashCache.HashFile(file, hash, state, hashDiagnostic))
                result.Diagnostics.Add(MoveTemp(hashDiagnostic));
            else
                result.FileStates.Add(MoveTemp(state));
        }
    }

    Dictionary<Guid, int32> recordIndices;
    Dictionary<String, int32> mainPathIndices;
    HashSet<String> consumedMeta;
    for (const String& sourcePath : files)
    {
        if ((options.Cancel && *options.Cancel) || IsExcluded(sourcePath, contentRoot, libraryRoot) || IsMeta(sourcePath))
        {
            if (options.Cancel && *options.Cancel)
            {
                result.Cancelled = true;
                return false;
            }
            continue;
        }
        result.FilesExamined++;
        if (FileSystem::GetExtension(sourcePath).ToLower() == TEXT("flax"))
        {
            AssetPipelineDiagnostic diagnostic;
            AssetPathPolicy::ProjectPath normalizedPath;
            if (AssetPathPolicy::TryNormalizeProjectPath(projectRoot, contentRoot, libraryRoot, sourcePath, normalizedPath, diagnostic))
            {
                result.Diagnostics.Add(diagnostic);
                continue;
            }
            Array<FlaxStorage::Entry> entries;
            if (ReadLegacyEntries(sourcePath, entries))
            {
                result.Diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::InvalidMeta, sourcePath, TEXT("Legacy binary asset header is unreadable.")));
                continue;
            }
            const Guid rootID = entries[0].ID;
            for (int32 i = 0; i < entries.Count(); i++)
            {
                AssetRecord record;
                record.ID = entries[i].ID;
                record.SourceAssetID = rootID;
                record.TypeName = entries[i].TypeName;
                record.CanonicalPath = CanonicalAssetPath(sourcePath);
                record.SourcePath = SourceFilePath(sourcePath);
                if (i != 0)
                    record.SubAsset = SubAssetKey(String::Format(TEXT("legacy:{0}"), i));
                record.PortabilityKey = normalizedPath.PortabilityKey;
                record.SourceKind = AssetSourceKind::LegacyBinary;
                record.Status = AssetRecordStatus::Ready;
                AddRecordWithDuplicateCheck(MoveTemp(record), records, recordIndices, result.Diagnostics);
            }
            continue;
        }
        const String metaPath = sourcePath + TEXT(".meta");
        if (!fileSet.Contains(metaPath))
        {
            if (options.StrictMetadata && RequiresMetadata(sourcePath))
                result.Diagnostics.Add(MakeDiagnostic(AssetPipelineDiagnosticCode::MissingMeta, sourcePath, TEXT("Canonical source is missing its adjacent metadata sidecar.")));
            continue;
        }
        consumedMeta.Add(metaPath);
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(metaPath, meta, diagnostic))
        {
            result.Diagnostics.Add(diagnostic);
            RetainPreviousMalformedRecords(previous, metaPath, records, recordIndices);
            continue;
        }
        result.SidecarsParsed++;
        AssetPathPolicy::ProjectPath normalizedPath;
        if (AssetPathPolicy::TryNormalizeProjectPath(projectRoot, contentRoot, libraryRoot, sourcePath, normalizedPath, diagnostic))
        {
            result.Diagnostics.Add(diagnostic);
            continue;
        }
        StringAnsi canonicalMeta;
        if (meta.ToJson(canonicalMeta, diagnostic))
        {
            result.Diagnostics.Add(diagnostic);
            continue;
        }
        const uint64 semanticHash = Crc::MemCrc32(canonicalMeta.Get(), canonicalMeta.Length());
        const AssetRecordStatus status = meta.MetaUpgradeRequired ? AssetRecordStatus::MetaUpgradeRequired : AssetRecordStatus::Ready;
        Array<AssetRecord> metaRecords;
        AddMetaRecords(meta, sourcePath, metaPath, normalizedPath, semanticHash, status, metaRecords);
        for (AssetRecord& record : metaRecords)
            AddRecordWithDuplicateCheck(MoveTemp(record), records, recordIndices, result.Diagnostics);

        const int32* currentMainPtr = recordIndices.TryGet(meta.ID);
        const int32 currentMain = currentMainPtr ? *currentMainPtr : -1;
        if (currentMain < 0 || currentMain >= records.Count() || records[currentMain].SourcePath.Get() != sourcePath)
            continue;
        const int32* existingMain = mainPathIndices.TryGet(normalizedPath.PortabilityKey);
        if (existingMain)
        {
            records[*existingMain].Status = AssetRecordStatus::PathCollision;
            records[currentMain].Status = AssetRecordStatus::PathCollision;
            AssetPipelineDiagnostic collision = MakeDiagnostic(AssetPipelineDiagnosticCode::PathCollision, sourcePath, TEXT("Two canonical source paths collide under the project portability policy."));
            collision.Related.Add(records[*existingMain].SourcePath.Get());
            collision.Related.Add(sourcePath);
            result.Diagnostics.Add(MoveTemp(collision));
        }
        else
        {
            mainPathIndices.Add(normalizedPath.PortabilityKey, currentMain);
        }
    }

    for (const String& metaPath : files)
    {
        if (IsExcluded(metaPath, contentRoot, libraryRoot) || !IsMeta(metaPath) || consumedMeta.Contains(metaPath))
            continue;
        const String sourcePath = metaPath.Substring(0, metaPath.Length() - 5);
        AssetPipelineDiagnostic orphan = MakeDiagnostic(AssetPipelineDiagnosticCode::SourceMissing, sourcePath, TEXT("Metadata sidecar has no adjacent source file."));
        orphan.Related.Add(metaPath);
        result.Diagnostics.Add(orphan);

        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(metaPath, meta, diagnostic))
        {
            result.Diagnostics.Add(diagnostic);
            continue;
        }
        AssetPathPolicy::ProjectPath normalizedPath;
        if (AssetPathPolicy::TryNormalizeProjectPath(projectRoot, contentRoot, libraryRoot, sourcePath, normalizedPath, diagnostic))
        {
            result.Diagnostics.Add(diagnostic);
            continue;
        }
        StringAnsi canonicalMeta;
        if (meta.ToJson(canonicalMeta, diagnostic))
        {
            result.Diagnostics.Add(diagnostic);
            continue;
        }
        Array<AssetRecord> metaRecords;
        AddMetaRecords(meta, sourcePath, metaPath, normalizedPath, Crc::MemCrc32(canonicalMeta.Get(), canonicalMeta.Length()), AssetRecordStatus::OrphanMeta, metaRecords);
        for (AssetRecord& record : metaRecords)
            AddRecordWithDuplicateCheck(MoveTemp(record), records, recordIndices, result.Diagnostics);
    }

    return false;
}
