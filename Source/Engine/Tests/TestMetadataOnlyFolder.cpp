// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

#if COMPILE_WITH_TESTS && USE_EDITOR

#if USE_CSHARP && USE_NETCORE
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#endif

TEST_CASE("Asset database materializes a metadata-only folder")
{
    const String root = Globals::TemporaryFolder / (TEXT("MetadataOnlyFolder-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    const String folder = content / TEXT("Recovered");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.FolderAsset = true;
    meta.SourceKind = AssetSourceKind::Folder;
    meta.Processor.ID = TEXT("Flax.Folder");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}";
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(folder + TEXT(".meta"), meta, diagnostic));
    REQUIRE_FALSE(FileSystem::DirectoryExists(folder));

    Array<String> files;
    files.Add(folder + TEXT(".meta"));
    Array<AssetRecord> records;
    AssetDatabaseScanResult result;
    AssetDatabaseScanOptions options;
    AssetDatabaseSnapshot previous;
    REQUIRE_FALSE(AssetDatabaseScanner::CollectFromFiles(root, content, library, files, options, previous, records, result));

    CHECK(FileSystem::DirectoryExists(folder));
    CHECK(result.Diagnostics.IsEmpty());
    REQUIRE(records.Count() == 1);
    CHECK(records[0].ID == meta.ID);
    CHECK(records[0].SourceKind == AssetSourceKind::Folder);
    CHECK(records[0].Status == AssetRecordStatus::Ready);
}

TEST_CASE("Project panel discovers and deletes a metadata-only folder")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestMetadataOnlyFolder");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunProjectPanelLifecycle", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestMetadataOnlyFolder"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

#endif
