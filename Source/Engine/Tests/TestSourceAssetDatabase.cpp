// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/SourceAssetDatabase.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
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
    transaction->UpsertSource(MakeSource(assetId, TEXT("Assets/Body.glb")));
    SourceAssetObjectRow object;
    object.AssetGuid = assetId;
    object.ObjectGuid = assetId;
    object.LocalFileId = 1;
    object.StableIdentifier = TEXT("main");
    object.TypeName = TEXT("FlaxEngine.Model");
    object.IsMain = true;
    Array<SourceAssetObjectRow> objects;
    objects.Add(object);
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
    first->UpsertSource(MakeSource(Guid(21, 22, 23, 24), TEXT("Assets/A.png")));
    REQUIRE_FALSE(first->Commit(diagnostic));
    stale->UpsertSource(MakeSource(Guid(31, 32, 33, 34), TEXT("Assets/B.png")));
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
