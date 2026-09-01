// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/SourceAssetDatabase.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Utilities/Crc.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
#pragma pack(push, 1)
    struct TestNormalizedManifest
    {
        uint32 Magic;
        uint32 Version;
        uint32 SchemaVersion;
        Guid ProjectId;
        uint64 Generation;
        uint64 Revision;
        uint32 Crc;
    };

    struct TestNormalizedTableHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 Table;
        uint64 Generation;
        uint64 Revision;
        uint32 PayloadSize;
        uint32 PayloadCrc;
    };
#pragma pack(pop)

    SourceAssetRow MakeSource(const Guid& guid, const StringView& path)
    {
        SourceAssetRow result;
        result.AssetGuid = guid;
        result.Path = path;
        result.CanonicalPath = String(path).ToLower();
        result.MetaPath = String(path) + TEXT(".meta");
        result.CanonicalMetaPath = result.MetaPath.ToLower();
        result.ImporterId = TEXT("Flax.Test");
        return result;
    }

    SourceAssetObjectRow MakeObject(const Guid& assetGuid, int64 localFileId, const StringView& stableIdentifier)
    {
        SourceAssetObjectRow result;
        result.AssetGuid = assetGuid;
        result.LocalFileId = localFileId;
        result.StableIdentifier = stableIdentifier;
        result.TypeName = TEXT("FlaxEngine.Model");
        result.IsMain = localFileId == 1;
        return result;
    }

    bool FileBytesEqual(const StringView& path, const Array<byte>& expected)
    {
        Array<byte> current;
        return !File::ReadAllBytes(path, current) && current.Count() == expected.Count() &&
               (!current.HasItems() || Platform::MemoryCompare(current.Get(), expected.Get(), current.Count()) == 0);
    }
}

TEST_CASE("Source asset database schema persists exact composite object identity")
{
    CHECK(AssetDatabaseSchema::Version == 5);

    SourceAssetDatabaseState state;
    state.Database.ProjectId = Guid::New();
    const Guid firstSource(101, 102, 103, 104);
    const int64 firstLocalId = 2;
    const Guid compatibilityCollision = AssetObjectId(AssetGuid(firstSource), firstLocalId).ToRuntimeObjectGuid();
    REQUIRE(compatibilityCollision.IsValid());
    REQUIRE(compatibilityCollision != firstSource);
    CHECK(AssetObjectId(AssetGuid(compatibilityCollision), 1).ToRuntimeObjectGuid() == compatibilityCollision);

    state.Sources.Add(MakeSource(firstSource, TEXT("Content/First.fbx")));
    state.Sources.Add(MakeSource(compatibilityCollision, TEXT("Content/Second.fbx")));
    state.Objects.Add(MakeObject(firstSource, firstLocalId, TEXT("mesh:/First")));
    state.Objects.Add(MakeObject(compatibilityCollision, 1, TEXT("main")));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(state.Validate(diagnostic));

    Array<byte> serialized;
    state.Serialize(serialized);
    SourceAssetDatabaseState loaded;
    REQUIRE_FALSE(SourceAssetDatabaseState::Deserialize(serialized.Get(), serialized.Count(), loaded, diagnostic));
    REQUIRE(loaded.Objects.Count() == 2);
    CHECK(loaded.Objects[0].AssetGuid == firstSource);
    CHECK(loaded.Objects[0].LocalFileId == firstLocalId);
    CHECK(loaded.Objects[1].AssetGuid == compatibilityCollision);
    CHECK(loaded.Objects[1].LocalFileId == 1);

    state.Database.SchemaVersion = 4;
    state.Serialize(serialized);
    CHECK(SourceAssetDatabaseState::Deserialize(serialized.Get(), serialized.Count(), loaded, diagnostic));
}

