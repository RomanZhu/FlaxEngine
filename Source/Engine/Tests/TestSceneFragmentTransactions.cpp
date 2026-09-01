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

#endif
