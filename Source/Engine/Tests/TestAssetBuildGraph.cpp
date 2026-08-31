// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Build/AssetBuildGraph.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    PreparedAsset GraphAsset(const Guid& id, uint64 revision = 11)
    {
        PreparedAsset asset;
        asset.AssetID = id;
        asset.ObjectID = AssetObjectId::Main(AssetGuid(id));
        asset.DatabaseRevision = revision;
        return asset;
    }

    void AddGraphDependency(PreparedAsset& owner, const Guid& dependencyId, AssetDependencyKind kind, const StringView& origin)
    {
        AssetDependency dependency;
        dependency.Kind = kind;
        dependency.StableIdentity = dependencyId.ToString(Guid::FormatType::N);
        dependency.ObjectID = AssetObjectId::Main(AssetGuid(dependencyId));
        dependency.ExactArtifact = ArtifactKey(ContentHash::Compute(&dependencyId, sizeof(dependencyId)));
        dependency.Origin.Path = origin;
        dependency.Origin.GraphNode = owner.AssetID.ToString(Guid::FormatType::N);
        dependency.Origin.GraphPin = TEXT("input");
        owner.Dependencies.Add(MoveTemp(dependency));
    }
}

TEST_CASE("AssetBuildGraph orders build inputs deterministically and ignores runtime cycles")
{
    const Guid a(30, 0, 0, 0);
    const Guid b(20, 0, 0, 0);
    const Guid c(10, 0, 0, 0);
    PreparedAsset assetA = GraphAsset(a);
    PreparedAsset assetB = GraphAsset(b);
    PreparedAsset assetC = GraphAsset(c);
    AddGraphDependency(assetA, b, AssetDependencyKind::BuildInput, TEXT("a:input"));
    AddGraphDependency(assetB, c, AssetDependencyKind::BuildInput, TEXT("b:input"));
    AddGraphDependency(assetC, a, AssetDependencyKind::RuntimeReference, TEXT("c:runtime"));
    Array<PreparedAsset> assets;
    assets.Add(assetA);
    assets.Add(assetC);
    assets.Add(assetB);

    AssetBuildGraph graph;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(graph.Build(assets, 11, diagnostic));
    REQUIRE(graph.GetBuildOrder().Count() == 3);
    CHECK(graph.GetBuildOrder()[0] == AssetObjectId::Main(AssetGuid(c)));
    CHECK(graph.GetBuildOrder()[1] == AssetObjectId::Main(AssetGuid(b)));
    CHECK(graph.GetBuildOrder()[2] == AssetObjectId::Main(AssetGuid(a)));
    CHECK(graph.IsCurrent(11));
    CHECK_FALSE(graph.IsCurrent(12));

    Array<PreparedAsset> reversed;
    reversed.Add(assetB);
    reversed.Add(assetA);
    reversed.Add(assetC);
    AssetBuildGraph second;
    REQUIRE_FALSE(second.Build(reversed, 11, diagnostic));
    CHECK(second.GetBuildOrder() == graph.GetBuildOrder());
}

TEST_CASE("AssetBuildGraph reports the full build-input cycle with origins")
{
    const Guid a(1, 0, 0, 0);
    const Guid b(2, 0, 0, 0);
    const Guid c(3, 0, 0, 0);
    PreparedAsset assetA = GraphAsset(a);
    PreparedAsset assetB = GraphAsset(b);
    PreparedAsset assetC = GraphAsset(c);
    AddGraphDependency(assetA, b, AssetDependencyKind::BuildInput, TEXT("Content/a.graph"));
    AddGraphDependency(assetB, c, AssetDependencyKind::BuildInput, TEXT("Content/b.graph"));
    AddGraphDependency(assetC, a, AssetDependencyKind::BuildInput, TEXT("Content/c.graph"));
    Array<PreparedAsset> assets;
    assets.Add(assetC);
    assets.Add(assetA);
    assets.Add(assetB);

    AssetBuildGraph graph;
    AssetPipelineDiagnostic diagnostic;
    CHECK(graph.Build(assets, 11, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCycle);
    CHECK(diagnostic.Message.Contains(a.ToString(Guid::FormatType::N)));
    CHECK(diagnostic.Message.Contains(b.ToString(Guid::FormatType::N)));
    CHECK(diagnostic.Message.Contains(c.ToString(Guid::FormatType::N)));
    CHECK(diagnostic.Related.Count() == 3);
    CHECK(diagnostic.Location.GraphPin == TEXT("input"));

    assets[0].DatabaseRevision = 12;
    CHECK(graph.Build(assets, 11, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
}

TEST_CASE("AssetBuildGraph handles deterministic hundred-thousand-edge fan-out")
{
    Array<PreparedAsset> assets;
    for (int32 i = 0; i < 100; i++)
        assets.Add(GraphAsset(Guid(i + 1, 0, 0, 0)));
    for (int32 i = 0; i < 1000; i++)
    {
        PreparedAsset dependant = GraphAsset(Guid(1000 + i, 0, 0, 0));
        for (int32 dependency = 0; dependency < 100; dependency++)
            AddGraphDependency(dependant, assets[dependency].ObjectID.Asset.Value, AssetDependencyKind::BuildInput, TEXT("synthetic-fan-out"));
        assets.Add(MoveTemp(dependant));
    }
    AssetBuildGraph graph;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(graph.Build(assets, 11, diagnostic));
    REQUIRE(graph.GetBuildOrder().Count() == 1100);
    CHECK(graph.GetBuildOrder()[0] == AssetObjectId::Main(AssetGuid(Guid(1, 0, 0, 0))));
    CHECK(graph.GetBuildOrder()[99] == AssetObjectId::Main(AssetGuid(Guid(100, 0, 0, 0))));
}
