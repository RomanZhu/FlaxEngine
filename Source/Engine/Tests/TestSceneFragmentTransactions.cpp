// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    SceneFragmentWrite MakeFragmentWrite(const char* name)
    {
        const StringAnsi payload = StringAnsi::Format("[{{\"fileId\":2,\"name\":\"{0}\"}}]", name);
        SceneFragmentWrite write;
        write.RootActorLocalId = 2;
        write.ContainedLocalIds.Add(2);
        write.Payload.Set(reinterpret_cast<const byte*>(payload.Get()), payload.Length());
        return write;
    }

    void ReadBytes(const StringView& path, BytesContainer& bytes)
    {
        bytes.Release();
        REQUIRE(!File::ReadAllBytes(path, bytes));
    }

    bool SameBytes(const BytesContainer& a, const BytesContainer& b)
    {
        return a.Length() == b.Length() &&
               (a.Length() == 0 || Platform::MemoryCompare(a.Get(), b.Get(), a.Length()) == 0);
    }

    void PrepareSceneSave(const Guid& sceneGuid, const StringView& scenePath, const char* fragmentName,
        const char* sceneData, PreparedSceneSave& save)
    {
        String error;
        save = PreparedSceneSave();
        save.SourcePath = scenePath;
        REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, save.ExpectedSource, error));
        Array<SceneFragmentWrite> writes;
        writes.Add(MakeFragmentWrite(fragmentName));
        REQUIRE(!SceneFragmentStore::PrepareSave(sceneGuid, writes, save.FragmentPlan, error));
        save.SourceData.Set(reinterpret_cast<const byte*>(sceneData), StringAnsiView(sceneData).Length());
    }
}

TEST_CASE("Scene fragment save transaction recovery")
{
    const Guid sceneGuid = Guid::New();
    const String scenePath = Globals::ProjectContentFolder / TEXT("__SceneFragmentTransaction.scene");
    const String fragmentPath = SceneFragmentStore::GetScenePath(sceneGuid) /
                                SceneFragmentStore::GetRelativeFragmentPath(2);
    const String indexPath = SceneFragmentStore::GetIndexPath(sceneGuid);
    FileSystem::DeleteFile(scenePath);
    FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuid), true);
    SCOPE_EXIT
    {
        String recoveryError;
        SceneFragmentStore::RecoverIncompleteTransactions(recoveryError);
        FileSystem::DeleteFile(scenePath);
        FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuid), true);
    };

    String error;
    SceneSourceRevision expectedSource;
    REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, expectedSource, error));
    Array<SceneFragmentWrite> initialWrites;
    initialWrites.Add(MakeFragmentWrite("old"));
    SceneFragmentSavePlan initialPlan;
    REQUIRE(!SceneFragmentStore::PrepareSave(sceneGuid, initialWrites, initialPlan, error));
    const char initialScene[] = "old-scene";
    REQUIRE(!SceneFragmentStore::CommitSceneSave(scenePath, initialScene, ARRAY_COUNT(initialScene) - 1,
        expectedSource, initialPlan, error));

    BytesContainer oldScene;
    BytesContainer oldIndex;
    BytesContainer oldFragment;
    ReadBytes(scenePath, oldScene);
    ReadBytes(indexPath, oldIndex);
    ReadBytes(fragmentPath, oldFragment);

    REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, expectedSource, error));
    Array<SceneFragmentWrite> nextWrites;
    nextWrites.Add(MakeFragmentWrite("new"));
    SceneFragmentSavePlan nextPlan;
    REQUIRE(!SceneFragmentStore::PrepareSave(sceneGuid, nextWrites, nextPlan, error));
    const char nextScene[] = "new-scene";
    REQUIRE(SceneFragmentStore::CommitSceneSave(scenePath, nextScene, ARRAY_COUNT(nextScene) - 1,
        expectedSource, nextPlan, error, SceneFragmentTransactionFailurePoint::AfterFirstApply));
    REQUIRE(!SceneFragmentStore::RecoverIncompleteTransactions(error));

    BytesContainer recoveredScene;
    BytesContainer recoveredIndex;
    BytesContainer recoveredFragment;
    ReadBytes(scenePath, recoveredScene);
    ReadBytes(indexPath, recoveredIndex);
    ReadBytes(fragmentPath, recoveredFragment);
    CHECK(SameBytes(oldScene, recoveredScene));
    CHECK(SameBytes(oldIndex, recoveredIndex));
    CHECK(SameBytes(oldFragment, recoveredFragment));

    REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, expectedSource, error));
    REQUIRE(!SceneFragmentStore::PrepareSave(sceneGuid, nextWrites, nextPlan, error));
    REQUIRE(SceneFragmentStore::CommitSceneSave(scenePath, nextScene, ARRAY_COUNT(nextScene) - 1,
        expectedSource, nextPlan, error, SceneFragmentTransactionFailurePoint::AfterCommitBeforeCleanup));
    REQUIRE(!SceneFragmentStore::RecoverIncompleteTransactions(error));

    ReadBytes(scenePath, recoveredScene);
    CHECK(recoveredScene.Length() == ARRAY_COUNT(nextScene) - 1);
    CHECK(Platform::MemoryCompare(recoveredScene.Get(), nextScene, ARRAY_COUNT(nextScene) - 1) == 0);
    SceneFragmentIndex index;
    Array<Array<byte>> fragments;
    REQUIRE(!SceneFragmentStore::Load(sceneGuid, index, fragments, error));
    CHECK(index.IndexRevision == initialPlan.ExpectedIndexRevision + 2);
    REQUIRE(fragments.Count() == 1);

    BytesContainer newIndex;
    BytesContainer newFragment;
    ReadBytes(indexPath, newIndex);
    ReadBytes(fragmentPath, newFragment);
    SceneFragmentSavePlan deletePlan;
    REQUIRE(!SceneFragmentStore::PrepareDelete(sceneGuid, deletePlan, error));
    REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, expectedSource, error));
    const char internalScene[] = "internal-scene";
    REQUIRE(SceneFragmentStore::CommitSceneSave(scenePath, internalScene, ARRAY_COUNT(internalScene) - 1,
        expectedSource, deletePlan, error, SceneFragmentTransactionFailurePoint::AfterAllApplyBeforeCommit));
    REQUIRE(!SceneFragmentStore::RecoverIncompleteTransactions(error));
    ReadBytes(scenePath, recoveredScene);
    ReadBytes(indexPath, recoveredIndex);
    ReadBytes(fragmentPath, recoveredFragment);
    CHECK(recoveredScene.Length() == ARRAY_COUNT(nextScene) - 1);
    CHECK(Platform::MemoryCompare(recoveredScene.Get(), nextScene, ARRAY_COUNT(nextScene) - 1) == 0);
    CHECK(SameBytes(newIndex, recoveredIndex));
    CHECK(SameBytes(newFragment, recoveredFragment));

    REQUIRE(!SceneFragmentStore::PrepareDelete(sceneGuid, deletePlan, error));
    REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, expectedSource, error));
    REQUIRE(!SceneFragmentStore::CommitSceneSave(scenePath, internalScene, ARRAY_COUNT(internalScene) - 1,
        expectedSource, deletePlan, error));
    CHECK(!FileSystem::DirectoryExists(SceneFragmentStore::GetScenePath(sceneGuid)));
}

