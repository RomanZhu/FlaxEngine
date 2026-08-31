// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    PreparedAsset PublicationPrepared(const byte* sourceBytes, int32 sourceLength, int32 fingerprintSalt = 0)
    {
        PreparedAsset prepared;
        prepared.ObjectID = AssetObjectId::Main(AssetGuid(Guid(1, 2, 3, 4)));
        prepared.AssetID = prepared.ObjectID.ToRuntimeObjectGuid();
        prepared.OutputType = TEXT("Tests.PublishedAsset");
        prepared.DatabaseRevision = 12;
        prepared.SettingsHash = ContentHash::Compute("settings", 8);
        prepared.InputFingerprint = ArtifactKey(ContentHash::Compute(&fingerprintSalt, sizeof(fingerprintSalt)));
        AssetDependency source;
        source.Kind = AssetDependencyKind::SourceFile;
        source.StableIdentity = TEXT("Content/source.publish");
        source.Content = ContentHash::Compute(sourceBytes, sourceLength);
        source.Origin.Path = source.StableIdentity;
        prepared.Dependencies.Add(source);
        DeclaredArtifactOutput output;
        output.Kind = "runtime";
        output.Extension = ".bin";
        output.FormatVersion = 2;
        output.TargetDimensions = ArtifactTargetDimension::Platform;
        output.CompatibilityTag = "Tests.Runtime.v2";
        output.EffectiveAssetID = prepared.AssetID;
        prepared.Outputs.Add(output);
        return prepared;
    }

    ArtifactPublicationRequest PublicationRequest(const PreparedAsset& prepared, const ArtifactKey& outputKey, bool& notified)
    {
        ArtifactPublicationRequest request;
        request.Target.Platform = "Windows";
        request.Target.Architecture = "x64";
        request.Target.Role = "Editor";
        request.ProcessorID = TEXT("Tests.Publisher");
        request.ProcessorImplementationVersion = 3;
        request.BuildID = TEXT("publish-test");
        request.BuiltAtUtc = TEXT("2026-08-20T12:00:00Z");
        ArtifactPublicationOutputPlan output;
        output.Kind = "runtime";
        output.Key = outputKey;
        request.Outputs.Add(output);
        request.QueryCurrentState = [&prepared](uint64& revision, ArtifactKey& fingerprint)
        {
            revision = prepared.DatabaseRevision;
            fingerprint = prepared.InputFingerprint;
        };
        request.Notify = [&notified](const ArtifactManifest&) { notified = true; };
        return request;
    }

    bool AcceptPublishedBytes(const StringView& path, const ArtifactManifestOutput& output, AssetPipelineDiagnostic& diagnostic)
    {
        diagnostic = AssetPipelineDiagnostic();
        return !FileSystem::FileExists(path) || FileSystem::GetFileSize(path) != output.Size;
    }

    bool RejectPublishedBytes(const StringView&, const ArtifactManifestOutput&, AssetPipelineDiagnostic& diagnostic)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Message = TEXT("Synthetic validator rejection.");
        return true;
    }

    void AddStagedOutput(ArtifactBuildContext& context, const byte* bytes, int32 length)
    {
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(context.Initialize(diagnostic));
        ArtifactWriter writer;
        REQUIRE_FALSE(context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic));
        REQUIRE_FALSE(writer.WriteFile(TEXT("result.bin"), bytes, length, diagnostic));
    }

    void AddStagedAuxiliaryOutput(ArtifactBuildContext& context, const byte* bytes, int32 length)
    {
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(context.Initialize(diagnostic));
        ArtifactWriter writer;
        REQUIRE_FALSE(context.OpenOutput(StringAnsiView("auxiliary"), writer, diagnostic));
        REQUIRE_FALSE(writer.WriteFile(TEXT("auxiliary.bin"), bytes, length, diagnostic));
    }
}

