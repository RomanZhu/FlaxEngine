// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetBuildSnapshot.h"
#include <algorithm>

namespace
{
    bool Less(const AssetObjectId& a, const AssetObjectId& b)
    {
        const Guid& left = a.Asset.Value;
        const Guid& right = b.Asset.Value;
        if (left.A != right.A)
            return left.A < right.A;
        if (left.B != right.B)
            return left.B < right.B;
        if (left.C != right.C)
            return left.C < right.C;
        if (left.D != right.D)
            return left.D < right.D;
        return a.LocalId < b.LocalId;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
        diagnostic.Message = message;
        return true;
    }
}

bool AssetBuildSnapshot::NormalizeAndValidate(AssetPipelineDiagnostic& diagnostic)
{
    if (DatabaseRevision == 0 || TargetHash.IsZero() || ProjectSettingsHash.IsZero() || RootObjects.IsEmpty() || Artifacts.IsEmpty())
        return Fail(diagnostic, TEXT("Build snapshot requires a database revision, target and settings hashes, roots, and exact artifacts."));
    if (Target.Role != "Runtime")
        return Fail(diagnostic, TEXT("Build snapshot target role must be Runtime."));

    std::sort(RootObjects.Get(), RootObjects.Get() + RootObjects.Count(), Less);
    for (int32 i = 0; i < RootObjects.Count(); i++)
    {
        if (!RootObjects[i].IsValid() || (i != 0 && RootObjects[i] == RootObjects[i - 1]))
            return Fail(diagnostic, TEXT("Build snapshot contains an invalid or duplicate root object."));
    }

    std::sort(Artifacts.Get(), Artifacts.Get() + Artifacts.Count(), [](const AssetBuildSnapshotArtifact& a, const AssetBuildSnapshotArtifact& b)
    {
        return Less(a.Object, b.Object);
    });
    for (int32 i = 0; i < Artifacts.Count(); i++)
    {
        const AssetBuildSnapshotArtifact& artifact = Artifacts[i];
        if (!artifact.Object.IsValid() || artifact.Manifest.IsZero() || artifact.ObjectContent.IsZero() ||
            (i != 0 && artifact.Object == Artifacts[i - 1].Object))
            return Fail(diagnostic, TEXT("Build snapshot contains an invalid or duplicate exact artifact publication."));
    }
    for (const AssetObjectId& root : RootObjects)
    {
        bool found = false;
        for (const AssetBuildSnapshotArtifact& artifact : Artifacts)
        {
            if (artifact.Object == root)
            {
                found = true;
                break;
            }
        }
        if (!found)
            return Fail(diagnostic, TEXT("Build snapshot root has no pinned artifact publication."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetBuildSnapshot::ComputeFingerprint(ArtifactKey& result, AssetPipelineDiagnostic& diagnostic) const
{
    AssetBuildSnapshot normalized = *this;
    if (normalized.NormalizeAndValidate(diagnostic))
    {
        result = ArtifactKey();
        return true;
    }

    ArtifactKeyBuilder builder(StringAnsiView("flax-asset-build-snapshot-v1"));
    builder.AddUInt64(StringAnsiView("database-revision"), normalized.DatabaseRevision);
    builder.AddTarget(normalized.Target, ArtifactTargetDimension::All);
    builder.AddHash(StringAnsiView("target"), normalized.TargetHash);
    builder.AddHash(StringAnsiView("project-settings"), normalized.ProjectSettingsHash);
    builder.AddUInt32(StringAnsiView("root-count"), normalized.RootObjects.Count());
    for (int32 i = 0; i < normalized.RootObjects.Count(); i++)
    {
        const AssetObjectId& object = normalized.RootObjects[i];
        builder.AddGuid(StringAnsi::Format("root-{0}-guid", i), object.Asset.Value);
        builder.AddUInt64(StringAnsi::Format("root-{0}-local", i), static_cast<uint64>(object.LocalId));
    }
    builder.AddUInt32(StringAnsiView("artifact-count"), normalized.Artifacts.Count());
    for (int32 i = 0; i < normalized.Artifacts.Count(); i++)
    {
        const AssetBuildSnapshotArtifact& artifact = normalized.Artifacts[i];
        builder.AddGuid(StringAnsi::Format("artifact-{0}-guid", i), artifact.Object.Asset.Value);
        builder.AddUInt64(StringAnsi::Format("artifact-{0}-local", i), static_cast<uint64>(artifact.Object.LocalId));
        builder.AddKey(StringAnsi::Format("artifact-{0}-manifest", i), artifact.Manifest);
        builder.AddHash(StringAnsi::Format("artifact-{0}-content", i), artifact.ObjectContent);
    }
    result = builder.Finalize();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
