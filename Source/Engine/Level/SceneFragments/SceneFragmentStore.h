// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "SceneFragmentIndex.h"
#include "SceneFragmentDiagnostics.h"
#include "SceneFragmentSavePlan.h"

/// <summary>One deterministic fragment produced by scene serialization.</summary>
struct FLAXENGINE_API SceneFragmentWrite
{
    int64 RootActorLocalId = 0;
    Array<int64> ContainedLocalIds;
    Array<byte> Payload;
    uint32 SerializerVersion = 1;
};

/// <summary>Private scene-GUID-keyed fragment storage.</summary>
class FLAXENGINE_API SceneFragmentStore
{
public:
    static constexpr uint32 FragmentFormatVersion = 1;

    static String GetRootPath(const StringView& projectRoot);
    static String GetRootPath();
    static String GetScenePath(const StringView& projectRoot, const Guid& sceneGuid);
    static String GetScenePath(const Guid& sceneGuid);
    static String GetIndexPath(const Guid& sceneGuid);
    static String GetRelativeFragmentPath(int64 rootActorLocalId);

    /// <summary>Validates a source fragment tree and prepares an owner-remapped clone in a caller-owned staging directory.</summary>
    static bool PrepareCloneDirectory(const StringView& projectRoot, const Guid& sourceSceneGuid,
        const Guid& destinationSceneGuid, const StringView& stagingDirectory, String& error);

    /// <summary>Loads and validates the index and every referenced fragment. Returns true on failure.</summary>
    static bool Load(const Guid& sceneGuid, SceneFragmentIndex& index, Array<Array<byte>>& fragments, String& error);

    /// <summary>Prepares serialized fragment and index bytes without mutating source state. Returns true on failure.</summary>
    static bool PrepareSave(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments,
        SceneFragmentSavePlan& plan, String& error);

    /// <summary>Prepares removal of the complete private fragment store without mutating source state. Returns true on failure.</summary>
    static bool PrepareDelete(const Guid& sceneGuid, SceneFragmentSavePlan& plan, String& error);

    /// <summary>Captures the exact current scene source revision before serialization. Returns true on failure.</summary>
    static bool CaptureSourceRevision(const StringView& scenePath, SceneSourceRevision& revision, String& error);

    /// <summary>Commits one scene source and its prepared fragment set through a recoverable journal. Returns true on failure.</summary>
    static bool CommitSceneSave(const StringView& scenePath, const void* sceneData, int32 sceneDataLength,
        const SceneSourceRevision& expectedSource, const SceneFragmentSavePlan& plan, String& error,
        SceneFragmentTransactionFailurePoint failurePoint = SceneFragmentTransactionFailurePoint::None);

    /// <summary>Commits several prepared scene sources and fragment sets through one recoverable journal. Returns true on failure.</summary>
    static bool CommitSceneSaves(const Array<PreparedSceneSave>& saves, String& error,
        SceneFragmentTransactionFailurePoint failurePoint = SceneFragmentTransactionFailurePoint::None);

    /// <summary>Recovers all interrupted scene-fragment transactions. Returns true on failure.</summary>
    static bool RecoverIncompleteTransactions(String& error);

    /// <summary>Publishes fragments through the recoverable private-store transaction. Returns true on failure.</summary>
    static bool Save(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments, String& error);

    /// <summary>Removes the private fragment directory owned by a scene. Returns true on failure.</summary>
    static bool Delete(const Guid& sceneGuid, String& error);

    static bool ReadIndex(const Guid& sceneGuid, SceneFragmentIndex& index, String& error);
};