TEST_CASE("ArtifactPublisher atomically selects validated immutable outputs")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactPublisher-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String sourcePath = content / TEXT("source.publish");
    const byte sourceBytes[] = { 1, 2, 3 };
    const byte outputBytes[] = { 4, 5, 6, 7 };
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, sourceBytes, ARRAY_COUNT(sourceBytes)));
    PreparedAsset prepared = PublicationPrepared(sourceBytes, ARRAY_COUNT(sourceBytes), 1);
    ArtifactBuildInput input;
    input.StableIdentity = TEXT("Content/source.publish");
    input.Path = sourcePath;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    AssetCancellationSource cancellation;
    ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
    AddStagedOutput(context, outputBytes, ARRAY_COUNT(outputBytes));
    ArtifactOutputValidatorRegistry validators;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(validators.Register(StringAnsiView("runtime"), StringView::Empty, &AcceptPublishedBytes, diagnostic));
    bool notified = false;
    const ArtifactKey outputKey(ContentHash::Compute("output-key", 10));
    ArtifactPublicationRequest request = PublicationRequest(prepared, outputKey, notified);
    ArtifactPublicationResult result;
    REQUIRE_FALSE(ArtifactPublisher::Publish(library, prepared, context, request, validators, result, diagnostic));
    CHECK(notified);
    CHECK(result.NotificationSent);
    REQUIRE(result.Manifest.Outputs.Count() == 1);
    ArtifactStoragePath outputPath;
    REQUIRE_FALSE(ArtifactStore::TryResolveLibraryRelative(library, result.Manifest.Outputs[0].RelativePath, outputPath, diagnostic));
    CHECK(FileSystem::FileExists(outputPath.Get()));
    ArtifactStoragePath manifestPath;
    REQUIRE_FALSE(ArtifactStore::TryGetManifestPath(library, request.Target, prepared.AssetID, manifestPath, diagnostic));
    StringAnsi manifestJson;
    REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), manifestJson));
    ArtifactManifest parsed;
    REQUIRE_FALSE(ArtifactManifest::Parse(manifestJson, manifestPath.Get(), parsed, diagnostic));
    CHECK(parsed.InputFingerprint == prepared.InputFingerprint);

    // An exact duplicate verifies and reuses the immutable destination.
    ArtifactBuildContext duplicate(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
    AddStagedOutput(duplicate, outputBytes, ARRAY_COUNT(outputBytes));
    notified = false;
    REQUIRE_FALSE(ArtifactPublisher::Publish(library, prepared, duplicate, request, validators, result, diagnostic));
    CHECK(notified);

    // An independently published output merges into the same exact manifest.
    PreparedAsset auxiliaryPrepared = prepared;
    DeclaredArtifactOutput auxiliaryOutput = auxiliaryPrepared.Outputs[0];
    auxiliaryOutput.Kind = "auxiliary";
    auxiliaryOutput.CompatibilityTag = "Tests.Auxiliary.v1";
    auxiliaryPrepared.Outputs.Clear();
    auxiliaryPrepared.Outputs.Add(auxiliaryOutput);
    const byte auxiliaryBytes[] = { 8, 9 };
    ArtifactBuildContext auxiliaryContext(root, content, library, Guid::New(), auxiliaryPrepared, inputs, cancellation.GetToken());
    AddStagedAuxiliaryOutput(auxiliaryContext, auxiliaryBytes, ARRAY_COUNT(auxiliaryBytes));
    REQUIRE_FALSE(validators.Register(StringAnsiView("auxiliary"), StringView::Empty, &AcceptPublishedBytes, diagnostic));
    ArtifactPublicationRequest auxiliaryRequest = PublicationRequest(auxiliaryPrepared, ArtifactKey(ContentHash::Compute("auxiliary-key", 13)), notified);
    auxiliaryRequest.Outputs[0].Kind = "auxiliary";
    REQUIRE_FALSE(ArtifactPublisher::Publish(library, auxiliaryPrepared, auxiliaryContext, auxiliaryRequest, validators, result, diagnostic));
    REQUIRE(result.Manifest.Outputs.Count() == 2);
    bool hasRuntime = false;
    bool hasAuxiliary = false;
    for (const ArtifactManifestOutput& output : result.Manifest.Outputs)
    {
        hasRuntime |= output.Kind == StringAnsiView("runtime");
        hasAuxiliary |= output.Kind == StringAnsiView("auxiliary");
    }
    CHECK(hasRuntime);
    CHECK(hasAuxiliary);

    // Corruption at an existing immutable key is rejected rather than overwritten.
    const byte corrupt[] = { 9, 9, 9, 9 };
    REQUIRE_FALSE(File::WriteAllBytes(outputPath.Get(), corrupt, ARRAY_COUNT(corrupt)));
    ArtifactBuildContext corruptAttempt(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
    AddStagedOutput(corruptAttempt, outputBytes, ARRAY_COUNT(outputBytes));
    CHECK(ArtifactPublisher::Publish(library, prepared, corruptAttempt, request, validators, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
}

TEST_CASE("ArtifactPublisher preserves current manifests across injected failures and supersession")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactPublisherFailures-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String sourcePath = content / TEXT("source.publish");
    const byte sourceBytes[] = { 1, 2, 3 };
    const byte outputBytes[] = { 7, 8, 9 };
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, sourceBytes, ARRAY_COUNT(sourceBytes)));
    ArtifactBuildInput input;
    input.StableIdentity = TEXT("Content/source.publish");
    input.Path = sourcePath;
    Array<ArtifactBuildInput> inputs;
    inputs.Add(input);
    AssetCancellationSource cancellation;
    ArtifactOutputValidatorRegistry validators;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(validators.Register(StringAnsiView("runtime"), StringView::Empty, &AcceptPublishedBytes, diagnostic));

    PreparedAsset baselinePrepared = PublicationPrepared(sourceBytes, ARRAY_COUNT(sourceBytes), 10);
    ArtifactBuildContext baselineContext(root, content, library, Guid::New(), baselinePrepared, inputs, cancellation.GetToken());
    AddStagedOutput(baselineContext, outputBytes, ARRAY_COUNT(outputBytes));
    bool notified = false;
    ArtifactPublicationRequest baselineRequest = PublicationRequest(baselinePrepared, ArtifactKey(ContentHash::Compute("baseline", 8)), notified);
    ArtifactPublicationResult result;
    REQUIRE_FALSE(ArtifactPublisher::Publish(library, baselinePrepared, baselineContext, baselineRequest, validators, result, diagnostic));
    ArtifactStoragePath manifestPath;
    REQUIRE_FALSE(ArtifactStore::TryGetManifestPath(library, baselineRequest.Target, baselinePrepared.AssetID, manifestPath, diagnostic));
    StringAnsi baselineJson;
    REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), baselineJson));

    const ArtifactPublicationFailurePoint beforeReplace[] =
    {
        ArtifactPublicationFailurePoint::AfterOutputClose,
        ArtifactPublicationFailurePoint::AfterOutputValidation,
        ArtifactPublicationFailurePoint::AfterFirstImmutableMove,
        ArtifactPublicationFailurePoint::AfterAllImmutableMoves,
        ArtifactPublicationFailurePoint::AfterManifestTempWrite,
        ArtifactPublicationFailurePoint::AfterManifestFlush,
        ArtifactPublicationFailurePoint::BeforeAtomicReplace,
    };
    for (int32 i = 0; i < ARRAY_COUNT(beforeReplace); i++)
    {
        PreparedAsset prepared = PublicationPrepared(sourceBytes, ARRAY_COUNT(sourceBytes), 20 + i);
        ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
        AddStagedOutput(context, outputBytes, ARRAY_COUNT(outputBytes));
        notified = false;
        ArtifactPublicationRequest request = PublicationRequest(prepared, ArtifactKey(ContentHash::Compute(&i, sizeof(i))), notified);
        request.FailurePoint = beforeReplace[i];
        CHECK(ArtifactPublisher::Publish(library, prepared, context, request, validators, result, diagnostic));
        CHECK_FALSE(notified);
        StringAnsi currentJson;
        REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), currentJson));
        CHECK(currentJson == baselineJson);
    }

    PreparedAsset superseded = PublicationPrepared(sourceBytes, ARRAY_COUNT(sourceBytes), 100);
    ArtifactBuildContext supersededContext(root, content, library, Guid::New(), superseded, inputs, cancellation.GetToken());
    AddStagedOutput(supersededContext, outputBytes, ARRAY_COUNT(outputBytes));
    ArtifactPublicationRequest supersededRequest = PublicationRequest(superseded, ArtifactKey(ContentHash::Compute("superseded", 10)), notified);
    supersededRequest.QueryCurrentState = [&superseded](uint64& revision, ArtifactKey& fingerprint)
    {
        revision = superseded.DatabaseRevision + 1;
        fingerprint = superseded.InputFingerprint;
    };
    REQUIRE_FALSE(ArtifactPublisher::Publish(library, superseded, supersededContext, supersededRequest, validators, result, diagnostic));
    CHECK(result.WasSuperseded);
    StringAnsi currentJson;
    REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), currentJson));
    CHECK(currentJson == baselineJson);

    ArtifactOutputValidatorRegistry rejecting;
    REQUIRE_FALSE(rejecting.Register(StringAnsiView("runtime"), StringView::Empty, &RejectPublishedBytes, diagnostic));
    PreparedAsset rejected = PublicationPrepared(sourceBytes, ARRAY_COUNT(sourceBytes), 101);
    ArtifactBuildContext rejectedContext(root, content, library, Guid::New(), rejected, inputs, cancellation.GetToken());
    AddStagedOutput(rejectedContext, outputBytes, ARRAY_COUNT(outputBytes));
    ArtifactPublicationRequest rejectedRequest = PublicationRequest(rejected, ArtifactKey(ContentHash::Compute("rejected", 8)), notified);
    CHECK(ArtifactPublisher::Publish(library, rejected, rejectedContext, rejectedRequest, rejecting, result, diagnostic));
    REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), currentJson));
    CHECK(currentJson == baselineJson);
}

