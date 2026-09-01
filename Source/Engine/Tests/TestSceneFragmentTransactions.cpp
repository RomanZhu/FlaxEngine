// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/SceneFragments/SceneFragmentReconciler.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Level/ScenePartitionDocument.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Serialization/JsonWriters.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    SceneFragmentWrite MakeFragmentWrite(const char* name)
    {
        const StringAnsi payload = StringAnsi::Format("[{{\"fileId\":2,\"name\":\"{0}\"}}]", name);
        SceneFragmentWrite write;
        write.RootActorLocalId = 2;
        write.ContainedLocalIds.Add(2);
        write.Payload.Set(reinterpret_cast<const byte*>(payload.Get()), payload.Length());
        return write;
    }

    void ReadBytes(const StringView& path, BytesContainer& bytes)
    {
        bytes.Release();
        REQUIRE(!File::ReadAllBytes(path, bytes));
    }

    bool SameBytes(const BytesContainer& a, const BytesContainer& b)
    {
        return a.Length() == b.Length() &&
               (a.Length() == 0 || Platform::MemoryCompare(a.Get(), b.Get(), a.Length()) == 0);
    }

    void WriteDocument(const StringView& path, const rapidjson_flax::Document& document)
    {
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        document.Accept(writer.GetWriter());
        REQUIRE_FALSE(File::WriteAllBytes(path, buffer.GetString(), static_cast<int32>(buffer.GetSize())));
    }

    bool HasDiagnostic(const Array<SceneFragmentDiagnostic>& diagnostics, SceneFragmentDiagnosticCode code)
    {
        for (const SceneFragmentDiagnostic& diagnostic : diagnostics)
        {
            if (diagnostic.Code == code && diagnostic.Message.HasChars() && diagnostic.Path.HasChars())
                return true;
        }
        return false;
    }

    void PrepareSceneSave(const Guid& sceneGuid, const StringView& scenePath, const char* fragmentName,
        const char* sceneData, PreparedSceneSave& save)
    {
        String error;
        save = PreparedSceneSave();
        save.SourcePath = scenePath;
        REQUIRE(!SceneFragmentStore::CaptureSourceRevision(scenePath, save.ExpectedSource, error));
        Array<SceneFragmentWrite> writes;
        if (fragmentName)
            writes.Add(MakeFragmentWrite(fragmentName));
        REQUIRE(!SceneFragmentStore::PrepareSave(sceneGuid, writes, save.FragmentPlan, error));
        save.SourceData.Set(reinterpret_cast<const byte*>(sceneData), StringAnsiView(sceneData).Length());
    }
}

