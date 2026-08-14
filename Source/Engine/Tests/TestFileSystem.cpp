// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("FileSystem path comparison")
{
    const String root = Globals::TemporaryFolder / (TEXT("FileSystemPathComparison-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE(!FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(root, true);
    };

    const String fileA = root / TEXT("FileA.txt");
    const String fileB = root / TEXT("FileB.txt");
    const String missing = root / TEXT("Missing.txt");
    const byte contentsA[] = { 1, 2, 3 };
    const byte contentsB[] = { 4, 5, 6 };
    REQUIRE(!File::WriteAllBytes(fileA, contentsA, ARRAY_COUNT(contentsA)));
    REQUIRE(!File::WriteAllBytes(fileB, contentsB, ARRAY_COUNT(contentsB)));

    SECTION("Missing destination is never created")
    {
        CHECK(!FileSystem::AreFilePathsSame(fileA, missing));
        CHECK(!FileSystem::AreFilePathsEquivalent(fileA, missing));
        CHECK(!FileSystem::FileExists(missing));
        CHECK(!FileSystem::DirectoryExists(missing));
    }

    SECTION("Existing object identity")
    {
        String alternateFileA(fileA);
        alternateFileA.Replace('/', '\\');
        CHECK(FileSystem::AreFilePathsSame(fileA, alternateFileA));
        CHECK(!FileSystem::AreFilePathsSame(fileA, fileB));
        CHECK(FileSystem::AreFilePathsSame(root, root / TEXT(".")));
    }

    SECTION("Lexical equivalence follows platform case rules")
    {
        String alternateFileA(fileA);
        alternateFileA.Replace('/', '\\');
        CHECK(FileSystem::AreFilePathsEquivalent(fileA, alternateFileA));
#if PLATFORM_WINDOWS
        CHECK(FileSystem::AreFilePathsEquivalent(fileA, root / TEXT("filea.TXT")));
#else
        CHECK(!FileSystem::AreFilePathsEquivalent(fileA, root / TEXT("filea.TXT")));
#endif
    }
}