TEST_CASE("Source asset database preserves foreign and future normalized stores")
{
    const String root = Globals::TemporaryFolder / (TEXT("SourceAssetDatabaseRejectedStore-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    const String content = root / TEXT("Content");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String authoredPath = content / TEXT("Authored.asset");
    const byte authoredBytes[] = { 1, 2, 3, 4 };
    REQUIRE_FALSE(File::WriteAllBytes(authoredPath, authoredBytes, ARRAY_COUNT(authoredBytes)));

    const Guid projectId = Guid::New();
    const Guid assetId = Guid::New();
    AssetPipelineDiagnostic diagnostic;
    SourceAssetDatabase database;
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    std::unique_ptr<AssetDatabaseTransaction> transaction = database.BeginTransaction();
    REQUIRE(transaction);
    transaction->UpsertSource(MakeSource(assetId, TEXT("Content/Authored.asset")));
    REQUIRE_FALSE(transaction->Commit(diagnostic));
    REQUIRE_FALSE(database.Close(&diagnostic));

    const String databaseDirectory = library / TEXT("AssetDatabase");
    const String manifestPath = databaseDirectory / TEXT("normalized-store.manifest");
    Array<byte> originalManifest;
    REQUIRE_FALSE(File::ReadAllBytes(manifestPath, originalManifest));
    Array<String> tablePaths;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(tablePaths, databaseDirectory, TEXT("*.table")));
    REQUIRE(tablePaths.HasItems());
    Array<Array<byte>> originalTables;
    originalTables.Resize(tablePaths.Count());
    for (int32 i = 0; i < tablePaths.Count(); i++)
        REQUIRE_FALSE(File::ReadAllBytes(tablePaths[i], originalTables[i]));

    CHECK(database.Open(library, Guid::New(), diagnostic));
    CHECK(diagnostic.Message.Contains(TEXT("another project")));
    CHECK(FileBytesEqual(manifestPath, originalManifest));
    for (int32 i = 0; i < tablePaths.Count(); i++)
        CHECK(FileBytesEqual(tablePaths[i], originalTables[i]));
    CHECK(FileSystem::FileExists(authoredPath));

    REQUIRE(originalManifest.Count() == sizeof(TestNormalizedManifest));
    TestNormalizedManifest futureManifest;
    Platform::MemoryCopy(&futureManifest, originalManifest.Get(), sizeof(futureManifest));
    futureManifest.SchemaVersion = AssetDatabaseSchema::Version + 1;
    futureManifest.Crc = Crc::MemCrc32(&futureManifest, sizeof(futureManifest) - sizeof(futureManifest.Crc));
    Array<byte> futureBytes;
    futureBytes.Resize(sizeof(futureManifest), false);
    Platform::MemoryCopy(futureBytes.Get(), &futureManifest, sizeof(futureManifest));
    REQUIRE_FALSE(File::WriteAllBytes(manifestPath, futureBytes.Get(), futureBytes.Count()));

    CHECK(database.Open(library, projectId, diagnostic));
    CHECK(diagnostic.Message.Contains(TEXT("newer unsupported format")));
    CHECK(FileBytesEqual(manifestPath, futureBytes));
    for (int32 i = 0; i < tablePaths.Count(); i++)
        CHECK(FileBytesEqual(tablePaths[i], originalTables[i]));
    CHECK(FileSystem::FileExists(authoredPath));

    REQUIRE_FALSE(File::WriteAllBytes(manifestPath, originalManifest.Get(), originalManifest.Count()));
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    SourceAssetRow source;
    CHECK(database.Read().TryGetSource(assetId, source));
    CHECK(source.AssetGuid == assetId);
    REQUIRE_FALSE(database.Close(&diagnostic));
}

TEST_CASE("Source asset database rebuilds an incompatible normalized cache")
{
    const String root = Globals::TemporaryFolder / (TEXT("SourceAssetDatabaseSchemaRebuild-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    const String content = root / TEXT("Content");
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String authoredPath = content / TEXT("Authored.asset");
    const String unrelatedLibraryPath = library / TEXT("unrelated.cache");
    const byte authoredBytes[] = { 1, 2, 3, 4 };
    const byte unrelatedBytes[] = { 5, 6, 7, 8 };
    REQUIRE_FALSE(File::WriteAllBytes(authoredPath, authoredBytes, ARRAY_COUNT(authoredBytes)));
    REQUIRE_FALSE(File::WriteAllBytes(unrelatedLibraryPath, unrelatedBytes, ARRAY_COUNT(unrelatedBytes)));

    const Guid projectId = Guid::New();
    const Guid assetId = Guid::New();
    AssetPipelineDiagnostic diagnostic;
    SourceAssetDatabase database;
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    std::unique_ptr<AssetDatabaseTransaction> transaction = database.BeginTransaction();
    REQUIRE(transaction);
    transaction->UpsertSource(MakeSource(assetId, TEXT("Content/Authored.asset")));
    REQUIRE_FALSE(transaction->Commit(diagnostic));
    REQUIRE_FALSE(database.Close(&diagnostic));

    const String databaseDirectory = library / TEXT("AssetDatabase");
    Array<String> tablePaths;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(tablePaths, databaseDirectory, TEXT("*.table")));
    REQUIRE(tablePaths.HasItems());
    for (const String& tablePath : tablePaths)
    {
        Array<byte> bytes;
        REQUIRE_FALSE(File::ReadAllBytes(tablePath, bytes));
        REQUIRE(bytes.Count() >= sizeof(TestNormalizedTableHeader) + sizeof(uint32));
        TestNormalizedTableHeader header;
        Platform::MemoryCopy(&header, bytes.Get(), sizeof(header));
        uint32 schemaVersion;
        Platform::MemoryCopy(&schemaVersion, bytes.Get() + sizeof(header), sizeof(schemaVersion));
        REQUIRE(schemaVersion == AssetDatabaseSchema::Version);
        schemaVersion = AssetDatabaseSchema::Version - 1;
        Platform::MemoryCopy(bytes.Get() + sizeof(header), &schemaVersion, sizeof(schemaVersion));
        header.PayloadCrc = Crc::MemCrc32(bytes.Get() + sizeof(header), header.PayloadSize);
        Platform::MemoryCopy(bytes.Get(), &header, sizeof(header));
        REQUIRE_FALSE(File::WriteAllBytes(tablePath, bytes.Get(), bytes.Count()));
    }

    const String manifestPath = databaseDirectory / TEXT("normalized-store.manifest");
    Array<byte> manifestBytes;
    REQUIRE_FALSE(File::ReadAllBytes(manifestPath, manifestBytes));
    REQUIRE(manifestBytes.Count() == sizeof(TestNormalizedManifest));
    TestNormalizedManifest manifest;
    Platform::MemoryCopy(&manifest, manifestBytes.Get(), sizeof(manifest));
    manifest.SchemaVersion = AssetDatabaseSchema::Version - 1;
    manifest.Crc = Crc::MemCrc32(&manifest, sizeof(manifest) - sizeof(manifest.Crc));
    Platform::MemoryCopy(manifestBytes.Get(), &manifest, sizeof(manifest));
    REQUIRE_FALSE(File::WriteAllBytes(manifestPath, manifestBytes.Get(), manifestBytes.Count()));

    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    CHECK_FALSE(database.WasLastShutdownClean());
    CHECK(database.GetRevision() == 0);
    CHECK(database.Read().GetState().Sources.IsEmpty());
    CHECK(FileSystem::FileExists(authoredPath));
    CHECK(FileSystem::FileExists(unrelatedLibraryPath));
    REQUIRE_FALSE(database.Close(&diagnostic));

    tablePaths.Clear();
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(tablePaths, databaseDirectory, TEXT("*.table")));
    REQUIRE(tablePaths.HasItems());
    Array<byte> corruptTable;
    REQUIRE_FALSE(File::ReadAllBytes(tablePaths[0], corruptTable));
    REQUIRE(corruptTable.HasItems());
    corruptTable[corruptTable.Count() - 1] ^= 0xff;
    REQUIRE_FALSE(File::WriteAllBytes(tablePaths[0], corruptTable.Get(), corruptTable.Count()));
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    CHECK(database.GetRevision() == 0);
    CHECK(database.Read().GetState().Sources.IsEmpty());
    CHECK(FileSystem::FileExists(authoredPath));
    CHECK(FileSystem::FileExists(unrelatedLibraryPath));
    REQUIRE_FALSE(database.Close(&diagnostic));
}

TEST_CASE("Source asset database commits durable coherent revisions")
{
    const String library = Globals::TemporaryFolder / (TEXT("SourceAssetDatabase-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(library, true); };
    const Guid projectId(1, 2, 3, 4);
    const Guid assetId(5, 6, 7, 8);
    AssetPipelineDiagnostic diagnostic;

    SourceAssetDatabase database;
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    const AssetDatabaseReadSnapshot revisionZero = database.Read();
    REQUIRE(revisionZero.IsValid());
    CHECK(revisionZero.GetRevision() == 0);

    std::unique_ptr<AssetDatabaseTransaction> transaction = database.BeginTransaction();
    REQUIRE(transaction);
    transaction->UpsertSource(MakeSource(assetId, TEXT("Content/Body.glb")));
    Array<SourceAssetObjectRow> objects;
    objects.Add(MakeObject(assetId, 1, TEXT("main")));
    transaction->ReplaceObjects(assetId, objects);
    SourceAssetPublicationRow publication;
    publication.AssetGuid = assetId;
    publication.LocalFileId = 1;
    publication.TargetId = TEXT("Windows-x64");
    publication.IsLastKnownGood = true;
    transaction->UpsertPublication(publication);
    REQUIRE_FALSE(transaction->Commit(diagnostic));
    CHECK(database.GetRevision() == 1);
    CHECK(revisionZero.GetRevision() == 0);
    CHECK(revisionZero.GetState().Sources.IsEmpty());

    AssetDatabaseReadSnapshot revisionOne = database.Read();
    SourceAssetRow found;
    REQUIRE(revisionOne.TryGetSource(assetId, found));
    CHECK(found.LastModifiedRevision == 1);
    REQUIRE_FALSE(database.Close(&diagnostic));

    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    CHECK(database.WasLastShutdownClean());
    CHECK(database.GetRevision() == 1);
    Array<AssetChangeSet> changes;
    bool requiresSnapshot;
    REQUIRE_FALSE(database.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    CHECK_FALSE(requiresSnapshot);
    REQUIRE(changes.Count() == 1);
    CHECK(changes[0].Revision == 1);
    CHECK(changes[0].Added.Count() == 1);
}

TEST_CASE("Source asset database checkpoints bounded WAL transactions")
{
    const String library = Globals::TemporaryFolder / (TEXT("SourceAssetDatabaseCheckpoint-") + Guid::New().ToString(Guid::FormatType::N));
    const String recoveryLibrary = library + TEXT("-Recovery");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    REQUIRE_FALSE(FileSystem::CreateDirectory(recoveryLibrary));
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(library, true);
        FileSystem::DeleteDirectory(recoveryLibrary, true);
    };

    AssetPipelineDiagnostic diagnostic;
    SourceAssetDatabase database;
    const Guid projectId = Guid::New();
    SourceAssetDatabaseCheckpointPolicy policy;
    policy.MaximumWalBytes = 0;
    policy.MaximumTransactions = 2;
    policy.MaximumElapsedSeconds = 0.0;
    database.SetCheckpointPolicy(policy);
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));

    const String databaseDirectory = database.GetDirectory();
    const String walPath = databaseDirectory / TEXT("normalized-store.wal");
    const uint64 emptyWalSize = FileSystem::GetFileSize(walPath);
    const auto commitSource = [&](const Guid& id, const Char* path)
    {
        std::unique_ptr<AssetDatabaseTransaction> transaction = database.BeginTransaction();
        if (!transaction)
            return true;
        transaction->UpsertSource(MakeSource(id, path));
        return transaction->Commit(diagnostic);
    };

    REQUIRE_FALSE(commitSource(Guid(101, 102, 103, 104), TEXT("Content/A.png")));
    CHECK(FileSystem::GetFileSize(walPath) > emptyWalSize);
    REQUIRE_FALSE(commitSource(Guid(111, 112, 113, 114), TEXT("Content/B.png")));
    CHECK(database.GetRevision() == 2);
    CHECK(FileSystem::GetFileSize(walPath) == emptyWalSize);

    Array<AssetChangeSet> changes;
    bool requiresSnapshot = false;
    REQUIRE_FALSE(database.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    CHECK_FALSE(requiresSnapshot);
    CHECK(changes.Count() == 2);

    const String recoveryDatabaseDirectory = recoveryLibrary / TEXT("AssetDatabase");
    REQUIRE_FALSE(FileSystem::CreateDirectory(recoveryDatabaseDirectory));
    Array<String> checkpointFiles;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(checkpointFiles, databaseDirectory));
    for (const String& file : checkpointFiles)
    {
        if (file.EndsWith(TEXT("writer.lock")))
            continue;
        REQUIRE_FALSE(FileSystem::CopyFile(recoveryDatabaseDirectory / StringUtils::GetFileName(file), file));
    }
    SourceAssetDatabase recovered;
    REQUIRE_FALSE(recovered.Open(recoveryLibrary, projectId, diagnostic));
    CHECK_FALSE(recovered.WasLastShutdownClean());
    CHECK(recovered.GetRevision() == 2);
    changes.Clear();
    REQUIRE_FALSE(recovered.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    CHECK_FALSE(requiresSnapshot);
    CHECK(changes.Count() == 2);
    REQUIRE_FALSE(recovered.Close(&diagnostic));

    policy.MaximumTransactions = 0;
    policy.MaximumWalBytes = emptyWalSize + 1;
    database.SetCheckpointPolicy(policy);
    REQUIRE_FALSE(commitSource(Guid(121, 122, 123, 124), TEXT("Content/C.png")));
    CHECK(FileSystem::GetFileSize(walPath) == emptyWalSize);

    policy.MaximumWalBytes = 0;
    database.SetCheckpointPolicy(policy);
    REQUIRE_FALSE(commitSource(Guid(131, 132, 133, 134), TEXT("Content/D.png")));
    CHECK(FileSystem::GetFileSize(walPath) > emptyWalSize);
    REQUIRE_FALSE(database.Checkpoint(diagnostic));
    CHECK(FileSystem::GetFileSize(walPath) == emptyWalSize);
    REQUIRE_FALSE(database.Close(&diagnostic));

    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    CHECK(database.WasLastShutdownClean());
    CHECK(database.GetRevision() == 4);
    changes.Clear();
    REQUIRE_FALSE(database.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    CHECK_FALSE(requiresSnapshot);
    CHECK(changes.Count() == 4);
    REQUIRE_FALSE(database.Close(&diagnostic));
}

TEST_CASE("Source asset database rejects stale writers and recovers a torn journal tail")
{
    const String library = Globals::TemporaryFolder / (TEXT("SourceAssetDatabaseRecovery-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(library, true); };
    const Guid projectId(11, 12, 13, 14);
    AssetPipelineDiagnostic diagnostic;
    SourceAssetDatabase database;
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    std::unique_ptr<AssetDatabaseTransaction> first = database.BeginTransaction();
    std::unique_ptr<AssetDatabaseTransaction> stale = database.BeginTransaction();
    REQUIRE(first);
    REQUIRE(stale);
    first->UpsertSource(MakeSource(Guid(21, 22, 23, 24), TEXT("Content/A.png")));
    REQUIRE_FALSE(first->Commit(diagnostic));
    stale->UpsertSource(MakeSource(Guid(31, 32, 33, 34), TEXT("Content/B.png")));
    CHECK(stale->Commit(diagnostic));
    CHECK(database.GetRevision() == 1);
    REQUIRE_FALSE(database.Close(&diagnostic));

    const String journal = library / TEXT("AssetDatabase/source-changes.log");
    File* file = File::Open(journal, FileMode::OpenExisting, FileAccess::Write, FileShare::None);
    REQUIRE(file != nullptr);
    file->SetPosition(file->GetSize());
    const uint32 tornFrame = 0x46434146;
    REQUIRE_FALSE(file->Write(&tornFrame, sizeof(tornFrame)));
    Delete(file);
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    CHECK(database.GetRevision() == 1);
    Array<AssetChangeSet> changes;
    bool requiresSnapshot;
    REQUIRE_FALSE(database.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 1);
    CHECK(changes[0].Revision == 1);
}

TEST_CASE("Source asset database object replacement prunes related rows and replays incrementally")
{
    const String root = Globals::TemporaryFolder / (TEXT("SourceAssetDatabaseObjectPrune-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    const String recoveryLibrary = root / TEXT("RecoveryLibrary");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    REQUIRE_FALSE(FileSystem::CreateDirectory(recoveryLibrary));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid projectId = Guid::New();
    const Guid assetId = Guid::New();
    const ArtifactKey mainArtifact(ContentHash::Compute("main-artifact", 13));
    const ArtifactKey subArtifact(ContentHash::Compute("sub-artifact", 12));
    AssetPipelineDiagnostic diagnostic;
    SourceAssetDatabase database;
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));

    std::unique_ptr<AssetDatabaseTransaction> seed = database.BeginTransaction();
    REQUIRE(seed);
    seed->UpsertSource(MakeSource(assetId, TEXT("Content/Body.fbx")));
    Array<SourceAssetObjectRow> objects;
    objects.Add(MakeObject(assetId, 1, TEXT("main")));
    objects.Add(MakeObject(assetId, 2, TEXT("mesh:/Body")));
    seed->ReplaceObjects(assetId, objects);
    SourceAssetDependencyRow dependency;
    dependency.OwnerAssetGuid = assetId;
    dependency.OwnerLocalFileId = 1;
    dependency.TargetId = TEXT("Windows-x64");
    dependency.Kind = AssetDependencyKind::RuntimeReference;
    dependency.TargetAssetGuid = assetId;
    dependency.TargetLocalFileId = 2;
    dependency.CustomDependency = AssetObjectId(AssetGuid(assetId), 2).ToString();
    Array<SourceAssetDependencyRow> dependencies;
    dependencies.Add(dependency);
    seed->ReplaceDependencies(assetId, 1, dependency.TargetId, dependencies);
    SourceAssetPublicationRow publication;
    publication.AssetGuid = assetId;
    publication.LocalFileId = 1;
    publication.TargetId = dependency.TargetId;
    publication.Artifact = mainArtifact;
    publication.IsLastKnownGood = true;
    seed->UpsertPublication(publication);
    publication.LocalFileId = 2;
    publication.Artifact = subArtifact;
    seed->UpsertPublication(publication);
    SourceArtifactObjectRow artifactObject;
    artifactObject.AssetGuid = assetId;
    artifactObject.LocalFileId = 1;
    artifactObject.TypeName = TEXT("FlaxEngine.Model");
    artifactObject.ObjectBlobId = ContentHash::Compute("main-blob", 9);
    Array<SourceArtifactObjectRow> artifactObjects;
    artifactObjects.Add(artifactObject);
    seed->ReplaceArtifactObjects(mainArtifact, artifactObjects);
    artifactObject.LocalFileId = 2;
    artifactObject.ObjectBlobId = ContentHash::Compute("sub-blob", 8);
    artifactObjects[0] = artifactObject;
    seed->ReplaceArtifactObjects(subArtifact, artifactObjects);
    SourceImportAttemptRow attempt;
    attempt.AttemptId = Guid::New();
    attempt.AssetGuid = assetId;
    attempt.TargetId = dependency.TargetId;
    attempt.Status = TEXT("Succeeded");
    seed->UpsertImportAttempt(attempt);
    REQUIRE_FALSE(seed->Commit(diagnostic));

    std::unique_ptr<AssetDatabaseTransaction> unrelated = database.BeginTransaction();
    REQUIRE(unrelated);
    Array<String> labels;
    labels.Add(TEXT("Character"));
    unrelated->SetLabels(assetId, labels);
    REQUIRE_FALSE(unrelated->Commit(diagnostic));
    AssetDatabaseReadSnapshot retained = database.Read();
    CHECK(retained.GetState().Publications.Count() == 2);
    CHECK(retained.GetState().ArtifactObjects.Count() == 2);
    CHECK(retained.GetState().ImportAttempts.Count() == 1);
    CHECK(retained.GetState().Dependencies.Count() == 1);

    std::unique_ptr<AssetDatabaseTransaction> prune = database.BeginTransaction();
    REQUIRE(prune);
    objects.RemoveAt(1);
    prune->ReplaceObjects(assetId, objects);
    REQUIRE_FALSE(prune->Commit(diagnostic));
    retained = database.Read();
    REQUIRE(retained.GetState().Publications.Count() == 1);
    CHECK(retained.GetState().Publications[0].LocalFileId == 1);
    REQUIRE(retained.GetState().ArtifactObjects.Count() == 1);
    CHECK(retained.GetState().ArtifactObjects[0].LocalFileId == 1);
    CHECK(retained.GetState().ImportAttempts.Count() == 1);
    CHECK(retained.GetState().Dependencies.IsEmpty());

    const String sourceDirectory = library / TEXT("AssetDatabase");
    const String recoveryDirectory = recoveryLibrary / TEXT("AssetDatabase");
    REQUIRE_FALSE(FileSystem::CreateDirectory(recoveryDirectory));
    Array<String> files;
    FileSystem::DirectoryGetFiles(files, sourceDirectory);
    for (const String& file : files)
    {
        if (file.EndsWith(TEXT("writer.lock")) || file.EndsWith(TEXT("source-changes.log")))
            continue;
        REQUIRE_FALSE(FileSystem::CopyFile(recoveryDirectory / StringUtils::GetFileName(file), file));
    }
    SourceAssetDatabase recovered;
    REQUIRE_FALSE(recovered.Open(recoveryLibrary, projectId, diagnostic));
    CHECK(recovered.GetRevision() == database.GetRevision());
    const AssetDatabaseReadSnapshot replayed = recovered.Read();
    CHECK(replayed.GetState().Objects.Count() == 1);
    CHECK(replayed.GetState().Publications.Count() == 1);
    CHECK(replayed.GetState().ArtifactObjects.Count() == 1);
    CHECK(replayed.GetState().ImportAttempts.Count() == 1);
    CHECK(replayed.GetState().Dependencies.IsEmpty());
    REQUIRE_FALSE(recovered.Close(&diagnostic));
    REQUIRE_FALSE(database.Close(&diagnostic));
}
