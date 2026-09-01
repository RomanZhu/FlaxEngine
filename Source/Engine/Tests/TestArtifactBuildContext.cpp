// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    PreparedAsset BuildContextPrepared(const StringView& sourceIdentity, const ContentHash& sourceHash)
    {
        PreparedAsset prepared;
        prepared.AssetID = Guid(1, 2, 3, 4);
        prepared.DatabaseRevision = 9;
        AssetDependency dependency;
        dependency.Kind = AssetDependencyKind::SourceFile;
        dependency.StableIdentity = sourceIdentity;
        dependency.Content = sourceHash;
        prepared.Dependencies.Add(dependency);
        DeclaredArtifactOutput output;
        output.Kind = "runtime";
        output.Extension = ".flax";
        output.EffectiveAssetID = prepared.AssetID;
        prepared.Outputs.Add(output);
        return prepared;
    }
}

TEST_CASE("ArtifactBuildContext confines declared inputs and output writers")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactBuildContext-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = content / TEXT("source.synthetic");
    const char sourceBytes[] = "declared-source";
    REQUIRE_FALSE(File::WriteAllBytes(source, sourceBytes, 15));
    const ContentHash sourceHash = ContentHash::Compute(sourceBytes, 15);
    PreparedAsset prepared = BuildContextPrepared(TEXT("Content/source.synthetic"), sourceHash);
    ArtifactBuildInput input;
    input.StableIdentity = TEXT("Content/source.synthetic");
    input.Path = source;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    AssetCancellationSource cancellation;
    ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken(), 1024, 1024, 4);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(context.Initialize(diagnostic));
    CHECK(AssetPathPolicy::IsSameOrChild(context.GetStagingPath(), library));
    CHECK(context.GetExternalProcessWorkingDirectory() == context.GetStagingPath());

    Array<byte> read;
    ContentHash readHash;
    REQUIRE_FALSE(context.ReadInput(TEXT("Content/source.synthetic"), read, readHash, diagnostic));
    CHECK(readHash == sourceHash);
    CHECK(context.ReadInput(TEXT("Content/not-declared.synthetic"), read, readHash, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);

    ArtifactWriter writer;
    REQUIRE_FALSE(context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic));
    const char outputBytes[] = "artifact";
    CHECK(writer.WriteFile(TEXT("../escape.flax"), outputBytes, 8, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::LibraryPathInvalid);
    CHECK(writer.WriteFile(root / TEXT("final.flax"), outputBytes, 8, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::LibraryPathInvalid);
    REQUIRE_FALSE(writer.WriteFile(TEXT("nested/result.flax"), outputBytes, 8, diagnostic));
    REQUIRE(context.GetFiles().Count() == 1);
    CHECK(FileSystem::FileExists(context.GetFiles()[0].AbsolutePath));
    CHECK(context.GetFiles()[0].Hash == ContentHash::Compute(outputBytes, 8));
    REQUIRE_FALSE(context.Close(diagnostic));
    CHECK(context.IsClosed());
    CHECK(writer.WriteFile(TEXT("late.flax"), outputBytes, 8, diagnostic));
}

TEST_CASE("ArtifactBuildContext enforces quotas and cancellation cleanup")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactBuildLimits-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = content / TEXT("source.synthetic");
    const char sourceBytes[] = "source";
    REQUIRE_FALSE(File::WriteAllBytes(source, sourceBytes, 6));
    PreparedAsset prepared = BuildContextPrepared(TEXT("Content/source.synthetic"), ContentHash::Compute(sourceBytes, 6));
    ArtifactBuildInput input;
    input.StableIdentity = TEXT("Content/source.synthetic");
    input.Path = source;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    AssetCancellationSource cancellation;
    ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken(), 4, 4, 1);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(context.Initialize(diagnostic));
    const String staging = context.GetStagingPath();
    Array<byte> read;
    ContentHash hash;
    CHECK(context.ReadInput(TEXT("Content/source.synthetic"), read, hash, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);
    ArtifactWriter writer;
    REQUIRE_FALSE(context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic));
    const char tooLarge[] = "large";
    CHECK(writer.WriteFile(TEXT("result.flax"), tooLarge, 5, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);
    cancellation.Cancel();
    CHECK(writer.WriteFile(TEXT("cancelled.flax"), tooLarge, 1, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCancelled);
    context.Cancel();
    CHECK_FALSE(FileSystem::DirectoryExists(staging));
}

TEST_CASE("ArtifactBuildContext streams validated temporary outputs")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactBuildFileOutput-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    PreparedAsset prepared = BuildContextPrepared(TEXT("Content/source.synthetic"), ContentHash::Compute("source", 6));
    Array<ArtifactBuildInput> inputs;
    AssetCancellationSource cancellation;
    ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken(), 1024, 1024, 4);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(context.Initialize(diagnostic));
    const String transferRoot = ArtifactStore::GetTemporaryPath(library) / TEXT("CallbackWorkers/Test");
    REQUIRE_FALSE(FileSystem::CreateDirectory(transferRoot));
    const String transfer = transferRoot / TEXT("runtime.bin");
    const char outputBytes[] = "streamed";
    REQUIRE_FALSE(File::WriteAllBytes(transfer, outputBytes, 8));
    const ContentHash outputHash = ContentHash::Compute(outputBytes, 8);
    const String outsideTransfer = root / TEXT("outside.bin");
    REQUIRE_FALSE(File::WriteAllBytes(outsideTransfer, outputBytes, 8));

    ArtifactWriter writer;
    REQUIRE_FALSE(context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic));
    CHECK(writer.WriteFileFromPath(TEXT("outside.bin"), outsideTransfer, 8, outputHash, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    REQUIRE_FALSE(writer.WriteFileFromPath(TEXT("runtime.bin"), transfer, 8, outputHash, diagnostic));
    REQUIRE(context.GetFiles().Count() == 1);
    CHECK(context.GetFiles()[0].Size == 8);
    CHECK(context.GetFiles()[0].Hash == outputHash);
    CHECK(FileSystem::FileExists(context.GetFiles()[0].AbsolutePath));
    CHECK(writer.WriteFileFromPath(TEXT("invalid.bin"), transfer, 8, ContentHash::Compute("wrong", 5), diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
    REQUIRE_FALSE(context.Close(diagnostic));
}

TEST_CASE("ArtifactBuildContext rejects undeclared input capabilities during initialization")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactBuildUndeclared-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    PreparedAsset prepared = BuildContextPrepared(TEXT("Content/source.synthetic"), ContentHash::Compute("source", 6));
    ArtifactBuildInput input;
    input.StableIdentity = TEXT("Content/other.synthetic");
    input.Path = content / TEXT("other.synthetic");
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    AssetCancellationSource cancellation;
    ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
    AssetPipelineDiagnostic diagnostic;
    CHECK(context.Initialize(diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    CHECK_FALSE(context.IsInitialized());
}
