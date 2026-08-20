// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDependency.h"
#include <algorithm>

namespace
{
    int32 CompareGuid(const Guid& a, const Guid& b)
    {
        for (int32 i = 0; i < 4; i++)
        {
            if (a.Values[i] < b.Values[i])
                return -1;
            if (a.Values[i] > b.Values[i])
                return 1;
        }
        return 0;
    }

    bool SameIdentity(const AssetDependency& a, const AssetDependency& b)
    {
        return a.Kind == b.Kind && a.StableIdentity == b.StableIdentity && a.AssetID == b.AssetID;
    }
}

bool AssetDependency::AffectsBuildKey() const
{
    return Kind != AssetDependencyKind::RuntimeReference;
}

void AssetDependency::AppendKeyComponents(ArtifactKeyBuilder& builder, int32 index) const
{
    if (!AffectsBuildKey())
        return;
    const StringAnsi prefix = StringAnsi::Format("dependency-{0}-", index);
    builder.AddUInt32(prefix + "kind", static_cast<uint32>(Kind));
    builder.AddString(prefix + "identity", StableIdentity);
    if (AssetID.IsValid())
        builder.AddGuid(prefix + "asset", AssetID);
    switch (Kind)
    {
    case AssetDependencyKind::SourceFile:
    case AssetDependencyKind::Toolchain:
        builder.AddHash(prefix + "content", Content);
        break;
    case AssetDependencyKind::BuildInput:
        if (!SemanticInterface.IsZero())
        {
            builder.AddUInt32(prefix + "interface-version", InterfaceVersion);
            builder.AddHash(prefix + "interface", SemanticInterface);
        }
        else
        {
            builder.AddKey(prefix + "artifact", ExactArtifact);
        }
        break;
    case AssetDependencyKind::RuntimeReference:
        break;
    }
}

bool AssetDependency::NormalizeAndSort(Array<AssetDependency>& dependencies, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    for (AssetDependency& dependency : dependencies)
    {
        dependency.StableIdentity.Replace(TEXT('\\'), TEXT('/'));
        if (dependency.StableIdentity.IsEmpty() ||
            ((dependency.Kind == AssetDependencyKind::BuildInput || dependency.Kind == AssetDependencyKind::RuntimeReference) && !dependency.AssetID.IsValid()) ||
            ((dependency.Kind == AssetDependencyKind::SourceFile || dependency.Kind == AssetDependencyKind::Toolchain) && dependency.Content.IsZero()) ||
            (dependency.Kind == AssetDependencyKind::BuildInput && dependency.ExactArtifact.IsZero() && dependency.SemanticInterface.IsZero()))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.SourcePath = dependency.Origin.Path;
            diagnostic.Message = TEXT("Dependency declaration has an invalid identity, hash, or build input key.");
            return true;
        }
    }
    std::sort(dependencies.Get(), dependencies.Get() + dependencies.Count(), [](const AssetDependency& a, const AssetDependency& b)
    {
        if (a.Kind != b.Kind)
            return static_cast<byte>(a.Kind) < static_cast<byte>(b.Kind);
        if (a.StableIdentity != b.StableIdentity)
            return a.StableIdentity < b.StableIdentity;
        return CompareGuid(a.AssetID, b.AssetID) < 0;
    });
    for (int32 i = 1; i < dependencies.Count(); i++)
    {
        if (SameIdentity(dependencies[i - 1], dependencies[i]))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.SourcePath = dependencies[i].Origin.Path;
            diagnostic.Message = TEXT("Dependency declaration is duplicated.");
            diagnostic.Related.Add(dependencies[i - 1].Origin.Path);
            diagnostic.Related.Add(dependencies[i].Origin.Path);
            return true;
        }
    }
    return false;
}