TEST_CASE("Scene fragment batch save transaction is atomic")
{
    const Guid sceneGuids[] = { Guid::New(), Guid::New() };
    const String scenePaths[] = {
        Globals::ProjectContentFolder / TEXT("__SceneFragmentBatchA.scene"),
        Globals::ProjectContentFolder / TEXT("__SceneFragmentBatchB.scene"),
    };
    for (int32 i = 0; i < 2; i++)
    {
        FileSystem::DeleteFile(scenePaths[i]);
        FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuids[i]), true);
    }
    SCOPE_EXIT
    {
        String recoveryError;
        SceneFragmentStore::RecoverIncompleteTransactions(recoveryError);
        for (int32 i = 0; i < 2; i++)
        {
            FileSystem::DeleteFile(scenePaths[i]);
            FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuids[i]), true);
        }
    };

    String error;
    Array<PreparedSceneSave> saves;
    saves.Resize(2);
    PrepareSceneSave(sceneGuids[0], scenePaths[0], "old-a", "old-scene-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], "old-b", "old-scene-b", saves[1]);
    REQUIRE(!SceneFragmentStore::CommitSceneSaves(saves, error));

    BytesContainer oldSources[2];
    BytesContainer oldIndexes[2];
    BytesContainer oldFragments[2];
    for (int32 i = 0; i < 2; i++)
    {
        ReadBytes(scenePaths[i], oldSources[i]);
        ReadBytes(SceneFragmentStore::GetIndexPath(sceneGuids[i]), oldIndexes[i]);
        ReadBytes(SceneFragmentStore::GetScenePath(sceneGuids[i]) /
            SceneFragmentStore::GetRelativeFragmentPath(2), oldFragments[i]);
    }

    PrepareSceneSave(sceneGuids[0], scenePaths[0], "new-a", "new-scene-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], "new-b", "new-scene-b", saves[1]);
    REQUIRE(SceneFragmentStore::CommitSceneSaves(saves, error,
        SceneFragmentTransactionFailurePoint::AfterFirstApply));
    REQUIRE(!SceneFragmentStore::RecoverIncompleteTransactions(error));

    for (int32 i = 0; i < 2; i++)
    {
        BytesContainer source;
        BytesContainer index;
        BytesContainer fragment;
        ReadBytes(scenePaths[i], source);
        ReadBytes(SceneFragmentStore::GetIndexPath(sceneGuids[i]), index);
        ReadBytes(SceneFragmentStore::GetScenePath(sceneGuids[i]) /
            SceneFragmentStore::GetRelativeFragmentPath(2), fragment);
        CHECK(SameBytes(oldSources[i], source));
        CHECK(SameBytes(oldIndexes[i], index));
        CHECK(SameBytes(oldFragments[i], fragment));
    }

    PrepareSceneSave(sceneGuids[0], scenePaths[0], "new-a", "new-scene-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], "new-b", "new-scene-b", saves[1]);
    REQUIRE(!SceneFragmentStore::CommitSceneSaves(saves, error));
    for (int32 i = 0; i < 2; i++)
    {
        BytesContainer source;
        ReadBytes(scenePaths[i], source);
        CHECK_FALSE(SameBytes(oldSources[i], source));
        SceneFragmentIndex index;
        Array<Array<byte>> fragments;
        REQUIRE(!SceneFragmentStore::Load(sceneGuids[i], index, fragments, error));
        REQUIRE(fragments.Count() == 1);
    }
}

#endif