TEST_CASE("ExternalActors integrity diagnostics classify invalid storage")
{
    const Guid sceneGuid = Guid::New();
    const String directory = SceneFragmentStore::GetScenePath(sceneGuid);
    const String indexPath = SceneFragmentStore::GetIndexPath(sceneGuid);
    const String fragmentPath = directory / SceneFragmentStore::GetRelativeFragmentPath(2);
    FileSystem::DeleteDirectory(directory, true);
    SCOPE_EXIT { FileSystem::DeleteDirectory(directory, true); };

    String error;
    Array<SceneFragmentWrite> writes;
    writes.Add(MakeFragmentWrite("actor"));
    REQUIRE_FALSE(SceneFragmentStore::Save(sceneGuid, writes, error));

    Array<SceneFragmentDiagnostic> diagnostics;
    SECTION("Missing indexed fragment")
    {
        REQUIRE_FALSE(FileSystem::DeleteFile(fragmentPath));
        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        CHECK(HasDiagnostic(diagnostics, SceneFragmentDiagnosticCode::MissingFragment));
    }

    SECTION("Orphaned physical fragment")
    {
        const String orphanPath = directory / TEXT("orphan.sceneactor");
        const char orphan[] = "{}";
        REQUIRE_FALSE(File::WriteAllBytes(orphanPath, orphan, ARRAY_COUNT(orphan) - 1));
        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        CHECK(HasDiagnostic(diagnostics, SceneFragmentDiagnosticCode::OrphanFragment));
    }

    SECTION("Malformed index")
    {
        const char malformed[] = "{";
        REQUIRE_FALSE(File::WriteAllBytes(indexPath, malformed, ARRAY_COUNT(malformed) - 1));
        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        CHECK(HasDiagnostic(diagnostics, SceneFragmentDiagnosticCode::Malformed));
    }

    SECTION("Malformed fragment")
    {
        BytesContainer fragmentBytes;
        ReadBytes(fragmentPath, fragmentBytes);
        rapidjson_flax::Document fragment;
        fragment.Parse(fragmentBytes.Get<char>(), fragmentBytes.Length());
        REQUIRE_FALSE(fragment.HasParseError());
        REQUIRE(fragment.RemoveMember("payload"));
        WriteDocument(fragmentPath, fragment);

        ReadBytes(fragmentPath, fragmentBytes);
        BytesContainer indexBytes;
        ReadBytes(indexPath, indexBytes);
        rapidjson_flax::Document index;
        index.Parse(indexBytes.Get<char>(), indexBytes.Length());
        REQUIRE_FALSE(index.HasParseError());
        auto fragments = index.FindMember("fragments");
        REQUIRE(fragments != index.MemberEnd());
        REQUIRE(fragments->value.Size() == 1);
        auto& entry = fragments->value[0];
        const StringAnsi contentHash = ContentHash::Compute(fragmentBytes.Get(), fragmentBytes.Length()).ToString();
        entry["contentHash"].SetString(contentHash.Get(), contentHash.Length(), index.GetAllocator());
        entry["size"].SetUint64(fragmentBytes.Length());
        WriteDocument(indexPath, index);

        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        CHECK(HasDiagnostic(diagnostics, SceneFragmentDiagnosticCode::Malformed));
    }

    SECTION("Duplicate index identity")
    {
        BytesContainer bytes;
        ReadBytes(indexPath, bytes);
        rapidjson_flax::Document index;
        index.Parse(bytes.Get<char>(), bytes.Length());
        REQUIRE_FALSE(index.HasParseError());
        auto fragments = index.FindMember("fragments");
        REQUIRE(fragments != index.MemberEnd());
        REQUIRE(fragments->value.Size() == 1);
        rapidjson_flax::Value duplicate;
        duplicate.CopyFrom(fragments->value[0], index.GetAllocator());
        fragments->value.PushBack(duplicate, index.GetAllocator());
        WriteDocument(indexPath, index);

        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        CHECK(HasDiagnostic(diagnostics, SceneFragmentDiagnosticCode::DuplicateLocalId));
    }

    SECTION("Wrong scene owner")
    {
        BytesContainer bytes;
        ReadBytes(indexPath, bytes);
        rapidjson_flax::Document index;
        index.Parse(bytes.Get<char>(), bytes.Length());
        REQUIRE_FALSE(index.HasParseError());
        auto owner = index.FindMember("ownerSceneGuid");
        REQUIRE(owner != index.MemberEnd());
        const StringAnsi wrongOwner(Guid::New().ToString(Guid::FormatType::N).ToLower());
        owner->value.SetString(wrongOwner.Get(), wrongOwner.Length(), index.GetAllocator());
        WriteDocument(indexPath, index);

        SceneFragmentReconciler::Reconcile(sceneGuid, diagnostics);
        CHECK(HasDiagnostic(diagnostics, SceneFragmentDiagnosticCode::OwnerMismatch));
    }
}

