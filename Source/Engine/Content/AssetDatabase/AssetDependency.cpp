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
        return a.Kind == b.Kind && a.StableIdentity == b.StableIdentity && a.ObjectID == b.ObjectID && a.AssetID == b.AssetID;
    }

    const char* StateName(AssetDependencyState state)
    {
        switch (state)
        {
        case AssetDependencyState::Present: return "present";
        case AssetDependencyState::Missing: return "missing";
        case AssetDependencyState::CurrentArtifact: return "current";
        case AssetDependencyState::ExactArtifact: return "exact";
        default: return "invalid";
        }
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
    builder.AddUInt32(prefix + "state", static_cast<uint32>(State));
    builder.AddString(prefix + "identity", StableIdentity);
    if (ObjectID.IsValid())
    {
        builder.AddGuid(prefix + "file-guid", ObjectID.Guid);
        builder.AddUInt64(prefix + "local-id", static_cast<uint64>(ObjectID.LocalId));
    }
    if (AssetID.IsValid())
        builder.AddGuid(prefix + "asset", AssetID);
    switch (Kind)
    {
    case AssetDependencyKind::ExactSourceFile:
    case AssetDependencyKind::SourceAsset:
        if (State == AssetDependencyState::Missing)
        {
            builder.AddString(prefix + "fingerprint", StringAnsiView("MISSING"));
            break;
        }
        builder.AddHash(prefix + "content", Content);
        if (!Metadata.IsZero())
            builder.AddHash(prefix + "metadata", Metadata);
        break;
    case AssetDependencyKind::Custom:
    case AssetDependencyKind::Global:
    case AssetDependencyKind::Target:
    case AssetDependencyKind::ImporterProvider:
    case AssetDependencyKind::Environment:
    case AssetDependencyKind::Toolchain:
        builder.AddHash(prefix + "content", Content);
        break;
    case AssetDependencyKind::LogicalPath:
        break;
    case AssetDependencyKind::Artifact:
        if (State == AssetDependencyState::Missing)
        {
            builder.AddString(prefix + "fingerprint", StringAnsiView("MISSING"));
            break;
        }
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

StringAnsi AssetDependency::DescribeFingerprint() const
{
    StringAnsi result = StringAnsi::Format("{0}|{1}", static_cast<uint32>(Kind), StateName(State));
    if (State == AssetDependencyState::Missing)
        return result + "|MISSING";
    if (!Content.IsZero())
        result += "|content=" + Content.ToString();
    if (!Metadata.IsZero())
        result += "|metadata=" + Metadata.ToString();
    if (!ExactArtifact.IsZero())
        result += "|artifact=" + ExactArtifact.ToString();
    if (!SemanticInterface.IsZero())
        result += StringAnsi::Format("|interface={0}:{1}", InterfaceVersion, SemanticInterface.ToString());
    if (ObjectID.IsValid())
        result += StringAnsi::Format("|object={0}:{1}", StringAnsi(ObjectID.Guid.ToString(Guid::FormatType::N)), ObjectID.LocalId);
    return result;
}

bool AssetDependency::NormalizeAndSort(Array<AssetDependency>& dependencies, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    for (AssetDependency& dependency : dependencies)
    {
        dependency.StableIdentity.Replace(TEXT('\\'), TEXT('/'));
        const bool sourceKind = dependency.Kind == AssetDependencyKind::ExactSourceFile || dependency.Kind == AssetDependencyKind::SourceAsset;
        const bool hashedKind = sourceKind || dependency.Kind == AssetDependencyKind::Custom || dependency.Kind == AssetDependencyKind::Global ||
            dependency.Kind == AssetDependencyKind::Target || dependency.Kind == AssetDependencyKind::ImporterProvider ||
            dependency.Kind == AssetDependencyKind::Toolchain || dependency.Kind == AssetDependencyKind::Environment;
        const bool artifactKind = dependency.Kind == AssetDependencyKind::Artifact;
        const bool missingAllowed = sourceKind || artifactKind;
        if (dependency.StableIdentity.IsEmpty() ||
            ((artifactKind || dependency.Kind == AssetDependencyKind::RuntimeReference) && !dependency.AssetID.IsValid() && !dependency.ObjectID.IsValid()) ||
            (dependency.State == AssetDependencyState::Missing && !missingAllowed) ||
            (dependency.State != AssetDependencyState::Missing && hashedKind && dependency.Content.IsZero()) ||
            (artifactKind && dependency.State != AssetDependencyState::Missing && dependency.ExactArtifact.IsZero() && dependency.SemanticInterface.IsZero()) ||
            (artifactKind && dependency.State == AssetDependencyState::Present) ||
            (!artifactKind && (dependency.State == AssetDependencyState::CurrentArtifact || dependency.State == AssetDependencyState::ExactArtifact)))
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
        const int32 objectGuid = CompareGuid(a.ObjectID.Guid, b.ObjectID.Guid);
        if (objectGuid != 0)
            return objectGuid < 0;
        if (a.ObjectID.LocalId != b.ObjectID.LocalId)
            return a.ObjectID.LocalId < b.ObjectID.LocalId;
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
