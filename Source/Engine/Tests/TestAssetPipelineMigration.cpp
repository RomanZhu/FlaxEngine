// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/MigrationInventory.h"
#include "Engine/Content/AssetDatabase/MigrationJournal.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    AssetRecord MakeRecord(const Guid& id, const String& typeName, const String& path, AssetSourceKind kind, AssetRecordStatus status = AssetRecordStatus::Ready)
    {
        AssetRecord record;
        record.ID = id;
        record.SourceAssetID = id;
        record.TypeName = typeName;
        record.CanonicalPath = CanonicalAssetPath(path);
        record.SourcePath = SourceFilePath(path);
        record.SourceKind = kind;
        record.Status = status;
        return record;
    }
}

TEST_CASE("Migration inventory is read-only, deterministic, and classifies mixed mode")
{
    Array<AssetRecord> records;
    records.Add(MakeRecord(Guid(2, 0, 0, 0), TEXT("FlaxEngine.Texture"), TEXT("Content/Tex.flax"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(1, 0, 0, 0), TEXT("FlaxEngine.Material"), TEXT("Content/Mat.flax"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(3, 0, 0, 0), TEXT("FlaxEngine.Material"), TEXT("Content/Done.material"), AssetSourceKind::TextDocument));
    records.Add(MakeRecord(Guid(4, 0, 0, 0), TEXT("FlaxEngine.Scene"), TEXT("Content/Level.scene"), AssetSourceKind::LegacyBinary));
    records.Add(MakeRecord(Guid(5, 0, 0, 0), TEXT("FlaxEngine.Material"), TEXT("Content/Dup.flax"), AssetSourceKind::LegacyBinary, AssetRecordStatus::DuplicateGuid));

    Array<MigrationInventoryEntry> entries;
    MigrationInventory::Build(records, entries);
    REQUIRE(entries.Count() == 5);
    CHECK(entries[0].ID == Guid(1, 0, 0, 0));
    CHECK(entries[0].Eligibility == TEXT("ReadyToMigrate"));
    CHECK(entries[0].ProposedDestination.EndsWith(TEXT(".material")));
    CHECK(entries[1].Eligibility == TEXT("MissingOriginalSource"));
    CHECK(entries[2].Eligibility == TEXT("AlreadyMigrated"));
    CHECK(entries[3].Eligibility == TEXT("Unsupported"));
    CHECK(entries[4].Eligibility == TEXT("Conflict"));
    CHECK(MigrationInventory::HasBlockingConflict(entries));

    AssetPipelineDiagnostic diagnostic;
    StringAnsi json;
    REQUIRE_FALSE(MigrationInventory::WriteCanonicalJson(entries, json, diagnostic));
    StringAnsi again;
    REQUIRE_FALSE(MigrationInventory::WriteCanonicalJson(entries, again, diagnostic));
    CHECK(json == again);
    CHECK(json.Contains("ReadyToMigrate"));
    CHECK_FALSE(json.Contains("Library"));
}

TEST_CASE("Migration journal can dry-run, resume, commit, and hash-safe roll back")
{
    const String root = Globals::TemporaryFolder / (TEXT("MigrationSession-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String backup = root / TEXT("Backup");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    const String source = content / TEXT("Mat.flax");
    const byte legacy[] = { 'l', 'e', 'g', 'a', 'c', 'y' };
    REQUIRE_FALSE(File::WriteAllBytes(source, legacy, ARRAY_COUNT(legacy)));

    Array<AssetRecord> records;
    records.Add(MakeRecord(Guid(1, 0, 0, 0), TEXT("FlaxEngine.Material"), source, AssetSourceKind::LegacyBinary));
    Array<MigrationInventoryEntry> inventory;
    MigrationInventory::Build(records, inventory);

    AssetPipelineDiagnostic diagnostic;
    Array<Guid> selected;
    selected.Add(Guid(1, 0, 0, 0));
    MigrationJournal journal;
    REQUIRE_FALSE(MigrationSession::CreatePlan(inventory, selected, backup, journal, diagnostic));
    CHECK(journal.State == TEXT("Planned"));
    CHECK_FALSE(FileSystem::FileExists(journal.Operations[0].BackupPath));
    CHECK(FileSystem::FileExists(source));

    CHECK(MigrationSession::Commit(journal, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::MigrationFailed);

    REQUIRE_FALSE(MigrationSession::Backup(journal, diagnostic));
    CHECK(journal.State == TEXT("BackedUp"));
    CHECK(FileSystem::FileExists(journal.Operations[0].BackupPath));
    REQUIRE_FALSE(MigrationSession::Backup(journal, diagnostic));

    REQUIRE_FALSE(MigrationSession::Publish(journal, diagnostic));
    CHECK(journal.State == TEXT("Published"));
    CHECK_FALSE(FileSystem::FileExists(source));
    CHECK(FileSystem::FileExists(journal.Operations[0].DestinationPath));

    const String journalPath = root / TEXT("journal.json");
    REQUIRE_FALSE(MigrationSession::SaveAtomic(journalPath, journal, diagnostic));
    MigrationJournal loaded;
    REQUIRE_FALSE(MigrationSession::Load(journalPath, loaded, diagnostic));
    CHECK(loaded.PlanFingerprint == journal.PlanFingerprint);
    REQUIRE_FALSE(MigrationSession::Commit(loaded, diagnostic));
    CHECK(loaded.State == TEXT("Committed"));

    records[0].SourcePath = SourceFilePath(source + TEXT(".changed"));
    MigrationInventory::Build(records, inventory);
    CHECK(MigrationSession::EnsureCurrentFingerprint(inventory, loaded, diagnostic));

    REQUIRE_FALSE(MigrationSession::Rollback(loaded, diagnostic));
    CHECK(loaded.State == TEXT("RolledBack"));
    CHECK(FileSystem::FileExists(source));
    CHECK_FALSE(FileSystem::FileExists(loaded.Operations[0].DestinationPath));
    Array<byte> restored;
    REQUIRE_FALSE(File::ReadAllBytes(source, restored));
    REQUIRE(restored.Count() == ARRAY_COUNT(legacy));
    CHECK(restored[0] == 'l');

    FileSystem::DeleteDirectory(root, true);
}

TEST_CASE("Migration rollback refuses post-migration edits and corrupt journals")
{
    const String root = Globals::TemporaryFolder / (TEXT("MigrationConflict-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String backup = root / TEXT("Backup");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    const String source = content / TEXT("Mat.flax");
    const byte legacy[] = { 1, 2, 3, 4 };
    REQUIRE_FALSE(File::WriteAllBytes(source, legacy, ARRAY_COUNT(legacy)));

    Array<AssetRecord> records;
    records.Add(MakeRecord(Guid(8, 0, 0, 0), TEXT("FlaxEngine.VisualScript"), source, AssetSourceKind::LegacyBinary));
    Array<MigrationInventoryEntry> inventory;
    MigrationInventory::Build(records, inventory);
    CHECK(inventory[0].ProposedDestination.EndsWith(TEXT(".visualscript")));

    AssetPipelineDiagnostic diagnostic;
    Array<Guid> selected;
    selected.Add(Guid(8, 0, 0, 0));
    MigrationJournal journal;
    REQUIRE_FALSE(MigrationSession::CreatePlan(inventory, selected, backup, journal, diagnostic));
    REQUIRE_FALSE(MigrationSession::Backup(journal, diagnostic));
    REQUIRE_FALSE(MigrationSession::Publish(journal, diagnostic));

    const byte edited[] = { 9, 9, 9 };
    REQUIRE_FALSE(File::WriteAllBytes(journal.Operations[0].DestinationPath, edited, ARRAY_COUNT(edited)));
    CHECK(MigrationSession::Rollback(journal, diagnostic));
    CHECK(diagnostic.Message.Contains(TEXT("edited")));
    CHECK(FileSystem::FileExists(journal.Operations[0].DestinationPath));

    CHECK(MigrationSession::ParseCanonicalJson("{\"formatVersion\":1}", journal, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::MigrationFailed);

    const String staging = root / TEXT("journal.json.tmp");
    REQUIRE_FALSE(File::WriteAllBytes(staging, legacy, ARRAY_COUNT(legacy)));
    CHECK(MigrationSession::Load(staging, journal, diagnostic));
    CHECK(diagnostic.Message.Contains(TEXT("staging")));

    FileSystem::DeleteDirectory(root, true);
}
