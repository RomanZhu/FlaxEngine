// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDependency.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    ArtifactKey BuildDependencyKey(Array<AssetDependency> dependencies)
    {
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(AssetDependency::NormalizeAndSort(dependencies, diagnostic));
        ArtifactKeyBuilder builder;
        int32 buildIndex = 0;
        for (const AssetDependency& dependency : dependencies)
        {
            if (dependency.AffectsBuildKey())
                dependency.AppendKeyComponents(builder, buildIndex++);
        }
        return builder.Finalize();
    }

    AssetDependency SourceDependency(const StringView& path, const char* bytes)
    {
        AssetDependency result;
        result.Kind = AssetDependencyKind::SourceFile;
        result.StableIdentity = path;
        result.Content = ContentHash::Compute(bytes, StringUtils::Length(bytes));
        result.Origin.Path = path;
        return result;
    }
}

TEST_CASE("Asset dependencies serialize deterministically by semantic kind")
{
    AssetDependency sourceA = SourceDependency(TEXT("Content\\a.txt"), "a");
    AssetDependency sourceB = SourceDependency(TEXT("Content/b.txt"), "b");
    Array<AssetDependency> forward;
    forward.Add(sourceA);
    forward.Add(sourceB);
    Array<AssetDependency> reverse;
    reverse.Add(sourceB);
    reverse.Add(sourceA);
    CHECK(BuildDependencyKey(forward) == BuildDependencyKey(reverse));

    sourceA.Origin.GraphNode = TEXT("DifferentDiagnosticOrigin");
    Array<AssetDependency> changedOrigin;
    changedOrigin.Add(sourceA);
    changedOrigin.Add(sourceB);
    CHECK(BuildDependencyKey(forward) == BuildDependencyKey(changedOrigin));
}

TEST_CASE("Runtime references do not invalidate build keys")
{
    Array<AssetDependency> dependencies;
    dependencies.Add(SourceDependency(TEXT("Content/source.txt"), "source"));
    const ArtifactKey withoutRuntime = BuildDependencyKey(dependencies);

    AssetDependency runtime;
    runtime.Kind = AssetDependencyKind::RuntimeReference;
    runtime.StableIdentity = TEXT("runtime-a");
    runtime.AssetID = Guid(1, 2, 3, 4);
    dependencies.Add(runtime);
    CHECK(BuildDependencyKey(dependencies) == withoutRuntime);
    dependencies.Last().StableIdentity = TEXT("runtime-b");
    dependencies.Last().AssetID = Guid(5, 6, 7, 8);
    CHECK(BuildDependencyKey(dependencies) == withoutRuntime);
}

TEST_CASE("Semantic interface versions participate in dependant keys")
{
    AssetDependency input;
    input.Kind = AssetDependencyKind::BuildInput;
    input.StableIdentity = TEXT("material-interface");
    input.AssetID = Guid(10, 11, 12, 13);
    input.InterfaceVersion = 1;
    input.SemanticInterface = ContentHash::Compute("surface", 7);
    Array<AssetDependency> dependencies;
    dependencies.Add(input);
    const ArtifactKey first = BuildDependencyKey(dependencies);
    dependencies[0].InterfaceVersion = 2;
    CHECK(BuildDependencyKey(dependencies) != first);
    dependencies[0].InterfaceVersion = 1;
    dependencies[0].SemanticInterface = ContentHash::Compute("surface-2", 9);
    CHECK(BuildDependencyKey(dependencies) != first);
}

TEST_CASE("Asset database indexes build and runtime reverse edges independently")
{
    const Guid owner(20, 21, 22, 23);
    const Guid buildInput(30, 31, 32, 33);
    const Guid runtimeReference(40, 41, 42, 43);
    AssetRecord record;
    record.ID = owner;
    record.SourceAssetID = owner;
    record.TypeName = TEXT("Synthetic");
    record.CanonicalPath = CanonicalAssetPath(TEXT("Content/owner.synthetic"));
    record.SourcePath = SourceFilePath(TEXT("Content/owner.synthetic"));
    record.BuildInputDependencies.Add(buildInput);
    record.RuntimeReferences.Add(runtimeReference);
    Array<AssetRecord> records;
    records.Add(record);
    AssetDatabase database;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));

    Array<AssetRecord> result;
    database.GetBuildDependants(buildInput, result);
    REQUIRE(result.Count() == 1);
    CHECK(result[0].ID == owner);
    database.GetRuntimeReferencers(runtimeReference, result);
    REQUIRE(result.Count() == 1);
    CHECK(result[0].ID == owner);
    database.GetBuildDependants(runtimeReference, result);
    CHECK(result.IsEmpty());
}