TEST_CASE("ExternalActors formats reject unsupported versions without mutation")
{
    const Guid sceneGuid = Guid::New();
    const String directory = SceneFragmentStore::GetScenePath(sceneGuid);
    const String indexPath = SceneFragmentStore::GetIndexPath(sceneGuid);
    const String fragmentPath = directory / TEXT("candidate.sceneactor");
    REQUIRE_FALSE(FileSystem::CreateDirectory(directory));
    SCOPE_EXIT { FileSystem::DeleteDirectory(directory, true); };
    const StringAnsi owner(sceneGuid.ToString(Guid::FormatType::N).ToLower());

    const StringAnsi indexes[] =
    {
        StringAnsi::Format("{{\"ownerSceneGuid\":\"{0}\",\"indexRevision\":1,\"fragments\":[]}}", owner),
        StringAnsi::Format("{{\"formatVersion\":0,\"ownerSceneGuid\":\"{0}\",\"indexRevision\":1,\"fragments\":[]}}", owner),
        StringAnsi::Format("{{\"formatVersion\":2,\"ownerSceneGuid\":\"{0}\",\"indexRevision\":1,\"fragments\":[]}}", owner),
        StringAnsi::Format("{{\"formatVersion\":1,\"sceneVersion\":4,\"ownerSceneGuid\":\"{0}\",\"indexRevision\":1,\"fragments\":[]}}", owner),
    };
    for (const StringAnsi& source : indexes)
    {
        REQUIRE_FALSE(File::WriteAllBytes(indexPath, source.Get(), source.Length()));
        SceneFragmentIndex index;
        String error;
        CHECK(SceneFragmentStore::ReadIndex(sceneGuid, index, error));
        BytesContainer bytes;
        ReadBytes(indexPath, bytes);
        REQUIRE(bytes.Length() == source.Length());
        CHECK(Platform::MemoryCompare(bytes.Get(), source.Get(), source.Length()) == 0);
    }

    const char* fragments[] =
    {
        R"({"rootActorLocalId":2,"payload":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})",
        R"({"formatVersion":0,"rootActorLocalId":2,"payload":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})",
        R"({"formatVersion":2,"rootActorLocalId":2,"payload":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})",
        R"({"formatVersion":1,"prefabVersion":4,"rootActorLocalId":2,"payload":[{"fileId":2,"type":"FlaxEngine.EmptyActor"}]})",
    };
    for (const char* source : fragments)
    {
        const int32 length = StringAnsiView(source).Length();
        REQUIRE_FALSE(File::WriteAllBytes(fragmentPath, source, length));
        BytesContainer bytes;
        ReadBytes(fragmentPath, bytes);
        rapidjson_flax::Document document;
        document.Parse(bytes.Get<char>(), bytes.Length());
        REQUIRE_FALSE(document.HasParseError());
        int64 rootActorLocalId;
        const rapidjson_flax::Value* objects;
        String error;
        CHECK(ScenePartitionDocument::ReadFragment(document, rootActorLocalId, objects, error));
        BytesContainer after;
        ReadBytes(fragmentPath, after);
        CHECK(SameBytes(bytes, after));
    }
}

