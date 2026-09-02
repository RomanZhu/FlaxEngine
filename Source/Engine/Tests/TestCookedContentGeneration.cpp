// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Build/CookedContentGeneration.h"
#include "Engine/Content/Build/RuntimeAssetCatalog.h"
#include "Engine/Content/AssetObjectRegistry.h"
#include "Engine/Core/Config/GameSettings.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Editor/Cooker/Steps/CookAssetsStep.h"
#include "Editor/Cooker/Steps/DeployDataStep.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    bool CreateCookedStage(const String& contentRoot, byte value, String& staging, AssetPipelineDiagnostic& diagnostic)
    {
        if (CookedContentGeneration::CreateStaging(contentRoot, Guid::New(), staging, diagnostic))
            return true;
        const String content = staging / TEXT("Content");
        const byte head[] = { value, 2, 3, 4 };
        const byte package[] = { value, 6, 7, 8, 9 };
        if (File::WriteAllBytes(content / TEXT("head"), head, ARRAY_COUNT(head)) ||
            File::WriteAllBytes(content / TEXT("Data_0.flaxpac"), package, ARRAY_COUNT(package)))
            return true;

        const Guid gameSettings(91, 92, 93, 94);
        RuntimeAssetCatalogEntry entry;
        entry.Object = gameSettings;
        entry.TypeName = "FlaxEngine.GameSettings";
        entry.PackageName = "Data_0.flaxpac";
        entry.Size = ARRAY_COUNT(package);
        entry.Content = ContentHash::Compute(package, ARRAY_COUNT(package));
        Array<RuntimeAssetCatalogEntry> entries;
        entries.Add(entry);
        RuntimeAssetCatalog catalog;
        if (catalog.Set(StringAnsiView("cooked-generation-test"), ContentHash::Compute("target", 6), entries, diagnostic))
            return true;
        catalog.SetGameSettingsObject(gameSettings);
        return catalog.SaveAtomic(content / TEXT("RuntimeAssetCatalog.bin"), diagnostic);
    }

    bool HasOnlyGeneration(const String& contentRoot, const ContentHash& generation)
    {
        Array<String> children;
        const String generations = CookedContentGeneration::GetGenerationsPath(contentRoot);
        return !FileSystem::GetChildDirectories(children, generations) && children.Count() == 1 &&
            FileSystem::AreFilePathsEquivalent(children[0], generations / String(generation.ToString()));
    }

    bool HasNoGenerations(const String& contentRoot)
    {
        Array<String> children;
        return !FileSystem::GetChildDirectories(children, CookedContentGeneration::GetGenerationsPath(contentRoot)) && children.IsEmpty();
    }
}

TEST_CASE("Cooked content staging creates the package Content parent")
{
    const String root = Globals::TemporaryFolder / (TEXT("CookedStageParent-") + Guid::New().ToString(Guid::FormatType::N));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    String staging;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(CookedContentGeneration::CreateStaging(root, Guid::New(), staging, diagnostic));
    CHECK(FileSystem::DirectoryExists(staging / TEXT("Content")));
}

TEST_CASE("Cooker roots scalar GameSettings references for the target platform")
{
    GameSettings settings;
    settings.Icon = Guid(1, 0, 0, 0);
    settings.SplashScreen = settings.Icon;
    settings.Time = Guid(2, 0, 0, 0);
    settings.Audio = Guid(3, 0, 0, 0);
    settings.LayersAndTags = Guid(4, 0, 0, 0);
    settings.Physics = Guid(5, 0, 0, 0);
    settings.Input = Guid(6, 0, 0, 0);
    settings.Graphics = Guid(7, 0, 0, 0);
    settings.Network = Guid(8, 0, 0, 0);
    settings.Navigation = Guid(9, 0, 0, 0);
    settings.Localization = Guid(10, 0, 0, 0);
    settings.GameCooking = Guid(11, 0, 0, 0);
    settings.Streaming = Guid(12, 0, 0, 0);
    settings.AssetPipeline = Guid(13, 0, 0, 0);
    settings.WindowsPlatform = Guid(14, 0, 0, 0);
    settings.LinuxPlatform = Guid(15, 0, 0, 0);
    const Guid custom(16, 0, 0, 0);
    settings.CustomSettings[TEXT("plugin")] = custom;

    Array<Guid> roots;
    DeployDataStep::CollectGameSettingsRoots(settings, BuildPlatform::Windows64, roots);
    CHECK(roots.Count() == 15);
    CHECK(roots.Contains(settings.Icon));
    CHECK(roots.Contains(settings.Time));
    CHECK(roots.Contains(settings.AssetPipeline));
    CHECK(roots.Contains(settings.WindowsPlatform));
    CHECK(roots.Contains(custom));
    CHECK_FALSE(roots.Contains(settings.LinuxPlatform));
    CHECK_FALSE(roots.Contains(Guid::Empty));
}

