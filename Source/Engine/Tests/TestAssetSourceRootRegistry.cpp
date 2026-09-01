// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetSourceRootRegistry.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    const AssetSourceRoot* FindRoot(const AssetSourceRootRegistry& registry, AssetSourceRootKind kind)
    {
        for (const AssetSourceRoot& root : registry.GetRoots())
        {
            if (root.Kind == kind)
                return &root;
        }
        return nullptr;
    }
}

TEST_CASE("Asset source root registry owns permissions visibility and logical paths")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetSourceRoots-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    const String externalActors = root / TEXT("ExternalActors");
    const String engineOwner = root / TEXT("EngineInstallation");
    const String engine = engineOwner / TEXT("Source/Editor/Assets");
    const String plugin = root / TEXT("PluginContent");
    const String external = root / TEXT("SharedContent");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(externalActors));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    REQUIRE_FALSE(FileSystem::CreateDirectory(engine));
    REQUIRE_FALSE(FileSystem::CreateDirectory(plugin));
    REQUIRE_FALSE(FileSystem::CreateDirectory(external));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetPipelineDiagnostic diagnostic;
    AssetSourceRootRegistry registry(root, library);
    REQUIRE_FALSE(registry.RegisterProjectRoots(content, diagnostic));
    REQUIRE_FALSE(registry.RegisterEngineRoot(engineOwner, engine, diagnostic));
    REQUIRE_FALSE(registry.RegisterPluginRoot(Guid(1, 2, 3, 4), TEXT("Tests"), plugin,
        TEXT("PluginContent/Tests"), diagnostic));
    REQUIRE_FALSE(registry.RegisterExternalReadOnlyRoot(Guid(5, 6, 7, 8), TEXT("Shared"), external,
        TEXT("External/Shared"), diagnostic));
    REQUIRE(registry.GetRoots().Count() == 5);

    const AssetSourceRoot* projectRoot = FindRoot(registry, AssetSourceRootKind::ProjectContent);
    const AssetSourceRoot* fragmentsRoot = FindRoot(registry, AssetSourceRootKind::SceneFragments);
    const AssetSourceRoot* engineRoot = FindRoot(registry, AssetSourceRootKind::EngineContent);
    const AssetSourceRoot* pluginRoot = FindRoot(registry, AssetSourceRootKind::PluginContent);
    const AssetSourceRoot* externalRoot = FindRoot(registry, AssetSourceRootKind::ExternalReadOnlyContent);
    REQUIRE(projectRoot);
    REQUIRE(fragmentsRoot);
    REQUIRE(engineRoot);
    REQUIRE(pluginRoot);
    REQUIRE(externalRoot);
    CHECK(projectRoot->RootId.IsValid());
    CHECK(projectRoot->RootId != fragmentsRoot->RootId);
    CHECK(pluginRoot->RootId == Guid(1, 2, 3, 4));
    CHECK(projectRoot->IsPublic());
    CHECK(projectRoot->PublicAssetNamespace);
    CHECK(projectRoot->BrowserVisible);
    CHECK(projectRoot->RequiresAdjacentMeta);
    CHECK(projectRoot->HasPermission(AssetSourceRootPermission::Write));
    CHECK(projectRoot->HasPermission(AssetSourceRootPermission::GenericMutation));
    CHECK_FALSE(fragmentsRoot->IsPublic());
    CHECK_FALSE(fragmentsRoot->PublicAssetNamespace);
    CHECK_FALSE(fragmentsRoot->BrowserVisible);
    CHECK_FALSE(fragmentsRoot->RequiresAdjacentMeta);
    CHECK(fragmentsRoot->HasPermission(AssetSourceRootPermission::Write));
    CHECK_FALSE(fragmentsRoot->HasPermission(AssetSourceRootPermission::Scan));
    CHECK_FALSE(engineRoot->HasPermission(AssetSourceRootPermission::Write));
    CHECK_FALSE(pluginRoot->HasPermission(AssetSourceRootPermission::Write));
    CHECK_FALSE(externalRoot->HasPermission(AssetSourceRootPermission::Write));

    AssetPathPolicy::ProjectPath writable;
    REQUIRE_FALSE(registry.ResolveForGenericMutation(TEXT("Content/Textures/Test.png"), writable, diagnostic));
    CHECK(writable.ProjectRelativePath == TEXT("Content/Textures/Test.png"));
    CHECK(AssetPathPolicy::IsSameOrChild(writable.AbsolutePath, content));

    ResolvedAssetSourcePath resolved;
    REQUIRE_FALSE(registry.Resolve(externalActors / TEXT("scene/actor.sceneactor"), resolved, diagnostic));
    CHECK(resolved.Root.Kind == AssetSourceRootKind::SceneFragments);
    CHECK(registry.Resolve(TEXT("ExternalActors/scene/actor.sceneactor"), resolved, diagnostic));
    CHECK(registry.ResolveForGenericMutation(externalActors / TEXT("scene/actor.sceneactor"), writable, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    CHECK(registry.ResolveForScan(externalActors / TEXT("scene/actor.sceneactor"), resolved, diagnostic));
    String privateLogical;
    CHECK(registry.PhysicalToLogical(externalActors / TEXT("scene/actor.sceneactor"), privateLogical, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);

    REQUIRE_FALSE(registry.ResolveForScan(TEXT("PluginContent/Tests/Model.fbx"), resolved, diagnostic));
    CHECK(resolved.Root.Kind == AssetSourceRootKind::PluginContent);
    CHECK(registry.ResolveForGenericMutation(TEXT("PluginContent/Tests/Model.fbx"), writable, diagnostic));
    REQUIRE_FALSE(registry.ResolveForScan(TEXT("External/Shared/Material.json"), resolved, diagnostic));
    CHECK(resolved.Root.Kind == AssetSourceRootKind::ExternalReadOnlyContent);
    CHECK(registry.ResolveForGenericMutation(TEXT("External/Shared/Material.json"), writable, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);

    String logical;
    REQUIRE_FALSE(registry.PhysicalToLogical(engine / TEXT("Editor/Icon.png"), logical, diagnostic));
    CHECK(logical == TEXT("Source/Editor/Assets/Editor/Icon.png"));
    String physical;
    REQUIRE_FALSE(registry.LogicalToPhysical(logical, physical, diagnostic));
    CHECK(FileSystem::AreFilePathsEqual(physical, engine / TEXT("Editor/Icon.png")));
}

TEST_CASE("Asset database scanner rejects the private ExternalActors root")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetSourceScannerRoots-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    const String externalActors = root / TEXT("ExternalActors");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(externalActors));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetDatabaseScanOptions options;
    AssetDatabaseSnapshot previous;
    Array<AssetRecord> records;
    AssetDatabaseScanResult result;
    CHECK(AssetDatabaseScanner::Collect(root, externalActors, library, options, previous, records, result));
    REQUIRE(result.Diagnostics.Count() == 1);
    CHECK(result.Diagnostics[0].Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    CHECK(records.IsEmpty());
}