TEST_CASE("Scene fragment batch rejects stale source before publication")
{
    const Guid sceneGuids[] = { Guid::New(), Guid::New() };
    const String scenePaths[] = {
        Globals::ProjectContentFolder / TEXT("__SceneFragmentBatchA.scene"),
        Globals::ProjectContentFolder / TEXT("__SceneFragmentBatchB.scene"),
    };
    for (int32 i = 0; i < 2; i++)
    {
        FileSystem::DeleteFile(scenePaths[i]);
        FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuids[i]), true);
    }
    SCOPE_EXIT
    {
        for (int32 i = 0; i < 2; i++)
        {
            FileSystem::DeleteFile(scenePaths[i]);
            FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuids[i]), true);
        }
    };

    String error;
    Array<PreparedSceneSave> saves;
    saves.Resize(2);
    PrepareSceneSave(sceneGuids[0], scenePaths[0], "old-a", "old-scene-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], "old-b", "old-scene-b", saves[1]);
    REQUIRE(!SceneFragmentStore::CommitSceneSaves(saves, error));

    BytesContainer oldSource;
    BytesContainer oldIndexes[2];
    BytesContainer oldFragments[2];
    ReadBytes(scenePaths[0], oldSource);
    for (int32 i = 0; i < 2; i++)
    {
        ReadBytes(SceneFragmentStore::GetIndexPath(sceneGuids[i]), oldIndexes[i]);
        ReadBytes(SceneFragmentStore::GetScenePath(sceneGuids[i]) /
            SceneFragmentStore::GetRelativeFragmentPath(2), oldFragments[i]);
    }

    PrepareSceneSave(sceneGuids[0], scenePaths[0], "new-a", "new-scene-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], "new-b", "new-scene-b", saves[1]);
    const char externalSource[] = "external-scene-b";
    REQUIRE(!File::WriteAllBytes(scenePaths[1], externalSource, ARRAY_COUNT(externalSource) - 1));
    REQUIRE(SceneFragmentStore::CommitSceneSaves(saves, error));
    CHECK(error.HasChars());

    BytesContainer current;
    ReadBytes(scenePaths[0], current);
    CHECK(SameBytes(oldSource, current));
    for (int32 i = 0; i < 2; i++)
    {
        ReadBytes(SceneFragmentStore::GetIndexPath(sceneGuids[i]), current);
        CHECK(SameBytes(oldIndexes[i], current));
        ReadBytes(SceneFragmentStore::GetScenePath(sceneGuids[i]) /
            SceneFragmentStore::GetRelativeFragmentPath(2), current);
        CHECK(SameBytes(oldFragments[i], current));
    }
}

TEST_CASE("Scene fragment cross-scene transfer publishes complete owner states")
{
    const Guid sceneGuids[] = { Guid::New(), Guid::New() };
    const String scenePaths[] = {
        Globals::ProjectContentFolder / TEXT("__SceneFragmentTransferA.scene"),
        Globals::ProjectContentFolder / TEXT("__SceneFragmentTransferB.scene"),
    };
    const String sourceFragment = SceneFragmentStore::GetScenePath(sceneGuids[0]) /
                                  SceneFragmentStore::GetRelativeFragmentPath(2);
    const String destinationFragment = SceneFragmentStore::GetScenePath(sceneGuids[1]) /
                                       SceneFragmentStore::GetRelativeFragmentPath(2);
    for (int32 i = 0; i < 2; i++)
    {
        FileSystem::DeleteFile(scenePaths[i]);
        FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuids[i]), true);
    }
    SCOPE_EXIT
    {
        for (int32 i = 0; i < 2; i++)
        {
            FileSystem::DeleteFile(scenePaths[i]);
            FileSystem::DeleteDirectory(SceneFragmentStore::GetScenePath(sceneGuids[i]), true);
        }
    };

    String error;
    Array<PreparedSceneSave> saves;
    saves.Resize(2);
    PrepareSceneSave(sceneGuids[0], scenePaths[0], "actor", "old-source-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], nullptr, "old-source-b", saves[1]);
    REQUIRE(!SceneFragmentStore::CommitSceneSaves(saves, error));
    REQUIRE(FileSystem::FileExists(sourceFragment));
    REQUIRE_FALSE(FileSystem::FileExists(destinationFragment));

    PrepareSceneSave(sceneGuids[0], scenePaths[0], nullptr, "moved-source-a", saves[0]);
    PrepareSceneSave(sceneGuids[1], scenePaths[1], "actor", "moved-source-b", saves[1]);
    REQUIRE(!SceneFragmentStore::CommitSceneSaves(saves, error));

    SceneFragmentIndex sourceIndex;
    SceneFragmentIndex destinationIndex;
    Array<Array<byte>> sourceFragments;
    Array<Array<byte>> destinationFragments;
    REQUIRE(!SceneFragmentStore::Load(sceneGuids[0], sourceIndex, sourceFragments, error));
    REQUIRE(!SceneFragmentStore::Load(sceneGuids[1], destinationIndex, destinationFragments, error));
    CHECK(sourceIndex.Fragments.IsEmpty());
    CHECK(sourceFragments.IsEmpty());
    REQUIRE(destinationIndex.Fragments.Count() == 1);
    REQUIRE(destinationFragments.Count() == 1);
    CHECK(destinationIndex.Fragments[0].RootActorLocalId == 2);
    CHECK_FALSE(FileSystem::FileExists(sourceFragment));
    CHECK(FileSystem::FileExists(destinationFragment));
}

#endif
