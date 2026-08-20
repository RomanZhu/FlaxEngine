// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class TestPreparedPayload : public PreparedAssetPayload
    {
    public:
        uint64 Bytes;

        explicit TestPreparedPayload(uint64 bytes)
            : Bytes(bytes)
        {
        }

        uint64 GetMemoryUsage() const override
        {
            return Bytes;
        }
    };

    AssetProcessorDescriptor MakePreparedProcessor()
    {
        AssetProcessorDescriptor descriptor;
        descriptor.ID = TEXT("Tests.Prepared");
        descriptor.ProviderID = TEXT("tests");
        descriptor.MainOutputType = TEXT("FlaxEngine.RawDataAsset");
        descriptor.EngineApiLevel = 1;
        descriptor.SettingsSchemaVersion = 2;
        descriptor.ImplementationVersion = 3;
        AssetProcessorOutputDescriptor output;
        output.Kind = "runtime";
        output.Extension = ".flax";
        output.FormatVersion = 4;
        output.TargetDimensions = ArtifactTargetDimension::Platform;
        descriptor.Outputs.Add(output);
        return descriptor;
    }

    AssetRecord MakePreparedRecord(const String& source)
    {
        AssetRecord record;
        record.ID = Guid(1, 2, 3, 4);
        record.SourceAssetID = record.ID;
        record.TypeName = TEXT("FlaxEngine.RawDataAsset");
        record.CanonicalPath = CanonicalAssetPath(source);
        record.SourcePath = SourceFilePath(source);
        record.ProcessorID = TEXT("Tests.Prepared");
        record.SourceKind = AssetSourceKind::ImportedSource;
        record.DatabaseRevision = 17;
        return record;
    }

    bool PrepareOneSource(const String& root, const String& content, const String& library, const String& source,
        SourceHashCache& cache, const AssetCancellationToken& cancellation, const Guid& runtimeReference,
        const AssetSemanticInterface& buildInterface, uint64 maximumBytes, PreparedAsset& result, AssetPipelineDiagnostic& diagnostic)
    {
        AssetRecord record = MakePreparedRecord(source);
        AssetProcessorDescriptor descriptor = MakePreparedProcessor();
        PrepareAssetContext context(root, content, library, record, descriptor, StringAnsiView("{\"quality\":1}\n"), cache, cancellation, maximumBytes, 8);
        Array<byte> data;
        ContentHash hash;
        AssetDependencyOrigin origin;
        origin.Path = source;
        if (context.ReadSourceFile(source, data, hash, origin, diagnostic))
            return true;
        if (runtimeReference.IsValid() && context.DeclareRuntimeReference(TEXT("runtime-reference"), runtimeReference, origin, diagnostic))
            return true;
        if (!buildInterface.Hash.IsZero() && context.DeclareBuildInput(TEXT("build-interface"), Guid(5, 6, 7, 8), ArtifactKey(), buildInterface, origin, diagnostic))
            return true;
        if (context.DeclareOutput(StringAnsiView("runtime"), Guid::Empty, diagnostic))
            return true;
        return context.Finalize(record.DatabaseRevision, result, diagnostic);
    }
}

