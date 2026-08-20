// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Content/AssetDatabase/SourceHashCache.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("ContentHash matches SHA-256 vectors")
{
    struct Vector
    {
        const char* Input;
        const char* Expected;
    };
    const Vector vectors[] =
    {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "The quick brown fox jumps over the lazy dog", "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592" },
    };

    for (const Vector& vector : vectors)
    {
        const uint64 length = StringUtils::Length(vector.Input);
        const ContentHash digest = ContentHash::Compute(vector.Input, length);
        CHECK(digest.ToString() == vector.Expected);

        ContentHasher streaming;
        for (uint64 i = 0; i < length; i++)
            streaming.Update(vector.Input + i, 1);
        CHECK(streaming.Finalize() == digest);
    }
}

TEST_CASE("ContentHash canonical form parses safely")
{
    const StringAnsi canonical("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ContentHash value;
    REQUIRE_FALSE(ContentHash::Parse(canonical, value));
    CHECK(value.ToString() == canonical);

    ContentHash uppercase;
    REQUIRE_FALSE(ContentHash::Parse(StringAnsiView("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"), uppercase));
    CHECK(uppercase == value);
    CHECK(ContentHash::Parse(StringAnsiView("abc"), uppercase));
    CHECK(ContentHash::Parse(StringAnsiView("za7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), uppercase));

    ArtifactKey key;
    REQUIRE_FALSE(ArtifactKey::Parse(canonical, key));
    Dictionary<ArtifactKey, int32> lookup;
    lookup.Add(key, 7);
    CHECK(lookup[key] == 7);
}

TEST_CASE("Artifact keys use typed length-prefixed fields")
{
    ArtifactKeyBuilder first;
    first.AddString(StringAnsiView("left"), StringAnsiView("ab"));
    first.AddString(StringAnsiView("right"), StringAnsiView("c"));
    ArtifactKeyBuilder second;
    second.AddString(StringAnsiView("left"), StringAnsiView("a"));
    second.AddString(StringAnsiView("right"), StringAnsiView("bc"));
    CHECK(first.Finalize() != second.Finalize());

    ArtifactKeyBuilder typedNumber;
    typedNumber.AddUInt32(StringAnsiView("value"), 7);
    ArtifactKeyBuilder typedString;
    typedString.AddString(StringAnsiView("value"), StringAnsiView("7"));
    CHECK(typedNumber.Finalize() != typedString.Finalize());

    ArtifactKeyBuilder domainA(StringAnsiView("domain-a"));
    ArtifactKeyBuilder domainB(StringAnsiView("domain-b"));
    CHECK(domainA.Finalize() != domainB.Finalize());
}

TEST_CASE("Artifact target keys include only declared dimensions")
{
    ArtifactTarget target;
    target.Platform = "Windows";
    target.Architecture = "x64";
    target.Graphics = "DirectX12-SM6";
    target.TextureCompression = "BC7";
    target.FeatureFlags.Add("raytracing");
    target.FeatureFlags.Add("virtual-textures");

    const ArtifactTargetDimension textureMask = ArtifactTargetDimension::Platform | ArtifactTargetDimension::TextureCompression;
    const ArtifactKey textureKey = target.BuildKey(textureMask);
    target.Graphics = "Vulkan-SM6";
    CHECK(target.BuildKey(textureMask) == textureKey);
    target.TextureCompression = "BC3";
    CHECK(target.BuildKey(textureMask) != textureKey);

    ArtifactTarget reordered = target;
    reordered.FeatureFlags.Clear();
    reordered.FeatureFlags.Add("virtual-textures");
    reordered.FeatureFlags.Add("raytracing");
    const ArtifactTargetDimension featureMask = ArtifactTargetDimension::Platform | ArtifactTargetDimension::FeatureFlags;
    CHECK(reordered.BuildKey(featureMask) == target.BuildKey(featureMask));
}

TEST_CASE("SourceHashCache uses content bytes as key truth")
{
    const String root = Globals::TemporaryFolder / (TEXT("SourceHashCache-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String path = root / TEXT("source.bin");
    const char firstBytes[] = "first";
    const char changedBytes[] = "other";
    REQUIRE_FALSE(File::WriteAllBytes(path, firstBytes, 5));

    SourceHashCache cache;
    ContentHash first;
    SourceHashFileState firstState;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(cache.HashFile(path, first, firstState, diagnostic));
    ContentHash cached;
    SourceHashFileState cachedState;
    REQUIRE_FALSE(cache.HashFile(path, cached, cachedState, diagnostic));
    CHECK(cached == first);
    CHECK(cache.GetMetrics().CacheHits == 1);

    SourceHashFileState corrupt = firstState;
    corrupt.CachedContentHash.Bytes[0] ^= 0xff;
    Array<SourceHashFileState> corruptStates;
    corruptStates.Add(corrupt);
    cache.Seed(corruptStates);
    ContentHash recovered;
    SourceHashFileState recoveredState;
    REQUIRE_FALSE(cache.HashFile(path, recovered, recoveredState, diagnostic));
    CHECK(recovered == first);

#if PLATFORM_WINDOWS
    HANDLE handle = CreateFileW(*path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    FILETIME originalWrite;
    REQUIRE(GetFileTime(handle, nullptr, nullptr, &originalWrite) != 0);
    CloseHandle(handle);
    REQUIRE_FALSE(File::WriteAllBytes(path, changedBytes, 5));
    handle = CreateFileW(*path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    REQUIRE(SetFileTime(handle, nullptr, nullptr, &originalWrite) != 0);
    CloseHandle(handle);
    ContentHash changed;
    SourceHashFileState changedState;
    REQUIRE_FALSE(cache.HashFile(path, changed, changedState, diagnostic));
    CHECK(changed != first);
#endif

    REQUIRE_FALSE(File::WriteAllBytes(path, firstBytes, 5));
    ContentHash sameBytes;
    SourceHashFileState sameState;
    REQUIRE_FALSE(cache.HashFile(path, sameBytes, sameState, diagnostic));
    CHECK(sameBytes == first);
}