TEST_CASE("Cooked generation paths use stable logical Content aliases")
{
    const String startup = TEXT("C:/Game");
    const String content = startup / TEXT("Content/Generations/0123456789abcdef/Content");
    CHECK(AssetObjectRegistry::NormalizeRuntimePathAlias(
        content / TEXT("Shaders/GUI.flax"), startup, content) == TEXT("Content/Shaders/GUI.flax"));
    CHECK(AssetObjectRegistry::NormalizeRuntimePathAlias(
        content / TEXT("Editor/Fonts/Roboto-Regular.flax"), startup, content) == TEXT("Content/Editor/Fonts/Roboto-Regular.flax"));
    CHECK(AssetObjectRegistry::NormalizeRuntimePathAlias(
        startup / TEXT("Content/Legacy.flax"), startup, content) == TEXT("Content/Legacy.flax"));
}

TEST_CASE("Cooked streaming freshness copies only missing or older output")
{
    CHECK(CookedContentGeneration::ShouldCopyStreamingFile(false, DateTime(20), DateTime::MinValue()));
    CHECK(CookedContentGeneration::ShouldCopyStreamingFile(true, DateTime(20), DateTime(10)));
    CHECK_FALSE(CookedContentGeneration::ShouldCopyStreamingFile(true, DateTime(20), DateTime(20)));
    CHECK_FALSE(CookedContentGeneration::ShouldCopyStreamingFile(true, DateTime(20), DateTime(30)));
}

TEST_CASE("Cooker cache keys entries by persistent object GUID")
{
    const Guid subAsset(101, 202, 303, 404);
    const Guid mainAsset(501, 502, 503, 504);

    CookAssetsStep::CacheData cache;
    cache.CacheFolder = TEXT("Cache/CookedObjects");
    cache.Entries[subAsset].ID = subAsset;
    cache.Entries[mainAsset].ID = mainAsset;
    CHECK(cache.Entries.Count() == 2);

    String subAssetPath;
    String mainAssetPath;
    cache.GetFilePath(subAsset, subAssetPath);
    cache.GetFilePath(mainAsset, mainAssetPath);
    CHECK(subAssetPath != mainAssetPath);
    CHECK(subAssetPath.Contains(subAsset.ToString(Guid::FormatType::N)));
    CHECK(mainAssetPath.Contains(mainAsset.ToString(Guid::FormatType::N)));
}

TEST_CASE("Cooked content generation activation is atomic cancellable and deterministic")
{
    const String root = Globals::TemporaryFolder / (TEXT("CookedGeneration-") + Guid::New().ToString(Guid::FormatType::N));
    const String contentRoot = root / TEXT("Content");
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    REQUIRE_FALSE(FileSystem::CreateDirectory(contentRoot));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    String firstStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 1, firstStage, diagnostic));
    ContentHash firstGeneration;
    REQUIRE_FALSE(CookedContentGeneration::Publish(contentRoot, firstStage, firstGeneration, diagnostic));
    String activeContent;
    ContentHash activeGeneration;
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == firstGeneration);

    String interruptedStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 42, interruptedStage, diagnostic));
    ContentHash interruptedGeneration;
    CHECK(CookedContentGeneration::Publish(contentRoot, interruptedStage, interruptedGeneration, diagnostic,
        Function<bool()>(), CookedContentPublicationFailurePoint::AfterGenerationMove));
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == firstGeneration);
    CHECK(FileSystem::DirectoryExists(CookedContentGeneration::GetGenerationsPath(contentRoot) / String(interruptedGeneration.ToString())));

    String recoveryStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 42, recoveryStage, diagnostic));
    ContentHash recoveryGeneration;
    REQUIRE_FALSE(CookedContentGeneration::Publish(contentRoot, recoveryStage, recoveryGeneration, diagnostic));
    CHECK(recoveryGeneration == interruptedGeneration);
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == recoveryGeneration);

    String repeatedStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 42, repeatedStage, diagnostic));
    ContentHash repeatedGeneration;
    REQUIRE_FALSE(CookedContentGeneration::Publish(contentRoot, repeatedStage, repeatedGeneration, diagnostic));
    CHECK(repeatedGeneration == recoveryGeneration);

    String cancelledStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 99, cancelledStage, diagnostic));
    ContentHash cancelledGeneration;
    CHECK(CookedContentGeneration::Publish(contentRoot, cancelledStage, cancelledGeneration, diagnostic, [] { return true; }));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCancelled);
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == recoveryGeneration);
}

