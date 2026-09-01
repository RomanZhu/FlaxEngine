// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Artifacts/ProjectLibrary.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Project Library root")
{
    AssetPipelineDiagnostic diagnostic;
    String normalized;
    const String project = Globals::ProjectFolder;
    const String content = project / TEXT("Content");
    const String library = project / TEXT("Library");

    CHECK_FALSE(ProjectLibrary::ValidateRoot(project, content, library, normalized, diagnostic));
    CHECK(FileSystem::AreFilePathsEquivalent(normalized, library));
    CHECK_FALSE(FileSystem::IsRelative(normalized));
    CHECK(FileSystem::FileExists(project / TEXT(".gitignore")));

    String ignoreRules;
    REQUIRE_FALSE(File::ReadAllText(project / TEXT(".gitignore"), ignoreRules));
    CHECK(ignoreRules.Contains(TEXT("/Library/")));
    CHECK_FALSE(ignoreRules.Contains(TEXT("/Content/")));
    CHECK_FALSE(ignoreRules.Contains(TEXT("*.meta")));
    CHECK(FileSystem::DirectoryExists(ArtifactStore::GetArtifactsPath(library)));
    CHECK(FileSystem::DirectoryExists(ArtifactStore::GetManifestsPath(library)));
    CHECK(FileSystem::DirectoryExists(ArtifactStore::GetTemporaryPath(library)));
    CHECK(FileSystem::FileExists(library / TEXT("schema.version")));

    CHECK(ProjectLibrary::ValidateRoot(project, content, content / TEXT("Library"), normalized, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::LibraryPathInvalid);

    CHECK(ProjectLibrary::ValidateRoot(project, content, project / TEXT("../Library"), normalized, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::LibraryPathInvalid);
}

TEST_CASE("Project Library recovery and clean stay contained")
{
    const String project = Globals::TemporaryFolder / (TEXT("ProjectLibraryClean-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = project / TEXT("Content");
    const String library = project / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    String normalized;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ProjectLibrary::EnsureRoot(project, content, library, normalized, diagnostic));
    REQUIRE_FALSE(ArtifactStore::EnsureLayout(library, diagnostic));
    SCOPE_EXIT { FileSystem::DeleteDirectory(project, true); };
    const String interrupted = ArtifactStore::GetTemporaryPath(library) / TEXT("Interrupted");
    const String leasedPath = ArtifactStore::GetArtifactsPath(library) / TEXT("leased.bin");
    const String contentSentinel = content / TEXT("__LibraryCleanMustNotTouch.txt");
    const byte value = 7;
    FileSystem::DeleteDirectory(interrupted, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(interrupted));
    REQUIRE_FALSE(File::WriteAllBytes(leasedPath, &value, 1));
    REQUIRE_FALSE(File::WriteAllBytes(contentSentinel, &value, 1));
    SCOPE_EXIT
    {
        FileSystem::DeleteFile(contentSentinel);
        FileSystem::DeleteDirectory(interrupted, true);
    };

    CHECK_FALSE(ArtifactStore::Recover(library, diagnostic));
    CHECK_FALSE(FileSystem::DirectoryExists(interrupted));

    ArtifactLease lease = ArtifactLease::Acquire(leasedPath);
    CHECK(ArtifactStore::CleanEntireLibrary(diagnostic));
    CHECK(FileSystem::FileExists(leasedPath));
    CHECK(FileSystem::FileExists(contentSentinel));
    lease.Reset();

    CHECK_FALSE(ArtifactStore::CleanEntireLibrary(diagnostic));
    CHECK_FALSE(FileSystem::FileExists(leasedPath));
    CHECK(FileSystem::FileExists(contentSentinel));
    CHECK(FileSystem::DirectoryExists(ArtifactStore::GetArtifactsPath(library)));
    CHECK(FileSystem::FileExists(library / TEXT("schema.version")));
}

TEST_CASE("Project Library creation failure is diagnosed")
{
    const String root = Globals::TemporaryFolder / (TEXT("ProjectLibraryCreation-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(root, true);
    };

    const String blocker = root / TEXT("blocked");
    const String library = blocker / TEXT("Library");
    const byte data = 1;
    REQUIRE_FALSE(File::WriteAllBytes(blocker, &data, 1));

    AssetPipelineDiagnostic diagnostic;
    String normalized;
    CHECK(ProjectLibrary::EnsureRoot(root, root / TEXT("Content"), library, normalized, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::LibraryCreationFailed);
    CHECK_FALSE(FileSystem::DirectoryExists(library));
}