TEST_CASE("PrepareAssetContext records only controlled deterministic inputs")
{
    const String root = Globals::TemporaryFolder / (TEXT("PreparedAsset-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = content / TEXT("source.synthetic");
    const char firstBytes[] = "source-one";
    REQUIRE_FALSE(File::WriteAllBytes(source, firstBytes, 10));

    SourceHashCache cache;
    AssetCancellationSource cancellation;
    AssetSemanticInterface interfaceValue;
    interfaceValue.Version = 2;
    interfaceValue.Hash = ContentHash::Compute("surface", 7);
    AssetPipelineDiagnostic diagnostic;
    PreparedAsset first;
    REQUIRE_FALSE(PrepareOneSource(root, content, library, source, cache, cancellation.GetToken(), Guid(10, 11, 12, 13), interfaceValue, 1024, first, diagnostic));
    REQUIRE(first.Dependencies.Count() == 3);
    CHECK(first.Outputs.Count() == 1);
    CHECK(first.DatabaseRevision == 17);
    CHECK_FALSE(first.InputFingerprint.IsZero());

    PreparedAsset repeated;
    REQUIRE_FALSE(PrepareOneSource(root, content, library, source, cache, cancellation.GetToken(), Guid(20, 21, 22, 23), interfaceValue, 1024, repeated, diagnostic));
    CHECK(repeated.InputFingerprint == first.InputFingerprint);

    AssetSemanticInterface changedInterface = interfaceValue;
    changedInterface.Hash = ContentHash::Compute("surface-changed", 15);
    PreparedAsset interfaceChanged;
    REQUIRE_FALSE(PrepareOneSource(root, content, library, source, cache, cancellation.GetToken(), Guid(20, 21, 22, 23), changedInterface, 1024, interfaceChanged, diagnostic));
    CHECK(interfaceChanged.InputFingerprint != first.InputFingerprint);

    const char changedBytes[] = "source-two";
    REQUIRE_FALSE(File::WriteAllBytes(source, changedBytes, 10));
    PreparedAsset sourceChanged;
    REQUIRE_FALSE(PrepareOneSource(root, content, library, source, cache, cancellation.GetToken(), Guid(10, 11, 12, 13), interfaceValue, 1024, sourceChanged, diagnostic));
    CHECK(sourceChanged.InputFingerprint != first.InputFingerprint);
}

TEST_CASE("PrepareAssetContext rejects unsafe, stale, cancelled, and oversized work")
{
    const String root = Globals::TemporaryFolder / (TEXT("PreparedAssetFailures-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = content / TEXT("source.synthetic");
    const String outside = root / TEXT("outside.synthetic");
    const char bytes[] = "source";
    REQUIRE_FALSE(File::WriteAllBytes(source, bytes, 6));
    REQUIRE_FALSE(File::WriteAllBytes(outside, bytes, 6));

    SourceHashCache cache;
    AssetRecord record = MakePreparedRecord(source);
    AssetProcessorDescriptor descriptor = MakePreparedProcessor();
    AssetDependencyOrigin origin;
    AssetPipelineDiagnostic diagnostic;
    Array<byte> data;
    ContentHash hash;

    AssetCancellationSource active;
    PrepareAssetContext unsafe(root, content, library, record, descriptor, StringAnsiView("{}\n"), cache, active.GetToken());
    CHECK(unsafe.ReadSourceFile(outside, data, hash, origin, diagnostic));
    CHECK(diagnostic.Code != AssetPipelineDiagnosticCode::None);
    CHECK(diagnostic.Stage == AssetPipelineDiagnosticStage::Prepare);

    PrepareAssetContext limited(root, content, library, record, descriptor, StringAnsiView("{}\n"), cache, active.GetToken(), 2, 8);
    CHECK(limited.ReadSourceFile(source, data, hash, origin, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);

    PrepareAssetContext stale(root, content, library, record, descriptor, StringAnsiView("{}\n"), cache, active.GetToken());
    REQUIRE_FALSE(stale.ReadSourceFile(source, data, hash, origin, diagnostic));
    REQUIRE_FALSE(stale.DeclareOutput(StringAnsiView("runtime"), Guid::Empty, diagnostic));
    PreparedAsset prepared;
    prepared.Payload = std::make_shared<TestPreparedPayload>(64);
    CHECK(stale.Finalize(record.DatabaseRevision + 1, prepared, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);

    AssetCancellationSource cancelled;
    cancelled.Cancel();
    PrepareAssetContext stopped(root, content, library, record, descriptor, StringAnsiView("{}\n"), cache, cancelled.GetToken());
    CHECK(stopped.ReadSourceFile(source, data, hash, origin, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCancelled);

    PrepareAssetContext memoryLimited(root, content, library, record, descriptor, StringAnsiView("{}\n"), cache, active.GetToken(), 32, 8);
    REQUIRE_FALSE(memoryLimited.ReadSourceFile(source, data, hash, origin, diagnostic));
    REQUIRE_FALSE(memoryLimited.DeclareOutput(StringAnsiView("runtime"), Guid::Empty, diagnostic));
    prepared = PreparedAsset();
    prepared.Payload = std::make_shared<TestPreparedPayload>(64);
    CHECK(memoryLimited.Finalize(record.DatabaseRevision, prepared, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);
}
