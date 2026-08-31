// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseStorage.h"
#include "AssetMeta.h"
#include "AssetMount.h"
#include "SubAsset.h"
#include "Engine/Content/Artifacts/ArtifactManifest.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "ThirdParty/sqlite/sqlite3.h"

namespace
{
    struct DatabaseHandle
    {
        sqlite3* Value = nullptr;

        ~DatabaseHandle()
        {
            if (Value)
                sqlite3_close(Value);
        }
    };

    struct StatementHandle
    {
        sqlite3_stmt* Value = nullptr;

        ~StatementHandle()
        {
            if (Value)
                sqlite3_finalize(Value);
        }
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        diagnostic.Remediation = TEXT("Delete Library/AssetDatabase and reopen the project to rebuild derived state.");
        return true;
    }

    bool FailSql(AssetPipelineDiagnostic& diagnostic, const StringView& path, sqlite3* database, const char* operation)
    {
        return Fail(diagnostic, path, String::Format(TEXT("Asset database {0} failed: {1}"),
            String(StringAnsiView(operation)), String(StringAnsiView(sqlite3_errmsg(database)))));
    }

    bool Execute(sqlite3* database, const char* sql, AssetPipelineDiagnostic& diagnostic, const StringView& path)
    {
        char* error = nullptr;
        const int32 result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
        if (result == SQLITE_OK)
            return false;
        const String message = error ? String(StringAnsiView(error)) : String(StringAnsiView(sqlite3_errmsg(database)));
        sqlite3_free(error);
        return Fail(diagnostic, path, String::Format(TEXT("Asset database command failed: {0}"), message));
    }

