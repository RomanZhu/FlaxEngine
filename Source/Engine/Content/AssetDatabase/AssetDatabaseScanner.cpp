// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseScanner.h"
#include "AssetMeta.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/Crc.h"

namespace
{
    bool IsMeta(const StringView& path)
    {
        return path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
    }

    bool RequiresMetadata(const StringView& path)
    {
        const String extension = FileSystem::GetExtension(path).ToLower();
        const Char* supported[] =
        {
            TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"), TEXT("bmp"), TEXT("hdr"), TEXT("exr"),
            TEXT("fbx"), TEXT("obj"), TEXT("gltf"), TEXT("glb"),
            TEXT("wav"), TEXT("mp3"), TEXT("ogg"), TEXT("flac"),
            TEXT("ttf"), TEXT("otf"),
            TEXT("materialfunction"), TEXT("animgraphfunction"), TEXT("animgraph"),
            TEXT("visualscript"), TEXT("behaviortree"), TEXT("particlefunction"), TEXT("material")
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
    result = AssetDatabaseScanResult();
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

    const AssetDatabaseSnapshot previous = database.GetSnapshot();
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

    Array<AssetRecord> records;
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
            continue;
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

    AssetPipelineDiagnostic publishDiagnostic;
    if (database.PublishFullSnapshot(records, publishDiagnostic))
    {
        result.Diagnostics.Add(publishDiagnostic);
        return true;
    }
    result.Revision = database.GetRevision();
    return false;
}
