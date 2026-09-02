// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#include "Engine/Scripting/Scripting.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    struct ColdIndexSource
    {
        String Path;
        Guid MainId;
        Guid ChildId;
        String Processor;
        String Type;
    };

    void WriteColdIndexSource(const ColdIndexSource& source)
    {
        const char payload[] = "reduced production validation source";
        REQUIRE_FALSE(File::WriteAllBytes(source.Path, payload, ARRAY_COUNT(payload) - 1));

        AssetMeta meta;
        meta.ID = source.MainId;
        meta.AssetType = source.Type;
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = source.Processor;
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}";
        if (source.ChildId.IsValid())
        {
            SubAssetMeta child;
            child.ID = source.ChildId;
            child.LocalId = 2;
            child.TypeName = TEXT("FlaxEngine.Model");
            child.DisplayName = TEXT("FixtureTriangle");
            meta.SubAssets.Add(TEXT("mesh:/FixtureTriangle"), child);
        }
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(AssetMeta::SaveAtomic(source.Path + TEXT(".meta"), meta, diagnostic));
    }

    void RequireCohortIdentity(AssetDatabase& database, const Array<ColdIndexSource>& sources)
    {
        for (const ColdIndexSource& source : sources)
        {
            AssetRecord record;
            REQUIRE(database.TryGetRecord(source.MainId, record));
            CHECK(record.ID == source.MainId);
            CHECK(record.SourceAssetID == source.MainId);
            CHECK(record.ProcessorID == source.Processor);
            CHECK(record.TypeName == source.Type);
            CHECK(record.Status == AssetRecordStatus::Ready);
            if (source.ChildId.IsValid())
            {
                REQUIRE(database.TryGetRecord(source.ChildId, record));
                CHECK(record.SourceAssetID == source.MainId);
                CHECK(record.SubAsset.Get() == TEXT("mesh:/FixtureTriangle"));
                CHECK(record.Status == AssetRecordStatus::Ready);
            }
        }
    }
}

TEST_CASE("Empty Library rebuild restores authored index identity from canonical sources")
{
    const String root = Globals::TemporaryFolder / (Guid::New().ToString(Guid::FormatType::N) + TEXT("-Asset81"));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    Array<ColdIndexSource> sources;
    sources.Add({ content / TEXT("Fixture.png"), Guid::New(), Guid::Empty,
        TEXT("Flax.Texture"), TEXT("FlaxEngine.Texture") });
    sources.Add({ content / TEXT("Fixture.glb"), Guid::New(), Guid::New(),
        TEXT("Flax.Model"), TEXT("FlaxEngine.Model") });
    sources.Add({ content / TEXT("Fixture.prefab"), Guid::New(), Guid::Empty,
        TEXT("Flax.JsonDocument"), TEXT("FlaxEngine.Prefab") });
    for (const ColdIndexSource& source : sources)
        WriteColdIndexSource(source);

    const Guid projectId = Guid::New();
    AssetPipelineDiagnostic diagnostic;
    AssetDatabaseScanOptions options;
    options.StrictMetadata = true;
    AssetDatabaseScanResult scan;

    // The first scan starts with no Library and establishes the canonical identity cohort.
    REQUIRE_FALSE(FileSystem::DirectoryExists(library));
    AssetDatabase cold;
    REQUIRE_FALSE(cold.Open(library, projectId, diagnostic));
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, cold, scan));
    CHECK(scan.Diagnostics.IsEmpty());
    RequireCohortIdentity(cold, sources);
    const int32 coldRecordCount = cold.GetSnapshot().Records.Count();
    REQUIRE_FALSE(cold.Close(&diagnostic));

    // Removing only Library forces a complete index rebuild from unchanged canonical sources.
    REQUIRE_FALSE(FileSystem::DeleteDirectory(library, true));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    AssetDatabase rebuilt;
    REQUIRE_FALSE(rebuilt.Open(library, projectId, diagnostic));
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, rebuilt, scan));
    CHECK(scan.Diagnostics.IsEmpty());
    RequireCohortIdentity(rebuilt, sources);
    CHECK(rebuilt.GetSnapshot().Records.Count() == coldRecordCount);
    const uint64 rebuiltRevision = rebuilt.GetRevision();
    REQUIRE_FALSE(rebuilt.Close(&diagnostic));

    // Reopen from only the newly rebuilt durable Library without injecting in-memory hash state.
    AssetDatabase restarted;
    REQUIRE_FALSE(restarted.Open(library, projectId, diagnostic));
    CHECK(restarted.GetRevision() == rebuiltRevision);
    RequireCohortIdentity(restarted, sources);
    CHECK(restarted.GetSnapshot().Records.Count() == coldRecordCount);
    REQUIRE_FALSE(restarted.Close(&diagnostic));
}

TEST_CASE("Real reduced imports survive restart and byte-identical remove re-add from cache")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestColdIndexCacheValidation");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunRealImportsSurviveRestartAndUnchangedReAdd", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestColdIndexCacheValidation"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

#endif