    bool Open(const StringView& path, DatabaseHandle& result, AssetPipelineDiagnostic& diagnostic)
    {
        const StringAnsi utf8Path(path);
        if (sqlite3_open_v2(utf8Path.Get(), &result.Value, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            return FailSql(diagnostic, path, result.Value, "open");
        sqlite3_extended_result_codes(result.Value, 1);
        sqlite3_busy_timeout(result.Value, 15000);
        return Execute(result.Value,
            "PRAGMA journal_mode=WAL;"
            "PRAGMA synchronous=FULL;"
            "PRAGMA foreign_keys=ON;"
            "PRAGMA temp_store=MEMORY;",
            diagnostic, path);
    }

    bool OpenReadOnly(const StringView& path, DatabaseHandle& result, AssetPipelineDiagnostic& diagnostic)
    {
        const StringAnsi utf8Path(path);
        if (sqlite3_open_v2(utf8Path.Get(), &result.Value, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            return FailSql(diagnostic, path, result.Value, "open read-only");
        sqlite3_extended_result_codes(result.Value, 1);
        sqlite3_busy_timeout(result.Value, 15000);
        return Execute(result.Value, "PRAGMA query_only=ON;PRAGMA foreign_keys=ON;", diagnostic, path);
    }

    bool ValidateCurrentSchema(sqlite3* database, const StringView& path, AssetPipelineDiagnostic& diagnostic)
    {
        StatementHandle statement;
        if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement.Value, nullptr) != SQLITE_OK ||
            sqlite3_step(statement.Value) != SQLITE_ROW)
            return FailSql(diagnostic, path, database, "read schema version");
        if (sqlite3_column_int(statement.Value, 0) != AssetDatabaseStorage::SchemaVersion)
            return Fail(diagnostic, path, TEXT("Durable asset database schema is not current and cannot be opened by an import worker."));
        return false;
    }

    bool EnsureSchema(sqlite3* database, const StringView& path, AssetPipelineDiagnostic& diagnostic)
    {
        int32 version = 0;
        {
            StatementHandle versionStatement;
            if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &versionStatement.Value, nullptr) != SQLITE_OK ||
                sqlite3_step(versionStatement.Value) != SQLITE_ROW)
                return FailSql(diagnostic, path, database, "read schema version");
            version = sqlite3_column_int(versionStatement.Value, 0);
        }
        if (version > AssetDatabaseStorage::SchemaVersion)
            return Fail(diagnostic, path, TEXT("Asset database was created by a newer engine version."));
        if (version == 1)
        {
            // Dependency rows are derived from immutable manifests. Drop the old,
            // incomplete reverse table and current selections so results are
            // republished with the complete v3 dependency fingerprint contract.
            if (Execute(database,
                "BEGIN IMMEDIATE;"
                "DROP TABLE IF EXISTS artifact_dependencies;"
                "DELETE FROM current_artifacts;"
                "PRAGMA user_version=2;"
                "COMMIT;", diagnostic, path))
                return true;
            version = 2;
        }
        if (version == 2)
        {
            if (Execute(database,
                "BEGIN IMMEDIATE;"
                "ALTER TABLE asset_objects ADD COLUMN object_revision INTEGER NOT NULL DEFAULT 0;"
                "UPDATE asset_objects SET object_revision=(SELECT source_revision FROM source_assets WHERE source_assets.guid=asset_objects.file_guid);"
                "PRAGMA user_version=3;"
                "COMMIT;", diagnostic, path))
                return true;
            version = 3;
        }
        if (version == 3)
        {
            if (Execute(database,
                "BEGIN IMMEDIATE;"
                "CREATE TABLE source_object_dependencies(owner_guid BLOB NOT NULL,input_file_guid BLOB NOT NULL,input_local_id INTEGER NOT NULL,PRIMARY KEY(owner_guid,input_file_guid,input_local_id));"
                "CREATE TABLE runtime_object_references(owner_guid BLOB NOT NULL,referenced_file_guid BLOB NOT NULL,referenced_local_id INTEGER NOT NULL,PRIMARY KEY(owner_guid,referenced_file_guid,referenced_local_id));"
                "ALTER TABLE artifact_dependencies ADD COLUMN target_file_guid BLOB;"
                "ALTER TABLE artifact_dependencies ADD COLUMN target_local_id INTEGER;"
                "INSERT OR IGNORE INTO source_object_dependencies SELECT d.owner_guid,o.file_guid,o.local_id FROM source_dependencies d JOIN asset_objects o ON o.backing_id=d.input_guid;"
                "INSERT OR IGNORE INTO runtime_object_references SELECT r.owner_guid,o.file_guid,o.local_id FROM runtime_references r JOIN asset_objects o ON o.backing_id=r.referenced_guid;"
                "CREATE INDEX source_object_dependencies_target_idx ON source_object_dependencies(input_file_guid,input_local_id);"
                "CREATE INDEX runtime_object_references_target_idx ON runtime_object_references(referenced_file_guid,referenced_local_id);"
                "PRAGMA user_version=4;"
                "COMMIT;", diagnostic, path))
                return true;
            version = 4;
        }
        if (version != 0 && version < AssetDatabaseStorage::SchemaVersion)
            return Fail(diagnostic, path, TEXT("Asset database schema is obsolete and must be rebuilt."));

        const char* schema =
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS database_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS mounts(mount_id BLOB PRIMARY KEY,logical_prefix TEXT UNIQUE NOT NULL,physical_root TEXT NOT NULL,kind INTEGER NOT NULL,writable INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS source_assets(guid BLOB PRIMARY KEY,mount_id BLOB,logical_path TEXT UNIQUE NOT NULL,portability_key TEXT UNIQUE NOT NULL,source_path TEXT NOT NULL,meta_path TEXT NOT NULL,is_folder INTEGER NOT NULL,source_kind INTEGER NOT NULL,importer_id TEXT NOT NULL,settings_schema_version INTEGER NOT NULL DEFAULT 1,meta_semantic_hash INTEGER NOT NULL,source_content_hash BLOB,source_size INTEGER NOT NULL DEFAULT 0,observed_mtime_ns INTEGER NOT NULL DEFAULT 0,observed_file_id BLOB,status INTEGER NOT NULL,source_revision INTEGER NOT NULL,last_refresh_session INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS asset_objects(file_guid BLOB NOT NULL,local_id INTEGER NOT NULL,backing_id BLOB NOT NULL,stable_key TEXT NOT NULL,type_name TEXT NOT NULL,is_main INTEGER NOT NULL,removed INTEGER NOT NULL,status INTEGER NOT NULL,object_revision INTEGER NOT NULL,PRIMARY KEY(file_guid,local_id),UNIQUE(file_guid,stable_key));"
            "CREATE TABLE IF NOT EXISTS labels(guid BLOB NOT NULL,label TEXT NOT NULL,PRIMARY KEY(guid,label));"
            "CREATE TABLE IF NOT EXISTS deleted_session_assets(guid BLOB NOT NULL,old_path TEXT NOT NULL,deleted_session INTEGER NOT NULL,payload BLOB);"
            "CREATE TABLE IF NOT EXISTS file_observations(portability_key TEXT PRIMARY KEY,size INTEGER NOT NULL,mtime_ns INTEGER NOT NULL,file_id BLOB,volume_id INTEGER NOT NULL,change_ticks INTEGER NOT NULL,identity_reliable INTEGER NOT NULL,content_hash BLOB,cache_checksum INTEGER NOT NULL,last_seen_session INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS source_dependencies(owner_guid BLOB NOT NULL,input_guid BLOB NOT NULL,PRIMARY KEY(owner_guid,input_guid));"
            "CREATE TABLE IF NOT EXISTS runtime_references(owner_guid BLOB NOT NULL,referenced_guid BLOB NOT NULL,PRIMARY KEY(owner_guid,referenced_guid));"
            "CREATE TABLE IF NOT EXISTS source_object_dependencies(owner_guid BLOB NOT NULL,input_file_guid BLOB NOT NULL,input_local_id INTEGER NOT NULL,PRIMARY KEY(owner_guid,input_file_guid,input_local_id));"
            "CREATE TABLE IF NOT EXISTS runtime_object_references(owner_guid BLOB NOT NULL,referenced_file_guid BLOB NOT NULL,referenced_local_id INTEGER NOT NULL,PRIMARY KEY(owner_guid,referenced_file_guid,referenced_local_id));"
            "CREATE TABLE IF NOT EXISTS artifacts(artifact_key BLOB PRIMARY KEY,guid BLOB NOT NULL,target_key BLOB,importer_id TEXT NOT NULL,importer_version INTEGER NOT NULL,source_input_key BLOB,manifest_path TEXT,status INTEGER NOT NULL,created_utc INTEGER NOT NULL,deterministic INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS artifact_objects(artifact_key BLOB NOT NULL,local_id INTEGER NOT NULL,type_name TEXT NOT NULL,output_name TEXT NOT NULL,blob_hash BLOB NOT NULL,PRIMARY KEY(artifact_key,local_id));"
            "CREATE TABLE IF NOT EXISTS artifact_outputs(artifact_key BLOB NOT NULL,output_kind TEXT NOT NULL,output_key BLOB NOT NULL,relative_path TEXT NOT NULL,content_hash BLOB NOT NULL,size INTEGER NOT NULL,compatibility TEXT NOT NULL,PRIMARY KEY(artifact_key,output_kind));"
            "CREATE TABLE IF NOT EXISTS artifact_dependencies(artifact_key BLOB NOT NULL,kind INTEGER NOT NULL,state INTEGER NOT NULL,identity TEXT NOT NULL,target_guid BLOB,target_file_guid BLOB,target_local_id INTEGER,observed_fingerprint BLOB NOT NULL,source_hash BLOB,metadata_hash BLOB,target_artifact_key BLOB,interface_hash BLOB,interface_version INTEGER NOT NULL,PRIMARY KEY(artifact_key,kind,identity));"
            "CREATE TABLE IF NOT EXISTS current_artifacts(guid BLOB NOT NULL,target_key BLOB NOT NULL,desired_input_key BLOB,current_artifact_key BLOB,last_good_artifact_key BLOB,import_status INTEGER NOT NULL,diagnostic_id INTEGER,PRIMARY KEY(guid,target_key));"
            "CREATE TABLE IF NOT EXISTS custom_dependencies(name TEXT PRIMARY KEY,hash BLOB,revision INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS import_history(import_id BLOB PRIMARY KEY,guid BLOB NOT NULL,target_key BLOB,reason_mask INTEGER NOT NULL,desired_input_key BLOB,artifact_key BLOB,started_utc INTEGER NOT NULL,completed_utc INTEGER,result INTEGER NOT NULL,log_path TEXT);"
            "CREATE INDEX IF NOT EXISTS source_assets_importer_idx ON source_assets(importer_id);"
            "CREATE INDEX IF NOT EXISTS asset_objects_backing_idx ON asset_objects(backing_id);"
            "CREATE INDEX IF NOT EXISTS source_dependencies_input_idx ON source_dependencies(input_guid);"
            "CREATE INDEX IF NOT EXISTS runtime_references_target_idx ON runtime_references(referenced_guid);"
            "CREATE INDEX IF NOT EXISTS source_object_dependencies_target_idx ON source_object_dependencies(input_file_guid,input_local_id);"
            "CREATE INDEX IF NOT EXISTS runtime_object_references_target_idx ON runtime_object_references(referenced_file_guid,referenced_local_id);"
            "CREATE INDEX IF NOT EXISTS artifact_dependencies_identity_idx ON artifact_dependencies(kind,identity);"
            "CREATE INDEX IF NOT EXISTS artifact_dependencies_guid_idx ON artifact_dependencies(target_guid);"
            "PRAGMA user_version=5;"
            "COMMIT;";
        return Execute(database, schema, diagnostic, path);
    }

    bool Prepare(sqlite3* database, const char* sql, StatementHandle& statement, AssetPipelineDiagnostic& diagnostic, const StringView& path)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement.Value, nullptr) != SQLITE_OK)
            return FailSql(diagnostic, path, database, "prepare statement");
        return false;
    }

    void BindGuid(sqlite3_stmt* statement, int32 index, const Guid& value)
    {
        sqlite3_bind_blob(statement, index, &value, sizeof(Guid), SQLITE_TRANSIENT);
    }

    void BindHash(sqlite3_stmt* statement, int32 index, const ContentHash& value)
    {
        sqlite3_bind_blob(statement, index, value.Bytes, sizeof(value.Bytes), SQLITE_TRANSIENT);
    }

    void BindKey(sqlite3_stmt* statement, int32 index, const ArtifactKey& value)
    {
        BindHash(statement, index, value.Digest);
    }

    void BindText(sqlite3_stmt* statement, int32 index, const StringView& value)
    {
        const StringAnsi utf8(value);
        sqlite3_bind_text(statement, index, utf8.Length() ? utf8.Get() : "", utf8.Length(), SQLITE_TRANSIENT);
    }

    ArtifactKey BuildDependencyFingerprint(const ArtifactManifestDependency& dependency)
    {
        ArtifactKeyBuilder builder(StringAnsiView("FlaxAssetDependency/v4"));
        builder.AddUInt32(StringAnsiView("kind"), static_cast<uint32>(dependency.Kind));
        builder.AddUInt32(StringAnsiView("state"), static_cast<uint32>(dependency.State));
        builder.AddString(StringAnsiView("identity"), dependency.Identity);
        builder.AddBool(StringAnsiView("has-object-id"), dependency.ObjectID.IsValid());
        if (dependency.ObjectID.IsValid())
        {
            builder.AddGuid(StringAnsiView("file-guid"), dependency.ObjectID.Guid);
            builder.AddUInt64(StringAnsiView("local-id"), static_cast<uint64>(dependency.ObjectID.LocalId));
        }
        builder.AddBool(StringAnsiView("has-asset-guid"), dependency.AssetID.IsValid());
        if (dependency.AssetID.IsValid())
            builder.AddGuid(StringAnsiView("asset-guid"), dependency.AssetID);
        builder.AddBool(StringAnsiView("has-source-hash"), !dependency.Hash.IsZero());
        if (!dependency.Hash.IsZero())
            builder.AddHash(StringAnsiView("source-hash"), dependency.Hash);
        builder.AddBool(StringAnsiView("has-metadata-hash"), !dependency.MetadataHash.IsZero());
        if (!dependency.MetadataHash.IsZero())
            builder.AddHash(StringAnsiView("metadata-hash"), dependency.MetadataHash);
        builder.AddBool(StringAnsiView("has-artifact-key"), !dependency.ExactArtifact.IsZero());
        if (!dependency.ExactArtifact.IsZero())
            builder.AddKey(StringAnsiView("artifact-key"), dependency.ExactArtifact);
        builder.AddBool(StringAnsiView("has-interface-hash"), !dependency.InterfaceHash.IsZero());
        if (!dependency.InterfaceHash.IsZero())
            builder.AddHash(StringAnsiView("interface-hash"), dependency.InterfaceHash);
        builder.AddUInt32(StringAnsiView("interface-version"), dependency.InterfaceVersion);
        return builder.Finalize();
    }

    bool WriteArtifactObjects(sqlite3* database, const StringView& databasePath, const ArtifactKey& artifactKey,
        const ArtifactManifest& manifest, AssetPipelineDiagnostic& diagnostic)
    {
        StatementHandle clearObjects;
        if (Prepare(database, "DELETE FROM artifact_objects WHERE artifact_key=?1", clearObjects, diagnostic, databasePath))
            return true;
        BindKey(clearObjects.Value, 1, artifactKey);
        if (sqlite3_step(clearObjects.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database, "replace artifact object inventory");

        StatementHandle object;
        if (Prepare(database,
            "INSERT INTO artifact_objects(artifact_key,local_id,type_name,output_name,blob_hash) VALUES(?1,?2,?3,?4,?5)",
            object, diagnostic, databasePath))
            return true;
        const ContentHash& blobHash = manifest.Outputs[0].Content;
        for (const ArtifactManifestObject& value : manifest.Objects)
        {
            sqlite3_reset(object.Value);
            sqlite3_clear_bindings(object.Value);
            BindKey(object.Value, 1, artifactKey);
            sqlite3_bind_int64(object.Value, 2, value.ObjectID.LocalId);
            BindText(object.Value, 3, value.TypeName);
            BindText(object.Value, 4, value.StableKey.HasChars() ? value.StableKey : value.Name);
            BindHash(object.Value, 5, blobHash);
            if (sqlite3_step(object.Value) != SQLITE_DONE)
                return FailSql(diagnostic, databasePath, database, "write artifact object inventory");
        }
        return false;
    }

    Guid ReadGuid(sqlite3_stmt* statement, int32 column)
    {
        Guid result = Guid::Empty;
        const void* data = sqlite3_column_blob(statement, column);
        if (data && sqlite3_column_bytes(statement, column) == sizeof(Guid))
            Platform::MemoryCopy(&result, data, sizeof(Guid));
        return result;
    }

    String ReadText(sqlite3_stmt* statement, int32 column)
    {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
        return value ? String(StringAnsiView(value, sqlite3_column_bytes(statement, column))) : String::Empty;
    }

    struct PreparedArtifactPublication
    {
        const ArtifactManifest* Manifest = nullptr;
        ArtifactKey Artifact;
        ArtifactKey Target;
        String RelativeManifest;
    };

    bool PrepareArtifactPublication(const StringView& libraryRoot, const ArtifactManifest& manifest,
        PreparedArtifactPublication& prepared, AssetPipelineDiagnostic& diagnostic)
    {
        if (manifest.Validate(StringView::Empty, diagnostic))
            return true;
        ArtifactKeyBuilder keyBuilder(StringAnsiView("FlaxArtifact/v4"));
        keyBuilder.AddGuid(StringAnsiView("asset"), manifest.AssetID);
        keyBuilder.AddKey(StringAnsiView("desired-input"), manifest.InputFingerprint);
        keyBuilder.AddString(StringAnsiView("importer"), manifest.ProcessorID);
        keyBuilder.AddUInt32(StringAnsiView("importer-version"), manifest.ProcessorImplementationVersion);
        keyBuilder.AddKey(StringAnsiView("target"), manifest.Target.BuildKey(ArtifactTargetDimension::All));
        Array<StringAnsi> dependencyInputs;
        for (const ArtifactManifestDependency& dependency : manifest.Dependencies)
            dependencyInputs.Add(BuildDependencyFingerprint(dependency).ToString());
        keyBuilder.AddSortedStrings(StringAnsiView("dependencies"), dependencyInputs);
        Array<StringAnsi> outputInputs;
        for (const ArtifactManifestOutput& output : manifest.Outputs)
        {
            outputInputs.Add(StringAnsi::Format("{0}|{1}|{2}|{3}|{4}", output.Kind, output.FormatVersion,
                output.Key.ToString(), output.Content.ToString(), output.Size));
        }
        keyBuilder.AddSortedStrings(StringAnsiView("outputs"), outputInputs);
        Array<StringAnsi> objectInputs;
        for (const ArtifactManifestObject& object : manifest.Objects)
        {
            ArtifactKeyBuilder objectBuilder(StringAnsiView("FlaxArtifactObject/v1"));
            objectBuilder.AddGuid(StringAnsiView("file-guid"), object.ObjectID.Guid);
            objectBuilder.AddUInt64(StringAnsiView("local-id"), static_cast<uint64>(object.ObjectID.LocalId));
            objectBuilder.AddGuid(StringAnsiView("backing-guid"), object.BackingAssetID);
            objectBuilder.AddString(StringAnsiView("type"), object.TypeName);
            objectBuilder.AddString(StringAnsiView("name"), object.Name);
            objectBuilder.AddString(StringAnsiView("stable-key"), object.StableKey);
            objectBuilder.AddBool(StringAnsiView("main"), object.IsMainObject);
            objectInputs.Add(objectBuilder.Finalize().ToString());
        }
        keyBuilder.AddSortedStrings(StringAnsiView("objects"), objectInputs);
        prepared.Manifest = &manifest;
        prepared.Artifact = keyBuilder.Finalize();
        prepared.Target = manifest.Target.BuildKey(ArtifactTargetDimension::All);

        const String keyText(prepared.Artifact.ToString());
        const String immutableDirectory = ArtifactStore::GetArtifactsPath(libraryRoot) / keyText.Substring(0, 2) / keyText;
        const String immutableManifest = immutableDirectory / TEXT("manifest.json");
        if (!FileSystem::DirectoryExists(immutableDirectory) && FileSystem::CreateDirectory(immutableDirectory))
            return Fail(diagnostic, immutableDirectory, TEXT("Cannot create immutable artifact manifest directory."));
        if (!FileSystem::FileExists(immutableManifest))
        {
            StringAnsi manifestJson;
            if (manifest.ToJson(manifestJson, diagnostic))
                return true;
            const String staging = immutableManifest + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
            SCOPE_EXIT { FileSystem::DeleteFile(staging); };
            if (File::WriteAllBytes(staging, manifestJson.Get(), manifestJson.Length()) || FileSystem::MoveFile(immutableManifest, staging, false))
            {
                if (!FileSystem::FileExists(immutableManifest))
                    return Fail(diagnostic, immutableManifest, TEXT("Cannot publish immutable artifact manifest."));
            }
        }
        StringAnsi verifiedJson;
        ArtifactManifest verifiedManifest;
        if (File::ReadAllText(immutableManifest, verifiedJson) || ArtifactManifest::Parse(verifiedJson, immutableManifest, verifiedManifest, diagnostic) ||
            verifiedManifest.AssetID != manifest.AssetID || verifiedManifest.InputFingerprint != manifest.InputFingerprint)
            return Fail(diagnostic, immutableManifest, TEXT("Immutable artifact manifest validation failed."));
        return ArtifactStore::TryMakeLibraryRelative(libraryRoot, immutableManifest, prepared.RelativeManifest, diagnostic);
    }

    bool WriteArtifactPublication(sqlite3* database, const StringView& databasePath,
        const PreparedArtifactPublication& prepared, AssetPipelineDiagnostic& diagnostic)
    {
        const ArtifactManifest& manifest = *prepared.Manifest;
        StatementHandle artifact;
        if (Prepare(database,
            "INSERT OR IGNORE INTO artifacts(artifact_key,guid,target_key,importer_id,importer_version,source_input_key,manifest_path,status,created_utc,deterministic) VALUES(?1,?2,?3,?4,?5,?6,?7,0,unixepoch(),1)",
            artifact, diagnostic, databasePath))
            return true;
        BindKey(artifact.Value, 1, prepared.Artifact);
        BindGuid(artifact.Value, 2, manifest.AssetID);
        BindKey(artifact.Value, 3, prepared.Target);
        BindText(artifact.Value, 4, manifest.ProcessorID);
        sqlite3_bind_int64(artifact.Value, 5, manifest.ProcessorImplementationVersion);
        BindKey(artifact.Value, 6, manifest.InputFingerprint);
        BindText(artifact.Value, 7, prepared.RelativeManifest);
        if (sqlite3_step(artifact.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database, "write artifact");

        StatementHandle output;
        if (Prepare(database,
            "INSERT OR REPLACE INTO artifact_outputs(artifact_key,output_kind,output_key,relative_path,content_hash,size,compatibility) VALUES(?1,?2,?3,?4,?5,?6,?7)",
            output, diagnostic, databasePath))
            return true;
        for (const ArtifactManifestOutput& value : manifest.Outputs)
        {
            sqlite3_reset(output.Value);
            sqlite3_clear_bindings(output.Value);
            BindKey(output.Value, 1, prepared.Artifact);
            BindText(output.Value, 2, String(value.Kind));
            BindKey(output.Value, 3, value.Key);
            BindText(output.Value, 4, value.RelativePath);
            BindHash(output.Value, 5, value.Content);
            sqlite3_bind_int64(output.Value, 6, static_cast<int64>(value.Size));
            BindText(output.Value, 7, String(value.Compatibility));
            if (sqlite3_step(output.Value) != SQLITE_DONE)
                return FailSql(diagnostic, databasePath, database, "write artifact output");
        }
        if (WriteArtifactObjects(database, databasePath, prepared.Artifact, manifest, diagnostic))
            return true;

        StatementHandle clearDependencies;
        if (Prepare(database, "DELETE FROM artifact_dependencies WHERE artifact_key=?1", clearDependencies, diagnostic, databasePath))
            return true;
        BindKey(clearDependencies.Value, 1, prepared.Artifact);
        if (sqlite3_step(clearDependencies.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database, "replace artifact dependencies");
        StatementHandle dependency;
        if (Prepare(database,
            "INSERT INTO artifact_dependencies(artifact_key,kind,state,identity,target_guid,target_file_guid,target_local_id,observed_fingerprint,source_hash,metadata_hash,target_artifact_key,interface_hash,interface_version) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
            dependency, diagnostic, databasePath))
            return true;
        for (const ArtifactManifestDependency& value : manifest.Dependencies)
        {
            sqlite3_reset(dependency.Value);
            sqlite3_clear_bindings(dependency.Value);
            BindKey(dependency.Value, 1, prepared.Artifact);
            sqlite3_bind_int(dependency.Value, 2, static_cast<int32>(value.Kind));
            sqlite3_bind_int(dependency.Value, 3, static_cast<int32>(value.State));
            BindText(dependency.Value, 4, value.Identity);
            if (value.AssetID.IsValid())
                BindGuid(dependency.Value, 5, value.AssetID);
            else
                sqlite3_bind_null(dependency.Value, 5);
            if (value.ObjectID.IsValid())
            {
                BindGuid(dependency.Value, 6, value.ObjectID.Guid);
                sqlite3_bind_int64(dependency.Value, 7, value.ObjectID.LocalId);
            }
            else
            {
                sqlite3_bind_null(dependency.Value, 6);
                sqlite3_bind_null(dependency.Value, 7);
            }
            BindKey(dependency.Value, 8, BuildDependencyFingerprint(value));
            if (!value.Hash.IsZero())
                BindHash(dependency.Value, 9, value.Hash);
            else
                sqlite3_bind_null(dependency.Value, 9);
            if (!value.MetadataHash.IsZero())
                BindHash(dependency.Value, 10, value.MetadataHash);
            else
                sqlite3_bind_null(dependency.Value, 10);
            if (value.ExactArtifact.IsZero())
                sqlite3_bind_null(dependency.Value, 11);
            else
                BindKey(dependency.Value, 11, value.ExactArtifact);
            if (!value.InterfaceHash.IsZero())
                BindHash(dependency.Value, 12, value.InterfaceHash);
            else
                sqlite3_bind_null(dependency.Value, 12);
            sqlite3_bind_int64(dependency.Value, 13, value.InterfaceVersion);
            if (sqlite3_step(dependency.Value) != SQLITE_DONE)
                return FailSql(diagnostic, databasePath, database, "write artifact dependency");
        }

        StatementHandle current;
        if (Prepare(database,
            "INSERT INTO current_artifacts(guid,target_key,desired_input_key,current_artifact_key,last_good_artifact_key,import_status,diagnostic_id) VALUES(?1,?2,?3,?4,NULL,0,NULL) "
            "ON CONFLICT(guid,target_key) DO UPDATE SET desired_input_key=excluded.desired_input_key,last_good_artifact_key=COALESCE(current_artifacts.current_artifact_key,current_artifacts.last_good_artifact_key),current_artifact_key=excluded.current_artifact_key,import_status=0,diagnostic_id=NULL",
            current, diagnostic, databasePath))
            return true;
        BindGuid(current.Value, 1, manifest.AssetID);
        BindKey(current.Value, 2, prepared.Target);
        BindKey(current.Value, 3, manifest.InputFingerprint);
        BindKey(current.Value, 4, prepared.Artifact);
        if (sqlite3_step(current.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database, "publish current artifact mapping");

        StatementHandle history;
        if (Prepare(database,
            "INSERT INTO import_history(import_id,guid,target_key,reason_mask,desired_input_key,artifact_key,started_utc,completed_utc,result,log_path) VALUES(?1,?2,?3,0,?4,?5,unixepoch(),unixepoch(),0,'')",
            history, diagnostic, databasePath))
            return true;
        const Guid importID = Guid::New();
        BindGuid(history.Value, 1, importID);
        BindGuid(history.Value, 2, manifest.AssetID);
        BindKey(history.Value, 3, prepared.Target);
        BindKey(history.Value, 4, manifest.InputFingerprint);
        BindKey(history.Value, 5, prepared.Artifact);
        if (sqlite3_step(history.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database, "write import history");
        return false;
    }
}

bool AssetDatabaseStorage::Save(const StringView& path, const StringView& projectRoot, const StringView& contentRoot,
    const AssetDatabaseSnapshot& snapshot, const Array<SourceHashFileState>& fileStates,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    const String directory = StringUtils::GetDirectoryName(path);
    if (!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory))
        return Fail(diagnostic, path, TEXT("Cannot create the durable asset database directory."));

    DatabaseHandle database;
    if (Open(path, database, diagnostic) || EnsureSchema(database.Value, path, diagnostic))
        return true;
    if (Execute(database.Value, "BEGIN IMMEDIATE;", diagnostic, path))
        return true;
    bool committed = false;
    SCOPE_EXIT
    {
        if (!committed)
            sqlite3_exec(database.Value, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    if (Execute(database.Value,
        "DELETE FROM database_metadata;"
        "DELETE FROM mounts;"
        "DELETE FROM source_dependencies;"
        "DELETE FROM runtime_references;"
        "DELETE FROM source_object_dependencies;"
        "DELETE FROM runtime_object_references;"
        "DELETE FROM asset_objects;"
        "DELETE FROM source_assets;"
        "DELETE FROM labels;"
        "DELETE FROM file_observations;",
        diagnostic, path))
        return true;

    StatementHandle metadata;
    if (Prepare(database.Value, "INSERT INTO database_metadata(key,value) VALUES(?1,?2)", metadata, diagnostic, path))
        return true;
    const auto insertMetadata = [&](const char* key, const StringView& value)
    {
        sqlite3_reset(metadata.Value);
        sqlite3_clear_bindings(metadata.Value);
        sqlite3_bind_text(metadata.Value, 1, key, -1, SQLITE_STATIC);
        BindText(metadata.Value, 2, value);
        return sqlite3_step(metadata.Value) != SQLITE_DONE;
    };
    if (insertMetadata("project_root", projectRoot) || insertMetadata("content_root", contentRoot) ||
        insertMetadata("metadata_format", String::Format(TEXT("{0}"), AssetMeta::CurrentMetaVersion)))
        return FailSql(diagnostic, path, database.Value, "write database roots");

    StatementHandle mount;
    if (Prepare(database.Value, "INSERT INTO mounts(mount_id,logical_prefix,physical_root,kind,writable) VALUES(?1,?2,?3,?4,?5)", mount, diagnostic, path))
        return true;
    const Array<AssetMount> configuredMounts = AssetMountRegistry::GetMounts();
    for (const AssetMount& configuredMount : configuredMounts)
    {
        sqlite3_reset(mount.Value);
        sqlite3_clear_bindings(mount.Value);
        BindGuid(mount.Value, 1, configuredMount.MountId);
        BindText(mount.Value, 2, configuredMount.LogicalPrefix);
        BindText(mount.Value, 3, configuredMount.PhysicalRoot);
        sqlite3_bind_int(mount.Value, 4, static_cast<int32>(configuredMount.Kind));
        sqlite3_bind_int(mount.Value, 5, configuredMount.Writable ? 1 : 0);
        if (sqlite3_step(mount.Value) != SQLITE_DONE)
            return FailSql(diagnostic, path, database.Value, "write asset mount");
    }

    StatementHandle source;
    if (Prepare(database.Value,
        "INSERT INTO source_assets(guid,mount_id,logical_path,portability_key,source_path,meta_path,is_folder,source_kind,importer_id,settings_schema_version,meta_semantic_hash,status,source_revision,last_refresh_session) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,1,?10,?11,?12,?13)", source, diagnostic, path))
        return true;
    StatementHandle object;
    if (Prepare(database.Value,
        "INSERT INTO asset_objects(file_guid,local_id,backing_id,stable_key,type_name,is_main,removed,status,object_revision) VALUES(?1,?2,?3,?4,?5,?6,0,?7,?8)",
        object, diagnostic, path))
        return true;
    StatementHandle sourceDependency;
    if (Prepare(database.Value, "INSERT OR IGNORE INTO source_dependencies(owner_guid,input_guid) VALUES(?1,?2)", sourceDependency, diagnostic, path))
        return true;
    StatementHandle sourceObjectDependency;
    if (Prepare(database.Value, "INSERT OR IGNORE INTO source_object_dependencies(owner_guid,input_file_guid,input_local_id) VALUES(?1,?2,?3)", sourceObjectDependency, diagnostic, path))
        return true;
    StatementHandle runtimeReference;
    if (Prepare(database.Value, "INSERT OR IGNORE INTO runtime_references(owner_guid,referenced_guid) VALUES(?1,?2)", runtimeReference, diagnostic, path))
        return true;
    StatementHandle runtimeObjectReference;
    if (Prepare(database.Value, "INSERT OR IGNORE INTO runtime_object_references(owner_guid,referenced_file_guid,referenced_local_id) VALUES(?1,?2,?3)", runtimeObjectReference, diagnostic, path))
        return true;
    StatementHandle label;
    if (Prepare(database.Value, "INSERT INTO labels(guid,label) VALUES(?1,?2)", label, diagnostic, path))
        return true;

    for (const AssetRecord& record : snapshot.Records)
    {
        const bool isMain = record.SubAsset.IsEmpty();
        if (isMain)
        {
            AssetMountResolution mountResolution;
            if (AssetMountRegistry::Get().ResolvePhysical(record.SourcePath.Get(), mountResolution, diagnostic))
                return true;
            sqlite3_reset(source.Value);
            sqlite3_clear_bindings(source.Value);
            BindGuid(source.Value, 1, record.SourceAssetID);
            BindGuid(source.Value, 2, mountResolution.Mount.MountId);
            BindText(source.Value, 3, mountResolution.LogicalPath);
            BindText(source.Value, 4, record.PortabilityKey);
            BindText(source.Value, 5, record.SourcePath.Get());
            BindText(source.Value, 6, record.MetaPath.Get());
            sqlite3_bind_int(source.Value, 7, record.SourceKind == AssetSourceKind::Folder ? 1 : 0);
            sqlite3_bind_int(source.Value, 8, static_cast<int32>(record.SourceKind));
            BindText(source.Value, 9, record.ProcessorID);
            sqlite3_bind_int64(source.Value, 10, static_cast<int64>(record.MetaSemanticHash));
            sqlite3_bind_int(source.Value, 11, static_cast<int32>(record.Status));
            sqlite3_bind_int64(source.Value, 12, static_cast<int64>(record.DatabaseRevision));
            sqlite3_bind_int64(source.Value, 13, static_cast<int64>(snapshot.Revision));
            if (sqlite3_step(source.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write source record");
            for (const String& value : record.Labels)
            {
                sqlite3_reset(label.Value);
                sqlite3_clear_bindings(label.Value);
                BindGuid(label.Value, 1, record.SourceAssetID);
                BindText(label.Value, 2, value);
                if (sqlite3_step(label.Value) != SQLITE_DONE)
                    return FailSql(diagnostic, path, database.Value, "write source label");
            }
        }

        sqlite3_reset(object.Value);
        sqlite3_clear_bindings(object.Value);
        BindGuid(object.Value, 1, record.SourceAssetID);
        sqlite3_bind_int64(object.Value, 2, record.LocalId);
        BindGuid(object.Value, 3, record.ID);
        BindText(object.Value, 4, isMain ? StringView(TEXT("main")) : StringView(record.SubAsset.Get()));
        BindText(object.Value, 5, record.TypeName);
        sqlite3_bind_int(object.Value, 6, isMain ? 1 : 0);
        sqlite3_bind_int(object.Value, 7, static_cast<int32>(record.Status));
        sqlite3_bind_int64(object.Value, 8, static_cast<int64>(record.DatabaseRevision));
        if (sqlite3_step(object.Value) != SQLITE_DONE)
            return FailSql(diagnostic, path, database.Value, "write object record");

        for (const Guid& dependency : record.BuildInputDependencies)
        {
            sqlite3_reset(sourceDependency.Value);
            sqlite3_clear_bindings(sourceDependency.Value);
            BindGuid(sourceDependency.Value, 1, record.ID);
            BindGuid(sourceDependency.Value, 2, dependency);
            if (sqlite3_step(sourceDependency.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write source dependency");
        }
        for (const AssetObjectId& dependency : record.BuildInputObjectDependencies)
        {
            sqlite3_reset(sourceObjectDependency.Value);
            sqlite3_clear_bindings(sourceObjectDependency.Value);
            BindGuid(sourceObjectDependency.Value, 1, record.ID);
            BindGuid(sourceObjectDependency.Value, 2, dependency.Guid);
            sqlite3_bind_int64(sourceObjectDependency.Value, 3, dependency.LocalId);
            if (sqlite3_step(sourceObjectDependency.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write exact source dependency");

            sqlite3_reset(sourceDependency.Value);
            sqlite3_clear_bindings(sourceDependency.Value);
            BindGuid(sourceDependency.Value, 1, record.ID);
            BindGuid(sourceDependency.Value, 2, SubAssetPolicy::GetBackingAssetId(dependency.Guid, dependency.LocalId));
            if (sqlite3_step(sourceDependency.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write source dependency projection");
        }
        for (const Guid& reference : record.RuntimeReferences)
        {
            sqlite3_reset(runtimeReference.Value);
            sqlite3_clear_bindings(runtimeReference.Value);
            BindGuid(runtimeReference.Value, 1, record.ID);
            BindGuid(runtimeReference.Value, 2, reference);
            if (sqlite3_step(runtimeReference.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write runtime reference");
        }
        for (const AssetObjectId& reference : record.RuntimeObjectReferences)
        {
            sqlite3_reset(runtimeObjectReference.Value);
            sqlite3_clear_bindings(runtimeObjectReference.Value);
            BindGuid(runtimeObjectReference.Value, 1, record.ID);
            BindGuid(runtimeObjectReference.Value, 2, reference.Guid);
            sqlite3_bind_int64(runtimeObjectReference.Value, 3, reference.LocalId);
            if (sqlite3_step(runtimeObjectReference.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write exact runtime reference");

            sqlite3_reset(runtimeReference.Value);
            sqlite3_clear_bindings(runtimeReference.Value);
            BindGuid(runtimeReference.Value, 1, record.ID);
            BindGuid(runtimeReference.Value, 2, SubAssetPolicy::GetBackingAssetId(reference.Guid, reference.LocalId));
            if (sqlite3_step(runtimeReference.Value) != SQLITE_DONE)
                return FailSql(diagnostic, path, database.Value, "write runtime reference projection");
        }
    }

    StatementHandle observation;
    if (Prepare(database.Value,
        "INSERT INTO file_observations(portability_key,size,mtime_ns,file_id,volume_id,change_ticks,identity_reliable,content_hash,cache_checksum,last_seen_session) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)", observation, diagnostic, path))
        return true;
    for (const SourceHashFileState& state : fileStates)
    {
        sqlite3_reset(observation.Value);
        sqlite3_clear_bindings(observation.Value);
        BindText(observation.Value, 1, state.Path);
        sqlite3_bind_int64(observation.Value, 2, static_cast<int64>(state.Size));
        sqlite3_bind_int64(observation.Value, 3, state.LastWriteTicks);
        sqlite3_bind_blob(observation.Value, 4, &state.FileIdentity, sizeof(state.FileIdentity), SQLITE_TRANSIENT);
        sqlite3_bind_int64(observation.Value, 5, static_cast<int64>(state.VolumeIdentity));
        sqlite3_bind_int64(observation.Value, 6, state.ChangeTicks);
        sqlite3_bind_int(observation.Value, 7, state.IdentityReliable ? 1 : 0);
        sqlite3_bind_blob(observation.Value, 8, state.CachedContentHash.Bytes, sizeof(state.CachedContentHash.Bytes), SQLITE_TRANSIENT);
        sqlite3_bind_int64(observation.Value, 9, state.CacheChecksum);
        sqlite3_bind_int64(observation.Value, 10, static_cast<int64>(snapshot.Revision));
        if (sqlite3_step(observation.Value) != SQLITE_DONE)
            return FailSql(diagnostic, path, database.Value, "write file observation");
    }
    if (Execute(database.Value, "COMMIT;", diagnostic, path))
        return true;
    committed = true;
    return false;
}

bool AssetDatabaseStorage::Load(const StringView& path, const StringView& projectRoot, const StringView& contentRoot,
    AssetDatabase& assetDatabase, Array<SourceHashFileState>& fileStates,
    AssetPipelineDiagnostic& diagnostic, bool readOnly)
{
    diagnostic = AssetPipelineDiagnostic();
    fileStates.Clear();
    if (!FileSystem::FileExists(path))
        return Fail(diagnostic, path, TEXT("Durable asset database does not exist."));

    DatabaseHandle database;
    if ((readOnly ? OpenReadOnly(path, database, diagnostic) : Open(path, database, diagnostic)) ||
        (readOnly ? ValidateCurrentSchema(database.Value, path, diagnostic) : EnsureSchema(database.Value, path, diagnostic)))
        return true;

    StatementHandle integrity;
    if (Prepare(database.Value, "PRAGMA quick_check", integrity, diagnostic, path) || sqlite3_step(integrity.Value) != SQLITE_ROW || ReadText(integrity.Value, 0) != TEXT("ok"))
        return Fail(diagnostic, path, TEXT("Durable asset database integrity check failed."));

    StatementHandle root;
    if (Prepare(database.Value, "SELECT value FROM database_metadata WHERE key=?1", root, diagnostic, path))
        return true;
    const auto readRoot = [&](const char* key, String& result)
    {
        sqlite3_reset(root.Value);
        sqlite3_clear_bindings(root.Value);
        sqlite3_bind_text(root.Value, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(root.Value) != SQLITE_ROW)
            return true;
        result = ReadText(root.Value, 0);
        return false;
    };
    String storedProjectRoot, storedContentRoot, storedMetadataFormat;
    if (readRoot("project_root", storedProjectRoot) || readRoot("content_root", storedContentRoot) ||
        readRoot("metadata_format", storedMetadataFormat) || storedProjectRoot != projectRoot ||
        storedContentRoot != contentRoot || storedMetadataFormat != String::Format(TEXT("{0}"), AssetMeta::CurrentMetaVersion))
        return Fail(diagnostic, path, TEXT("Durable asset database belongs to a different project or source root."));

    const Array<AssetMount> configuredMounts = AssetMountRegistry::GetMounts();
    Dictionary<Guid, AssetMount> mountsById;
    for (const AssetMount& mount : configuredMounts)
        mountsById.Add(mount.MountId, mount);
    StatementHandle storedMounts;
    if (Prepare(database.Value, "SELECT mount_id,logical_prefix,physical_root,kind,writable FROM mounts", storedMounts, diagnostic, path))
        return true;
    int32 storedMountCount = 0;
    int32 mountStep;
    while ((mountStep = sqlite3_step(storedMounts.Value)) == SQLITE_ROW)
    {
        const Guid mountId = ReadGuid(storedMounts.Value, 0);
        const AssetMount* configured = mountsById.TryGet(mountId);
        if (!configured || configured->LogicalPrefix != ReadText(storedMounts.Value, 1) ||
            configured->PhysicalRoot != ReadText(storedMounts.Value, 2) ||
            static_cast<int32>(configured->Kind) != sqlite3_column_int(storedMounts.Value, 3) ||
            configured->Writable != (sqlite3_column_int(storedMounts.Value, 4) != 0))
            return Fail(diagnostic, path, TEXT("Durable asset database mount table differs from the explicit project mount registry."));
        storedMountCount++;
    }
    if (mountStep != SQLITE_DONE || storedMountCount != configuredMounts.Count())
        return Fail(diagnostic, path, TEXT("Durable asset database mount table is incomplete."));

    uint64 snapshotRevision = 0;
    StatementHandle revision;
    if (Prepare(database.Value, "SELECT MAX(last_refresh_session) FROM source_assets", revision, diagnostic, path) ||
        sqlite3_step(revision.Value) != SQLITE_ROW)
        return FailSql(diagnostic, path, database.Value, "read snapshot revision");
    snapshotRevision = static_cast<uint64>(sqlite3_column_int64(revision.Value, 0));
    if (snapshotRevision == 0)
        return Fail(diagnostic, path, TEXT("Durable asset database has no valid snapshot revision."));

    Array<AssetRecord> records;
    Dictionary<Guid, int32> recordByBackingId;
    StatementHandle objects;
    if (Prepare(database.Value,
        "SELECT o.backing_id,o.file_guid,o.local_id,o.type_name,s.logical_path,s.source_path,s.meta_path,o.stable_key,s.importer_id,s.portability_key,s.meta_semantic_hash,s.source_kind,o.status,o.object_revision,o.is_main "
        "FROM asset_objects o JOIN source_assets s ON s.guid=o.file_guid ORDER BY s.logical_path,o.is_main DESC,o.local_id",
        objects, diagnostic, path))
        return true;
    int32 stepResult;
    while ((stepResult = sqlite3_step(objects.Value)) == SQLITE_ROW)
    {
        AssetRecord record;
        record.ID = ReadGuid(objects.Value, 0);
        record.SourceAssetID = ReadGuid(objects.Value, 1);
        record.LocalId = sqlite3_column_int64(objects.Value, 2);
        record.TypeName = ReadText(objects.Value, 3);
        record.CanonicalPath = CanonicalAssetPath(ReadText(objects.Value, 5));
        record.SourcePath = SourceFilePath(ReadText(objects.Value, 5));
        record.MetaPath = MetaFilePath(ReadText(objects.Value, 6));
        const bool isMain = sqlite3_column_int(objects.Value, 14) != 0;
        if (!isMain)
            record.SubAsset = SubAssetKey(ReadText(objects.Value, 7));
        record.ProcessorID = ReadText(objects.Value, 8);
        record.PortabilityKey = ReadText(objects.Value, 9);
        record.MetaSemanticHash = static_cast<uint64>(sqlite3_column_int64(objects.Value, 10));
        record.SourceKind = static_cast<AssetSourceKind>(sqlite3_column_int(objects.Value, 11));
        record.Status = static_cast<AssetRecordStatus>(sqlite3_column_int(objects.Value, 12));
        record.DatabaseRevision = static_cast<uint64>(sqlite3_column_int64(objects.Value, 13));
        if (!record.ID.IsValid() || !record.SourceAssetID.IsValid() || record.LocalId <= 0 || record.CanonicalPath.IsEmpty() || record.SourcePath.IsEmpty())
            return Fail(diagnostic, path, TEXT("Durable asset database contains an invalid object record."));
        recordByBackingId.Add(record.ID, records.Count());
        records.Add(MoveTemp(record));
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read object records");

    StatementHandle labels;
    if (Prepare(database.Value, "SELECT guid,label FROM labels ORDER BY guid,label", labels, diagnostic, path))
        return true;
    while ((stepResult = sqlite3_step(labels.Value)) == SQLITE_ROW)
    {
        const Guid fileGuid = ReadGuid(labels.Value, 0);
        const String value = ReadText(labels.Value, 1);
        for (AssetRecord& record : records)
        {
            if (record.SourceAssetID == fileGuid)
                record.Labels.Add(value);
        }
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read source labels");

    StatementHandle dependencies;
    if (Prepare(database.Value, "SELECT owner_guid,input_guid FROM source_dependencies ORDER BY owner_guid,input_guid", dependencies, diagnostic, path))
        return true;
    while ((stepResult = sqlite3_step(dependencies.Value)) == SQLITE_ROW)
    {
        int32 recordIndex;
        if (recordByBackingId.TryGet(ReadGuid(dependencies.Value, 0), recordIndex))
            records[recordIndex].BuildInputDependencies.Add(ReadGuid(dependencies.Value, 1));
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read source dependencies");

    StatementHandle objectDependencies;
    if (Prepare(database.Value, "SELECT owner_guid,input_file_guid,input_local_id FROM source_object_dependencies ORDER BY owner_guid,input_file_guid,input_local_id", objectDependencies, diagnostic, path))
        return true;
    while ((stepResult = sqlite3_step(objectDependencies.Value)) == SQLITE_ROW)
    {
        int32 recordIndex;
        if (recordByBackingId.TryGet(ReadGuid(objectDependencies.Value, 0), recordIndex))
            records[recordIndex].BuildInputObjectDependencies.Add(AssetObjectId(ReadGuid(objectDependencies.Value, 1), sqlite3_column_int64(objectDependencies.Value, 2)));
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read exact source dependencies");

    StatementHandle references;
    if (Prepare(database.Value, "SELECT owner_guid,referenced_guid FROM runtime_references ORDER BY owner_guid,referenced_guid", references, diagnostic, path))
        return true;
    while ((stepResult = sqlite3_step(references.Value)) == SQLITE_ROW)
    {
        int32 recordIndex;
        if (recordByBackingId.TryGet(ReadGuid(references.Value, 0), recordIndex))
            records[recordIndex].RuntimeReferences.Add(ReadGuid(references.Value, 1));
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read runtime references");

    StatementHandle objectReferences;
    if (Prepare(database.Value, "SELECT owner_guid,referenced_file_guid,referenced_local_id FROM runtime_object_references ORDER BY owner_guid,referenced_file_guid,referenced_local_id", objectReferences, diagnostic, path))
        return true;
    while ((stepResult = sqlite3_step(objectReferences.Value)) == SQLITE_ROW)
    {
        int32 recordIndex;
        if (recordByBackingId.TryGet(ReadGuid(objectReferences.Value, 0), recordIndex))
            records[recordIndex].RuntimeObjectReferences.Add(AssetObjectId(ReadGuid(objectReferences.Value, 1), sqlite3_column_int64(objectReferences.Value, 2)));
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read exact runtime references");

    StatementHandle observations;
    if (Prepare(database.Value,
        "SELECT portability_key,size,mtime_ns,file_id,volume_id,change_ticks,identity_reliable,content_hash,cache_checksum FROM file_observations ORDER BY portability_key",
        observations, diagnostic, path))
        return true;
    while ((stepResult = sqlite3_step(observations.Value)) == SQLITE_ROW)
    {
        SourceHashFileState state;
        state.Path = ReadText(observations.Value, 0);
        state.Size = static_cast<uint64>(sqlite3_column_int64(observations.Value, 1));
        state.LastWriteTicks = sqlite3_column_int64(observations.Value, 2);
        const void* fileIdentity = sqlite3_column_blob(observations.Value, 3);
        if (fileIdentity && sqlite3_column_bytes(observations.Value, 3) == sizeof(state.FileIdentity))
            Platform::MemoryCopy(&state.FileIdentity, fileIdentity, sizeof(state.FileIdentity));
        state.VolumeIdentity = static_cast<uint64>(sqlite3_column_int64(observations.Value, 4));
        state.ChangeTicks = sqlite3_column_int64(observations.Value, 5);
        state.IdentityReliable = sqlite3_column_int(observations.Value, 6) != 0;
        const void* contentHash = sqlite3_column_blob(observations.Value, 7);
        if (contentHash && sqlite3_column_bytes(observations.Value, 7) == sizeof(state.CachedContentHash.Bytes))
            Platform::MemoryCopy(state.CachedContentHash.Bytes, contentHash, sizeof(state.CachedContentHash.Bytes));
        state.CacheChecksum = static_cast<uint32>(sqlite3_column_int64(observations.Value, 8));
        fileStates.Add(MoveTemp(state));
    }
    if (stepResult != SQLITE_DONE)
        return FailSql(diagnostic, path, database.Value, "read file observations");
    if (records.IsEmpty())
        return Fail(diagnostic, path, TEXT("Durable asset database has no committed source records."));
    return assetDatabase.RestoreSnapshot(records, snapshotRevision, diagnostic);
}

bool AssetDatabaseStorage::PublishArtifact(const StringView& libraryRoot, const ArtifactManifest& manifest,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (manifest.Validate(StringView::Empty, diagnostic))
        return true;

    ArtifactKeyBuilder keyBuilder(StringAnsiView("FlaxArtifact/v4"));
    keyBuilder.AddGuid(StringAnsiView("asset"), manifest.AssetID);
    keyBuilder.AddKey(StringAnsiView("desired-input"), manifest.InputFingerprint);
    keyBuilder.AddString(StringAnsiView("importer"), manifest.ProcessorID);
    keyBuilder.AddUInt32(StringAnsiView("importer-version"), manifest.ProcessorImplementationVersion);
    keyBuilder.AddKey(StringAnsiView("target"), manifest.Target.BuildKey(ArtifactTargetDimension::All));
    Array<StringAnsi> dependencyInputs;
    for (const ArtifactManifestDependency& dependency : manifest.Dependencies)
        dependencyInputs.Add(BuildDependencyFingerprint(dependency).ToString());
    keyBuilder.AddSortedStrings(StringAnsiView("dependencies"), dependencyInputs);
    Array<StringAnsi> outputInputs;
    for (const ArtifactManifestOutput& output : manifest.Outputs)
    {
        outputInputs.Add(StringAnsi::Format("{0}|{1}|{2}|{3}|{4}", output.Kind, output.FormatVersion,
            output.Key.ToString(), output.Content.ToString(), output.Size));
    }
    keyBuilder.AddSortedStrings(StringAnsiView("outputs"), outputInputs);
    Array<StringAnsi> objectInputs;
    for (const ArtifactManifestObject& object : manifest.Objects)
    {
        ArtifactKeyBuilder objectBuilder(StringAnsiView("FlaxArtifactObject/v1"));
        objectBuilder.AddGuid(StringAnsiView("file-guid"), object.ObjectID.Guid);
        objectBuilder.AddUInt64(StringAnsiView("local-id"), static_cast<uint64>(object.ObjectID.LocalId));
        objectBuilder.AddGuid(StringAnsiView("backing-guid"), object.BackingAssetID);
        objectBuilder.AddString(StringAnsiView("type"), object.TypeName);
        objectBuilder.AddString(StringAnsiView("name"), object.Name);
        objectBuilder.AddString(StringAnsiView("stable-key"), object.StableKey);
        objectBuilder.AddBool(StringAnsiView("main"), object.IsMainObject);
        objectInputs.Add(objectBuilder.Finalize().ToString());
    }
    keyBuilder.AddSortedStrings(StringAnsiView("objects"), objectInputs);
    const ArtifactKey artifactKey = keyBuilder.Finalize();

    const String keyText(artifactKey.ToString());
    const String immutableDirectory = ArtifactStore::GetArtifactsPath(libraryRoot) / keyText.Substring(0, 2) / keyText;
    const String immutableManifest = immutableDirectory / TEXT("manifest.json");
    if (!FileSystem::DirectoryExists(immutableDirectory) && FileSystem::CreateDirectory(immutableDirectory))
        return Fail(diagnostic, immutableDirectory, TEXT("Cannot create immutable artifact manifest directory."));
    if (!FileSystem::FileExists(immutableManifest))
    {
        StringAnsi manifestJson;
        if (manifest.ToJson(manifestJson, diagnostic))
            return true;
        const String staging = immutableManifest + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { FileSystem::DeleteFile(staging); };
        if (File::WriteAllBytes(staging, manifestJson.Get(), manifestJson.Length()) || FileSystem::MoveFile(immutableManifest, staging, false))
        {
            if (!FileSystem::FileExists(immutableManifest))
                return Fail(diagnostic, immutableManifest, TEXT("Cannot publish immutable artifact manifest."));
        }
    }
    StringAnsi verifiedJson;
    ArtifactManifest verifiedManifest;
    if (File::ReadAllText(immutableManifest, verifiedJson) || ArtifactManifest::Parse(verifiedJson, immutableManifest, verifiedManifest, diagnostic) ||
        verifiedManifest.AssetID != manifest.AssetID || verifiedManifest.InputFingerprint != manifest.InputFingerprint)
        return Fail(diagnostic, immutableManifest, TEXT("Immutable artifact manifest validation failed."));

    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    const String databaseDirectory = StringUtils::GetDirectoryName(databasePath);
    if (!FileSystem::DirectoryExists(databaseDirectory) && FileSystem::CreateDirectory(databaseDirectory))
        return Fail(diagnostic, databaseDirectory, TEXT("Cannot create durable artifact database directory."));
    DatabaseHandle database;
    if (Open(databasePath, database, diagnostic) || EnsureSchema(database.Value, databasePath, diagnostic) ||
        Execute(database.Value, "BEGIN IMMEDIATE;", diagnostic, databasePath))
        return true;
    bool committed = false;
    SCOPE_EXIT
    {
        if (!committed)
            sqlite3_exec(database.Value, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    String relativeManifest;
    if (ArtifactStore::TryMakeLibraryRelative(libraryRoot, immutableManifest, relativeManifest, diagnostic))
        return true;
    const ArtifactKey targetKey = manifest.Target.BuildKey(ArtifactTargetDimension::All);

    StatementHandle artifact;
    if (Prepare(database.Value,
        "INSERT OR IGNORE INTO artifacts(artifact_key,guid,target_key,importer_id,importer_version,source_input_key,manifest_path,status,created_utc,deterministic) VALUES(?1,?2,?3,?4,?5,?6,?7,0,unixepoch(),1)",
        artifact, diagnostic, databasePath))
        return true;
    BindKey(artifact.Value, 1, artifactKey);
    BindGuid(artifact.Value, 2, manifest.AssetID);
    BindKey(artifact.Value, 3, targetKey);
    BindText(artifact.Value, 4, manifest.ProcessorID);
    sqlite3_bind_int64(artifact.Value, 5, manifest.ProcessorImplementationVersion);
    BindKey(artifact.Value, 6, manifest.InputFingerprint);
    BindText(artifact.Value, 7, relativeManifest);
    if (sqlite3_step(artifact.Value) != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "write artifact");

    StatementHandle output;
    if (Prepare(database.Value,
        "INSERT OR REPLACE INTO artifact_outputs(artifact_key,output_kind,output_key,relative_path,content_hash,size,compatibility) VALUES(?1,?2,?3,?4,?5,?6,?7)",
        output, diagnostic, databasePath))
        return true;
    for (const ArtifactManifestOutput& value : manifest.Outputs)
    {
        sqlite3_reset(output.Value);
        sqlite3_clear_bindings(output.Value);
        BindKey(output.Value, 1, artifactKey);
        BindText(output.Value, 2, String(value.Kind));
        BindKey(output.Value, 3, value.Key);
        BindText(output.Value, 4, value.RelativePath);
        BindHash(output.Value, 5, value.Content);
        sqlite3_bind_int64(output.Value, 6, static_cast<int64>(value.Size));
        BindText(output.Value, 7, String(value.Compatibility));
        if (sqlite3_step(output.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database.Value, "write artifact output");
    }
    if (WriteArtifactObjects(database.Value, databasePath, artifactKey, manifest, diagnostic))
        return true;

    StatementHandle clearDependencies;
    if (Prepare(database.Value, "DELETE FROM artifact_dependencies WHERE artifact_key=?1", clearDependencies, diagnostic, databasePath))
        return true;
    BindKey(clearDependencies.Value, 1, artifactKey);
    if (sqlite3_step(clearDependencies.Value) != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "replace artifact dependencies");
    StatementHandle dependency;
    if (Prepare(database.Value,
        "INSERT INTO artifact_dependencies(artifact_key,kind,state,identity,target_guid,target_file_guid,target_local_id,observed_fingerprint,source_hash,metadata_hash,target_artifact_key,interface_hash,interface_version) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
        dependency, diagnostic, databasePath))
        return true;
    for (const ArtifactManifestDependency& value : manifest.Dependencies)
    {
        sqlite3_reset(dependency.Value);
        sqlite3_clear_bindings(dependency.Value);
        BindKey(dependency.Value, 1, artifactKey);
        sqlite3_bind_int(dependency.Value, 2, static_cast<int32>(value.Kind));
        sqlite3_bind_int(dependency.Value, 3, static_cast<int32>(value.State));
        BindText(dependency.Value, 4, value.Identity);
        if (value.AssetID.IsValid())
            BindGuid(dependency.Value, 5, value.AssetID);
        else
            sqlite3_bind_null(dependency.Value, 5);
        if (value.ObjectID.IsValid())
        {
            BindGuid(dependency.Value, 6, value.ObjectID.Guid);
            sqlite3_bind_int64(dependency.Value, 7, value.ObjectID.LocalId);
        }
        else
        {
            sqlite3_bind_null(dependency.Value, 6);
            sqlite3_bind_null(dependency.Value, 7);
        }
        BindKey(dependency.Value, 8, BuildDependencyFingerprint(value));
        if (!value.Hash.IsZero())
            BindHash(dependency.Value, 9, value.Hash);
        else
            sqlite3_bind_null(dependency.Value, 9);
        if (!value.MetadataHash.IsZero())
            BindHash(dependency.Value, 10, value.MetadataHash);
        else
            sqlite3_bind_null(dependency.Value, 10);
        if (value.ExactArtifact.IsZero())
            sqlite3_bind_null(dependency.Value, 11);
        else
            BindKey(dependency.Value, 11, value.ExactArtifact);
        if (!value.InterfaceHash.IsZero())
            BindHash(dependency.Value, 12, value.InterfaceHash);
        else
            sqlite3_bind_null(dependency.Value, 12);
        sqlite3_bind_int64(dependency.Value, 13, value.InterfaceVersion);
        if (sqlite3_step(dependency.Value) != SQLITE_DONE)
            return FailSql(diagnostic, databasePath, database.Value, "write artifact dependency");
    }

    StatementHandle current;
    if (Prepare(database.Value,
        "INSERT INTO current_artifacts(guid,target_key,desired_input_key,current_artifact_key,last_good_artifact_key,import_status,diagnostic_id) VALUES(?1,?2,?3,?4,NULL,0,NULL) "
        "ON CONFLICT(guid,target_key) DO UPDATE SET desired_input_key=excluded.desired_input_key,last_good_artifact_key=COALESCE(current_artifacts.current_artifact_key,current_artifacts.last_good_artifact_key),current_artifact_key=excluded.current_artifact_key,import_status=0,diagnostic_id=NULL",
        current, diagnostic, databasePath))
        return true;
    BindGuid(current.Value, 1, manifest.AssetID);
    BindKey(current.Value, 2, targetKey);
    BindKey(current.Value, 3, manifest.InputFingerprint);
    BindKey(current.Value, 4, artifactKey);
    if (sqlite3_step(current.Value) != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "publish current artifact mapping");

    StatementHandle history;
    if (Prepare(database.Value,
        "INSERT INTO import_history(import_id,guid,target_key,reason_mask,desired_input_key,artifact_key,started_utc,completed_utc,result,log_path) VALUES(?1,?2,?3,0,?4,?5,unixepoch(),unixepoch(),0,'')",
        history, diagnostic, databasePath))
        return true;
    const Guid importID = Guid::New();
    BindGuid(history.Value, 1, importID);
    BindGuid(history.Value, 2, manifest.AssetID);
    BindKey(history.Value, 3, targetKey);
    BindKey(history.Value, 4, manifest.InputFingerprint);
    BindKey(history.Value, 5, artifactKey);
    if (sqlite3_step(history.Value) != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "write import history");
    if (Execute(database.Value, "COMMIT;", diagnostic, databasePath))
        return true;
    committed = true;
    return false;
}

bool AssetDatabaseStorage::PublishArtifacts(const StringView& libraryRoot, const Array<ArtifactManifest>& manifests,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (manifests.IsEmpty())
        return Fail(diagnostic, StringView::Empty, TEXT("Artifact publication batch is empty."));
    Array<PreparedArtifactPublication> prepared;
    prepared.EnsureCapacity(manifests.Count());
    HashSet<String> identities;
    for (const ArtifactManifest& manifest : manifests)
    {
        const String identity = manifest.AssetID.ToString(Guid::FormatType::N) + TEXT("/") +
            String(manifest.Target.BuildKey(ArtifactTargetDimension::All).ToString());
        if (!identities.Add(identity))
            return Fail(diagnostic, identity, TEXT("Artifact publication batch repeats an asset and target mapping."));
        PreparedArtifactPublication value;
        if (PrepareArtifactPublication(libraryRoot, manifest, value, diagnostic))
            return true;
        prepared.Add(MoveTemp(value));
    }

    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    const String databaseDirectory = StringUtils::GetDirectoryName(databasePath);
    if (!FileSystem::DirectoryExists(databaseDirectory) && FileSystem::CreateDirectory(databaseDirectory))
        return Fail(diagnostic, databaseDirectory, TEXT("Cannot create durable artifact database directory."));
    DatabaseHandle database;
    if (Open(databasePath, database, diagnostic) || EnsureSchema(database.Value, databasePath, diagnostic) ||
        Execute(database.Value, "BEGIN IMMEDIATE;", diagnostic, databasePath))
        return true;
    bool committed = false;
    SCOPE_EXIT
    {
        if (!committed)
            sqlite3_exec(database.Value, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    for (const PreparedArtifactPublication& value : prepared)
    {
        if (WriteArtifactPublication(database.Value, databasePath, value, diagnostic))
            return true;
    }
    if (Execute(database.Value, "COMMIT;", diagnostic, databasePath))
        return true;
    committed = true;
    return false;
}

bool AssetDatabaseStorage::GetCurrentArtifactManifest(const StringView& libraryRoot, const Guid& assetID,
    const ArtifactKey& targetKey, String& manifestPath, AssetPipelineDiagnostic& diagnostic)
{
    manifestPath.Clear();
    diagnostic = AssetPipelineDiagnostic();
    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    if (!FileSystem::FileExists(databasePath))
        return false;
    DatabaseHandle database;
    if (OpenReadOnly(databasePath, database, diagnostic) || ValidateCurrentSchema(database.Value, databasePath, diagnostic))
        return true;
    StatementHandle query;
    if (Prepare(database.Value,
        "SELECT a.manifest_path FROM current_artifacts c JOIN artifacts a ON a.artifact_key=c.current_artifact_key WHERE c.guid=?1 AND c.target_key=?2",
        query, diagnostic, databasePath))
        return true;
    BindGuid(query.Value, 1, assetID);
    BindKey(query.Value, 2, targetKey);
    const int32 result = sqlite3_step(query.Value);
    if (result == SQLITE_DONE)
        return false;
    if (result != SQLITE_ROW)
        return FailSql(diagnostic, databasePath, database.Value, "read current artifact mapping");
    const String relativePath = ReadText(query.Value, 0);
    ArtifactStoragePath absolutePath;
    if (ArtifactStore::TryResolveLibraryRelative(libraryRoot, relativePath, absolutePath, diagnostic))
        return true;
    manifestPath = absolutePath.Get();
    return false;
}

bool AssetDatabaseStorage::GetReachableArtifactManifests(const StringView& libraryRoot, Array<String>& manifestPaths,
    AssetPipelineDiagnostic& diagnostic)
{
    manifestPaths.Clear();
    diagnostic = AssetPipelineDiagnostic();
    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    if (!FileSystem::FileExists(databasePath))
        return Fail(diagnostic, databasePath, TEXT("Artifact garbage collection requires the durable asset database."));
    DatabaseHandle database;
    if (OpenReadOnly(databasePath, database, diagnostic) || ValidateCurrentSchema(database.Value, databasePath, diagnostic))
        return true;
    StatementHandle query;
    if (Prepare(database.Value,
        "SELECT DISTINCT a.manifest_path FROM artifacts a JOIN ("
        "SELECT current_artifact_key AS artifact_key FROM current_artifacts WHERE current_artifact_key IS NOT NULL "
        "UNION SELECT last_good_artifact_key FROM current_artifacts WHERE last_good_artifact_key IS NOT NULL"
        ") retained ON retained.artifact_key=a.artifact_key ORDER BY a.manifest_path",
        query, diagnostic, databasePath))
        return true;
    int32 result;
    while ((result = sqlite3_step(query.Value)) == SQLITE_ROW)
    {
        ArtifactStoragePath absolutePath;
        if (ArtifactStore::TryResolveLibraryRelative(libraryRoot, ReadText(query.Value, 0), absolutePath, diagnostic))
            return true;
        manifestPaths.Add(absolutePath.Get());
    }
    if (result != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "read retained artifact mappings");
    return false;
}

bool AssetDatabaseStorage::RegisterCustomDependency(const StringView& libraryRoot, const StringView& name,
    const Guid& hash, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (name.IsEmpty() || !hash.IsValid())
        return Fail(diagnostic, name, TEXT("Custom dependency name and hash must be valid."));
    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    DatabaseHandle database;
    if (Open(databasePath, database, diagnostic) || EnsureSchema(database.Value, databasePath, diagnostic))
        return true;
    StatementHandle statement;
    if (Prepare(database.Value,
        "INSERT INTO custom_dependencies(name,hash,revision) VALUES(?1,?2,1) "
        "ON CONFLICT(name) DO UPDATE SET hash=excluded.hash,revision=custom_dependencies.revision+1 WHERE custom_dependencies.hash<>excluded.hash",
        statement, diagnostic, databasePath))
        return true;
    BindText(statement.Value, 1, name);
    BindGuid(statement.Value, 2, hash);
    if (sqlite3_step(statement.Value) != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "register custom dependency");
    return false;
}

bool AssetDatabaseStorage::GetCustomDependency(const StringView& libraryRoot, const StringView& name,
    Guid& hash, uint64& revision, bool& found, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    hash = Guid::Empty;
    revision = 0;
    found = false;
    if (name.IsEmpty())
        return Fail(diagnostic, name, TEXT("Custom dependency name must not be empty."));
    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    if (!FileSystem::FileExists(databasePath))
        return false;
    DatabaseHandle database;
    if (OpenReadOnly(databasePath, database, diagnostic) || ValidateCurrentSchema(database.Value, databasePath, diagnostic))
        return true;
    StatementHandle statement;
    if (Prepare(database.Value, "SELECT hash,revision FROM custom_dependencies WHERE name=?1", statement, diagnostic, databasePath))
        return true;
    BindText(statement.Value, 1, name);
    const int32 result = sqlite3_step(statement.Value);
    if (result == SQLITE_DONE)
        return false;
    if (result != SQLITE_ROW)
        return FailSql(diagnostic, databasePath, database.Value, "read custom dependency");
    hash = ReadGuid(statement.Value, 0);
    revision = static_cast<uint64>(sqlite3_column_int64(statement.Value, 1));
    found = hash.IsValid() && revision != 0;
    return false;
}

bool AssetDatabaseStorage::UnregisterCustomDependencyPrefix(const StringView& libraryRoot, const StringView& prefix,
    AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    if (prefix.IsEmpty())
        return Fail(diagnostic, prefix, TEXT("Custom dependency prefix must not be empty."));
    const String databasePath = String(libraryRoot) / TEXT("AssetDatabase/AssetDatabase.sqlite");
    if (!FileSystem::FileExists(databasePath))
        return false;
    DatabaseHandle database;
    if (Open(databasePath, database, diagnostic) || EnsureSchema(database.Value, databasePath, diagnostic))
        return true;
    StatementHandle statement;
    if (Prepare(database.Value, "DELETE FROM custom_dependencies WHERE substr(name,1,length(?1))=?1", statement, diagnostic, databasePath))
        return true;
    BindText(statement.Value, 1, prefix);
    if (sqlite3_step(statement.Value) != SQLITE_DONE)
        return FailSql(diagnostic, databasePath, database.Value, "unregister custom dependencies");
    return false;
}
