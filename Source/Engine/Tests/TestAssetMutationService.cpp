// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetMutationService.h"
#include "Engine/Content/AssetDatabase/AssetSaveService.h"
#include "Engine/Content/Documents/AuthoredSourceDocument.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    AssetMeta MakeMutationMeta()
    {
        AssetMeta meta;
        meta.ID = Guid::New();
        meta.AssetType = TEXT("FlaxEngine.JsonAsset");
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = TEXT("Flax.Test");
        return meta;
    }

    bool WriteMutationBytes(const StringView& path, const char* value)
    {
        return File::WriteAllBytes(path, value, StringUtils::Length(value));
    }
}

TEST_CASE("Native asset mutation service publishes complete source metadata pairs")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetMutation-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library/AssetRecovery/Journals");
    const String recovery = root / TEXT("Recovery");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetMutationService service(root, content, library, recovery);
    AssetMutationResult result;

    const String folder = content / TEXT("Sources");
    REQUIRE_FALSE(service.CreateFolder(folder, result));
    REQUIRE(result.Succeeded);
    REQUIRE(FileSystem::DirectoryExists(folder));
    REQUIRE(FileSystem::FileExists(folder + TEXT(".meta")));

    const String source = folder / TEXT("A.txt");
    REQUIRE_FALSE(WriteMutationBytes(source, "one"));
    AssetMeta sourceMeta = MakeMutationMeta();
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(source + TEXT(".meta"), sourceMeta, diagnostic));

    const String copied = folder / TEXT("B.txt");
    REQUIRE_FALSE(service.Copy(source, copied, result));
    CHECK(result.AssetID != sourceMeta.ID);
    AssetMeta copiedMeta;
    REQUIRE_FALSE(AssetMeta::Load(copied + TEXT(".meta"), copiedMeta, diagnostic));
    CHECK(copiedMeta.ID == result.AssetID);
    CHECK(copiedMeta.ID != sourceMeta.ID);

    const String moved = folder / TEXT("C.txt");
    REQUIRE_FALSE(service.Move(copied, moved, result));
    REQUIRE_FALSE(AssetMeta::Load(moved + TEXT(".meta"), copiedMeta, diagnostic));
    CHECK(copiedMeta.ID == result.AssetID);
    CHECK_FALSE(FileSystem::FileExists(copied));
    CHECK_FALSE(FileSystem::FileExists(copied + TEXT(".meta")));

    const String replacement = root / TEXT("Replacement.txt");
    REQUIRE_FALSE(WriteMutationBytes(replacement, "two"));
    REQUIRE_FALSE(service.ReplaceContents(moved, replacement, result));
    Array<byte> bytes;
    REQUIRE_FALSE(File::ReadAllBytes(moved, bytes));
    REQUIRE(bytes.Count() == 3);
    CHECK(Platform::MemoryCompare(bytes.Get(), "two", 3) == 0);
    AssetMeta retainedMeta;
    REQUIRE_FALSE(AssetMeta::Load(moved + TEXT(".meta"), retainedMeta, diagnostic));
    CHECK(retainedMeta.ID == copiedMeta.ID);

    REQUIRE_FALSE(service.DeleteToRecovery(moved, result));
    const String recoveryPath = result.RecoveryPath;
    CHECK_FALSE(FileSystem::FileExists(moved));
    REQUIRE(FileSystem::FileExists(recoveryPath));
    REQUIRE(FileSystem::FileExists(recoveryPath + TEXT(".meta")));

    const String restored = folder / TEXT("Restored.txt");
    REQUIRE_FALSE(service.Recover(recoveryPath, restored, result));
    REQUIRE(FileSystem::FileExists(restored));
    AssetMeta restoredMeta;
    REQUIRE_FALSE(AssetMeta::Load(restored + TEXT(".meta"), restoredMeta, diagnostic));
    CHECK(restoredMeta.ID == copiedMeta.ID);

    Array<AssetMutationResult> recovered;
    CHECK_FALSE(service.RecoverPending(recovered));
    CHECK(recovered.IsEmpty());
}

TEST_CASE("Authored save service preserves local IDs and tombstones")
{
    const String root = Globals::TemporaryFolder / (TEXT("AuthoredSave-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String journals = root / TEXT("Library/AssetDatabase/MutationJournals");
    const String recovery = root / TEXT("Library/AssetDatabase/Recovery");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetSaveService service(root, content, journals, recovery);
    const String path = content / TEXT("Objects.asset");
    AssetSaveResult result;
    REQUIRE_FALSE(service.CreateAsset(path, TEXT("authored:main"), TEXT("Tests.Main"), TEXT("Main"), StringAnsiView("{\"value\":1}"), result));
    REQUIRE(result.AssetID.IsValid());

    int64 childId = 0;
    REQUIRE_FALSE(service.AddObjectToAsset(path, TEXT("authored:child"), TEXT("Tests.Child"), TEXT("Child"),
        StringAnsiView("{\"value\":2}"), childId, result));
    CHECK(childId > 1);
    REQUIRE_FALSE(service.SetMainObject(path, childId, result));
    REQUIRE_FALSE(service.SetMainObject(path, 1, result));
    REQUIRE_FALSE(service.RemoveObjectFromAsset(path, childId, result));

    Array<byte> bytes;
    REQUIRE_FALSE(File::ReadAllBytes(path, bytes));
    AuthoredSourceDocument document;
    String error;
    REQUIRE_FALSE(AuthoredSourceDocument::Parse(StringAnsiView((const char*)bytes.Get(), bytes.Count()), document, error));
    REQUIRE(document.Tombstones.Count() == 1);
    CHECK(document.Tombstones[0].LocalId == childId);

    int64 replacementId = 0;
    REQUIRE_FALSE(service.AddObjectToAsset(path, TEXT("authored:replacement"), TEXT("Tests.Child"), TEXT("Replacement"),
        StringAnsiView("{\"value\":3}"), replacementId, result));
    CHECK(replacementId != childId);
    REQUIRE_FALSE(service.StageObjectData(path, 1, StringAnsiView("{\"value\":4}"), TEXT("test edit"), result));
    CHECK(service.IsDirty(path));
    REQUIRE_FALSE(service.SaveAssetIfDirty(path, result));
    CHECK(result.Saved);
    CHECK_FALSE(service.IsDirty(path));
    REQUIRE_FALSE(service.ForceReserialize(path, true, result));
}

#endif