TEST_CASE("Cooked content deployment exposes only current data and restores reported failures")
{
    const String root = Globals::TemporaryFolder / (TEXT("CookedDeployment-") + Guid::New().ToString(Guid::FormatType::N));
    const String contentRoot = root / TEXT("Content");
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    REQUIRE_FALSE(FileSystem::CreateDirectory(contentRoot));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    String firstStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 1, firstStage, diagnostic));
    ContentHash firstGeneration;
    REQUIRE_FALSE(CookedContentGeneration::Publish(contentRoot, firstStage, firstGeneration, diagnostic));

    String historicalStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 2, historicalStage, diagnostic));
    ContentHash historicalGeneration;
    REQUIRE(CookedContentGeneration::Publish(contentRoot, historicalStage, historicalGeneration, diagnostic,
        Function<bool()>(), CookedContentPublicationFailurePoint::AfterGenerationMove));

    String abandonedStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 3, abandonedStage, diagnostic));
    String nextStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 4, nextStage, diagnostic));
    CookedContentDeploymentState failedDeployment;
    const String failedRollback = root / TEXT("ReportedFailureRollback");
    REQUIRE_FALSE(CookedContentGeneration::BeginDeployment(contentRoot, nextStage, failedRollback, failedDeployment, diagnostic));
    CHECK(HasOnlyGeneration(contentRoot, failedDeployment.NewGeneration));
    CHECK(FileSystem::DirectoryExists(failedRollback / String(firstGeneration.ToString())));
    CHECK_FALSE(FileSystem::DirectoryExists(CookedContentGeneration::GetGenerationsPath(contentRoot) / String(historicalGeneration.ToString())));
    CHECK_FALSE(FileSystem::DirectoryExists(abandonedStage));

    String activeContent;
    ContentHash activeGeneration;
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == failedDeployment.NewGeneration);

    REQUIRE_FALSE(CookedContentGeneration::RollbackDeployment(contentRoot, failedDeployment, diagnostic));
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == firstGeneration);
    CHECK(HasOnlyGeneration(contentRoot, firstGeneration));
    CHECK_FALSE(FileSystem::DirectoryExists(failedRollback));

    String successStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 4, successStage, diagnostic));
    CookedContentDeploymentState successfulDeployment;
    const String successRollback = root / TEXT("SuccessRollback");
    REQUIRE_FALSE(CookedContentGeneration::BeginDeployment(contentRoot, successStage, successRollback, successfulDeployment, diagnostic));
    const ContentHash successfulGeneration = successfulDeployment.NewGeneration;
    REQUIRE_FALSE(CookedContentGeneration::CommitDeployment(successfulDeployment, diagnostic));
    CHECK(HasOnlyGeneration(contentRoot, successfulGeneration));
    CHECK_FALSE(FileSystem::DirectoryExists(successRollback));

    String repeatedStage;
    REQUIRE_FALSE(CreateCookedStage(contentRoot, 4, repeatedStage, diagnostic));
    CookedContentDeploymentState repeatedDeployment;
    const String repeatedRollback = root / TEXT("RepeatedRollback");
    REQUIRE_FALSE(CookedContentGeneration::BeginDeployment(contentRoot, repeatedStage, repeatedRollback, repeatedDeployment, diagnostic));
    CHECK(repeatedDeployment.HadPreviousGeneration);
    CHECK_FALSE(repeatedDeployment.ActivationChanged);
    REQUIRE_FALSE(CookedContentGeneration::RollbackDeployment(contentRoot, repeatedDeployment, diagnostic));
    REQUIRE_FALSE(CookedContentGeneration::Resolve(contentRoot, activeContent, activeGeneration, diagnostic));
    CHECK(activeGeneration == successfulGeneration);
    CHECK(HasOnlyGeneration(contentRoot, successfulGeneration));

    const String firstContentRoot = root / TEXT("FirstContent");
    REQUIRE_FALSE(FileSystem::CreateDirectory(firstContentRoot));
    String firstCookStage;
    REQUIRE_FALSE(CreateCookedStage(firstContentRoot, 8, firstCookStage, diagnostic));
    CookedContentDeploymentState firstCookDeployment;
    REQUIRE_FALSE(CookedContentGeneration::BeginDeployment(firstContentRoot, firstCookStage, root / TEXT("FirstCookRollback"),
        firstCookDeployment, diagnostic));
    CHECK_FALSE(firstCookDeployment.HadPreviousGeneration);
    CHECK(HasOnlyGeneration(firstContentRoot, firstCookDeployment.NewGeneration));
    REQUIRE_FALSE(CookedContentGeneration::RollbackDeployment(firstContentRoot, firstCookDeployment, diagnostic));
    CHECK_FALSE(FileSystem::FileExists(CookedContentGeneration::GetCurrentGenerationPath(firstContentRoot)));
    CHECK(HasNoGenerations(firstContentRoot));
}

#endif