TEST_CASE("ArtifactPublisher exposes only complete manifests after atomic replacement")
{
    const ArtifactPublicationFailurePoint afterReplace[] =
    {
        ArtifactPublicationFailurePoint::AfterAtomicReplaceBeforeNotification,
        ArtifactPublicationFailurePoint::DuringCleanup,
    };
    for (int32 i = 0; i < ARRAY_COUNT(afterReplace); i++)
    {
        const String root = Globals::TemporaryFolder / (TEXT("ArtifactPublisherAfterReplace-") + Guid::New().ToString(Guid::FormatType::N));
        const String content = root / TEXT("Content");
        const String library = root / TEXT("Library");
        REQUIRE_FALSE(FileSystem::CreateDirectory(content));
        REQUIRE_FALSE(FileSystem::CreateDirectory(library));
        SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
        const String sourcePath = content / TEXT("source.publish");
        const byte sourceBytes[] = { 1, 2, 3 };
        const byte outputBytes[] = { 4, 5, 6 };
        REQUIRE_FALSE(File::WriteAllBytes(sourcePath, sourceBytes, ARRAY_COUNT(sourceBytes)));
        PreparedAsset prepared = PublicationPrepared(sourceBytes, ARRAY_COUNT(sourceBytes), i + 200);
        ArtifactBuildInput input;
        input.StableIdentity = TEXT("Content/source.publish");
        input.Path = sourcePath;
        Array<ArtifactBuildInput> inputs;
        inputs.Add(input);
        AssetCancellationSource cancellation;
        ArtifactBuildContext context(root, content, library, Guid::New(), prepared, inputs, cancellation.GetToken());
        AddStagedOutput(context, outputBytes, ARRAY_COUNT(outputBytes));
        ArtifactOutputValidatorRegistry validators;
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(validators.Register(StringAnsiView("runtime"), StringView::Empty, &AcceptPublishedBytes, diagnostic));
        bool notified = false;
        ArtifactPublicationRequest request = PublicationRequest(prepared, ArtifactKey(ContentHash::Compute(&i, sizeof(i))), notified);
        request.FailurePoint = afterReplace[i];
        ArtifactPublicationResult result;
        CHECK(ArtifactPublisher::Publish(library, prepared, context, request, validators, result, diagnostic));
        ArtifactStoragePath manifestPath;
        REQUIRE_FALSE(ArtifactStore::TryGetManifestPath(library, request.Target, prepared.AssetID, manifestPath, diagnostic));
        StringAnsi json;
        REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), json));
        ArtifactManifest manifest;
        REQUIRE_FALSE(ArtifactManifest::Parse(json, manifestPath.Get(), manifest, diagnostic));
        CHECK(manifest.InputFingerprint == prepared.InputFingerprint);
    }
}
