// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneFragmentStore.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"

namespace
{
    constexpr const Char* IndexFileName = TEXT("scene-fragments.index");
    constexpr const Char* FragmentExtension = TEXT(".sceneactor");

    bool Fail(String& error, const StringView& message)
    {
        error = message;
        return true;
    }

    bool CompareEntries(const SceneFragmentIndexEntry& a, const SceneFragmentIndexEntry& b)
    {
        return a.RootActorLocalId < b.RootActorLocalId;
    }

    bool SameEntries(const Array<SceneFragmentIndexEntry>& a, const Array<SceneFragmentIndexEntry>& b)
    {
        if (a.Count() != b.Count())
            return false;
        for (int32 i = 0; i < a.Count(); i++)
        {
            if (a[i].RootActorLocalId != b[i].RootActorLocalId ||
                a[i].RelativePhysicalPath != b[i].RelativePhysicalPath ||
                a[i].Content != b[i].Content ||
                a[i].Size != b[i].Size ||
                a[i].SerializerVersion != b[i].SerializerVersion)
            {
                return false;
            }
        }
        return true;
    }

    bool WriteAtomicIfChanged(const StringView& path, const void* data, int32 length)
    {
        BytesContainer existing;
        if (!File::ReadAllBytes(path, existing) && existing.Length() == length &&
            Platform::MemoryCompare(existing.Get(), data, length) == 0)
        {
            return false;
        }
        const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        if (File::WriteAllBytes(staging, data, length) || FileSystem::MoveFile(path, staging, true))
        {
            FileSystem::DeleteFile(staging);
            return true;
        }
        return false;
    }

    bool ParseFragment(const Guid& expectedOwner, const SceneFragmentIndexEntry& entry, const Array<byte>& bytes, String& error)
    {
        rapidjson_flax::Document document;
        document.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
        if (document.HasParseError() || !document.IsObject())
            return Fail(error, TEXT("Scene fragment is not valid JSON."));
        const auto version = document.FindMember("formatVersion");
        const auto owner = document.FindMember("ownerSceneGuid");
        const auto root = document.FindMember("rootActorLocalId");
        const auto contained = document.FindMember("containedLocalIds");
        const auto serializer = document.FindMember("serializerVersion");
        const auto payload = document.FindMember("payload");
        Guid ownerGuid;
        if (version == document.MemberEnd() || !version->value.IsUint() ||
            owner == document.MemberEnd() || !owner->value.IsString() ||
            Guid::Parse(StringAnsiView(owner->value.GetString(), owner->value.GetStringLength()), ownerGuid) ||
            root == document.MemberEnd() || !root->value.IsInt64() ||
            contained == document.MemberEnd() || !contained->value.IsArray() ||
            serializer == document.MemberEnd() || !serializer->value.IsUint() ||
            payload == document.MemberEnd() || !payload->value.IsArray())
        {
            return Fail(error, TEXT("Scene fragment header is malformed."));
        }
        if (version->value.GetUint() > SceneFragmentStore::FragmentFormatVersion)
            return Fail(error, TEXT("Scene fragment uses a future format version."));
        if (version->value.GetUint() != SceneFragmentStore::FragmentFormatVersion || ownerGuid != expectedOwner ||
            root->value.GetInt64() != entry.RootActorLocalId || serializer->value.GetUint() != entry.SerializerVersion)
        {
            return Fail(error, TEXT("Scene fragment header does not match its owner index."));
        }
        HashSet<int64> localIds;
        for (const rapidjson_flax::Value& value : contained->value.GetArray())
        {
            if (!value.IsInt64() || value.GetInt64() <= 1 || !localIds.Add(value.GetInt64()))
                return Fail(error, TEXT("Scene fragment contains invalid or duplicate local IDs."));
        }
        if (!localIds.Contains(entry.RootActorLocalId))
            return Fail(error, TEXT("Scene fragment does not contain its root actor local ID."));
        return false;
    }

    void WriteIndex(const SceneFragmentIndex& index, rapidjson_flax::StringBuffer& buffer)
    {
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("formatVersion");
        writer.Uint(index.FormatVersion);
        writer.JKEY("ownerSceneGuid");
        const String owner = index.OwnerSceneGuid.ToString(Guid::FormatType::N).ToLower();
        const StringAnsi ownerAnsi(owner);
        writer.String(ownerAnsi.Get(), ownerAnsi.Length());
        writer.JKEY("indexRevision");
        writer.Uint64(index.IndexRevision);
        writer.JKEY("fragments");
        writer.StartArray();
        for (const SceneFragmentIndexEntry& entry : index.Fragments)
        {
            writer.StartObject();
            writer.JKEY("rootActorLocalId");
            writer.Int64(entry.RootActorLocalId);
            writer.JKEY("relativePhysicalPath");
            const StringAnsi relativePath(entry.RelativePhysicalPath);
            writer.String(relativePath.Get(), relativePath.Length());
            writer.JKEY("contentHash");
            const StringAnsi hash = entry.Content.ToString();
            writer.String(hash.Get(), hash.Length());
            writer.JKEY("size");
            writer.Uint64(entry.Size);
            writer.JKEY("serializerVersion");
            writer.Uint(entry.SerializerVersion);
            writer.EndObject();
        }
        writer.EndArray(index.Fragments.Count());
        writer.EndObject();
    }
}

String SceneFragmentStore::GetRootPath()
{
    return Globals::ProjectFolder / TEXT("ExternalActors");
}

String SceneFragmentStore::GetScenePath(const Guid& sceneGuid)
{
    return GetRootPath() / sceneGuid.ToString(Guid::FormatType::N).ToLower();
}

String SceneFragmentStore::GetIndexPath(const Guid& sceneGuid)
{
    return GetScenePath(sceneGuid) / IndexFileName;
}

String SceneFragmentStore::GetRelativeFragmentPath(int64 rootActorLocalId)
{
    const String localId = String::Format(TEXT("{0}"), rootActorLocalId);
    const String shard = localId.Length() >= 2 ? localId.Substring(0, 2) : TEXT("00");
    return shard / localId + FragmentExtension;
}

bool SceneFragmentStore::ReadIndex(const Guid& sceneGuid, SceneFragmentIndex& index, String& error)
{
    index = SceneFragmentIndex();
    error.Clear();
    if (!sceneGuid.IsValid())
        return Fail(error, TEXT("Scene fragment owner GUID is invalid."));
    BytesContainer bytes;
    const String path = GetIndexPath(sceneGuid);
    if (File::ReadAllBytes(path, bytes))
        return Fail(error, TEXT("Scene fragment index is missing or unreadable."));
    rapidjson_flax::Document document;
    document.Parse(bytes.Get<char>(), bytes.Length());
    if (document.HasParseError() || !document.IsObject())
        return Fail(error, TEXT("Scene fragment index is malformed."));
    const auto version = document.FindMember("formatVersion");
    const auto owner = document.FindMember("ownerSceneGuid");
    const auto revision = document.FindMember("indexRevision");
    const auto fragments = document.FindMember("fragments");
    Guid ownerGuid;
    if (version == document.MemberEnd() || !version->value.IsUint() ||
        owner == document.MemberEnd() || !owner->value.IsString() ||
        Guid::Parse(StringAnsiView(owner->value.GetString(), owner->value.GetStringLength()), ownerGuid) ||
        revision == document.MemberEnd() || !revision->value.IsUint64() || revision->value.GetUint64() == 0 ||
        fragments == document.MemberEnd() || !fragments->value.IsArray())
    {
        return Fail(error, TEXT("Scene fragment index header is malformed."));
    }
    if (version->value.GetUint() > SceneFragmentIndex::CurrentFormatVersion)
        return Fail(error, TEXT("Scene fragment index uses a future format version."));
    if (version->value.GetUint() != SceneFragmentIndex::CurrentFormatVersion || ownerGuid != sceneGuid)
        return Fail(error, TEXT("Scene fragment index ownership is invalid."));

    index.OwnerSceneGuid = ownerGuid;
    index.FormatVersion = version->value.GetUint();
    index.IndexRevision = revision->value.GetUint64();
    HashSet<int64> roots;
    HashSet<String> paths;
    for (const rapidjson_flax::Value& value : fragments->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(error, TEXT("Scene fragment index entry is malformed."));
        const auto root = value.FindMember("rootActorLocalId");
        const auto relativePath = value.FindMember("relativePhysicalPath");
        const auto hash = value.FindMember("contentHash");
        const auto size = value.FindMember("size");
        const auto serializer = value.FindMember("serializerVersion");
        SceneFragmentIndexEntry entry;
        if (root == value.MemberEnd() || !root->value.IsInt64() || root->value.GetInt64() <= 1 ||
            relativePath == value.MemberEnd() || !relativePath->value.IsString() ||
            hash == value.MemberEnd() || !hash->value.IsString() ||
            size == value.MemberEnd() || !size->value.IsUint64() || size->value.GetUint64() == 0 ||
            serializer == value.MemberEnd() || !serializer->value.IsUint() || serializer->value.GetUint() == 0)
        {
            return Fail(error, TEXT("Scene fragment index entry fields are malformed."));
        }
        entry.RootActorLocalId = root->value.GetInt64();
        entry.RelativePhysicalPath = String(relativePath->value.GetString(), relativePath->value.GetStringLength());
        entry.Size = size->value.GetUint64();
        entry.SerializerVersion = serializer->value.GetUint();
        if (ContentHash::Parse(StringAnsiView(hash->value.GetString(), hash->value.GetStringLength()), entry.Content) ||
            entry.Content.IsZero() || entry.RelativePhysicalPath != GetRelativeFragmentPath(entry.RootActorLocalId) ||
            !roots.Add(entry.RootActorLocalId) || !paths.Add(entry.RelativePhysicalPath))
        {
            return Fail(error, TEXT("Scene fragment index contains invalid, duplicate, or misplaced entries."));
        }
        index.Fragments.Add(MoveTemp(entry));
    }
    Sorting::QuickSort(index.Fragments.Get(), index.Fragments.Count(), CompareEntries);
    return false;
}

bool SceneFragmentStore::Load(const Guid& sceneGuid, SceneFragmentIndex& index, Array<Array<byte>>& fragments, String& error)
{
    fragments.Clear();
    if (ReadIndex(sceneGuid, index, error))
        return true;
    const String scenePath = GetScenePath(sceneGuid);
    for (const SceneFragmentIndexEntry& entry : index.Fragments)
    {
        Array<byte> bytes;
        const String path = scenePath / entry.RelativePhysicalPath;
        if (File::ReadAllBytes(path, bytes))
            return Fail(error, TEXT("A scene fragment referenced by the index is missing or unreadable."));
        if (static_cast<uint64>(bytes.Count()) != entry.Size || ContentHash::Compute(bytes.Get(), bytes.Count()) != entry.Content)
            return Fail(error, TEXT("A scene fragment does not match its indexed content hash or size."));
        if (ParseFragment(sceneGuid, entry, bytes, error))
            return true;
        fragments.Add(MoveTemp(bytes));
    }
    return false;
}

bool SceneFragmentStore::Save(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments, String& error)
{
    error.Clear();
    if (!sceneGuid.IsValid())
        return Fail(error, TEXT("Scene fragment owner GUID is invalid."));
    const String scenePath = GetScenePath(sceneGuid);
    if (!FileSystem::DirectoryExists(scenePath) && FileSystem::CreateDirectory(scenePath))
        return Fail(error, TEXT("Cannot create the private scene fragment directory."));

    SceneFragmentIndex previous;
    String previousError;
    const bool hasPrevious = !ReadIndex(sceneGuid, previous, previousError);
    SceneFragmentIndex next;
    next.OwnerSceneGuid = sceneGuid;
    HashSet<int64> roots;
    for (const SceneFragmentWrite& fragment : fragments)
    {
        if (fragment.RootActorLocalId <= 1 || fragment.SerializerVersion == 0 || fragment.Payload.IsEmpty() ||
            !roots.Add(fragment.RootActorLocalId))
        {
            return Fail(error, TEXT("Scene fragment write contains an invalid or duplicate root actor local ID."));
        }
        HashSet<int64> contained;
        for (int64 localId : fragment.ContainedLocalIds)
        {
            if (localId <= 1 || !contained.Add(localId))
                return Fail(error, TEXT("Scene fragment write contains invalid or duplicate local IDs."));
        }
        if (!contained.Contains(fragment.RootActorLocalId))
            return Fail(error, TEXT("Scene fragment write does not contain its root actor local ID."));

        rapidjson_flax::Document payload;
        payload.Parse(reinterpret_cast<const char*>(fragment.Payload.Get()), fragment.Payload.Count());
        if (payload.HasParseError() || !payload.IsArray())
            return Fail(error, TEXT("Scene fragment payload must be a valid authored object array."));
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("formatVersion");
        writer.Uint(FragmentFormatVersion);
        writer.JKEY("ownerSceneGuid");
        const String owner = sceneGuid.ToString(Guid::FormatType::N).ToLower();
        const StringAnsi ownerAnsi(owner);
        writer.String(ownerAnsi.Get(), ownerAnsi.Length());
        writer.JKEY("rootActorLocalId");
        writer.Int64(fragment.RootActorLocalId);
        writer.JKEY("containedLocalIds");
        writer.StartArray();
        for (int64 localId : fragment.ContainedLocalIds)
            writer.Int64(localId);
        writer.EndArray(fragment.ContainedLocalIds.Count());
        writer.JKEY("serializerVersion");
        writer.Uint(fragment.SerializerVersion);
        writer.JKEY("payload");
        payload.Accept(writer.GetWriter());
        writer.EndObject();

        SceneFragmentIndexEntry entry;
        entry.RootActorLocalId = fragment.RootActorLocalId;
        entry.RelativePhysicalPath = GetRelativeFragmentPath(fragment.RootActorLocalId);
        entry.Content = ContentHash::Compute(buffer.GetString(), buffer.GetSize());
        entry.Size = buffer.GetSize();
        entry.SerializerVersion = fragment.SerializerVersion;
        const String path = scenePath / entry.RelativePhysicalPath;
        const String directory = StringUtils::GetDirectoryName(path);
        if (!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory))
            return Fail(error, TEXT("Cannot create a scene fragment shard directory."));
        if (WriteAtomicIfChanged(path, buffer.GetString(), static_cast<int32>(buffer.GetSize())))
            return Fail(error, TEXT("Cannot publish a scene fragment."));
        next.Fragments.Add(MoveTemp(entry));
    }
    Sorting::QuickSort(next.Fragments.Get(), next.Fragments.Count(), CompareEntries);
    next.IndexRevision = hasPrevious && SameEntries(previous.Fragments, next.Fragments)
                             ? previous.IndexRevision
                             : hasPrevious ? previous.IndexRevision + 1 : 1;
    rapidjson_flax::StringBuffer indexBytes;
    WriteIndex(next, indexBytes);
    if (WriteAtomicIfChanged(GetIndexPath(sceneGuid), indexBytes.GetString(), static_cast<int32>(indexBytes.GetSize())))
        return Fail(error, TEXT("Cannot publish the scene fragment index."));

    if (hasPrevious)
    {
        HashSet<String> retained;
        for (const SceneFragmentIndexEntry& entry : next.Fragments)
            retained.Add(entry.RelativePhysicalPath);
        for (const SceneFragmentIndexEntry& entry : previous.Fragments)
        {
            if (!retained.Contains(entry.RelativePhysicalPath))
                FileSystem::DeleteFile(scenePath / entry.RelativePhysicalPath);
        }
    }
    return false;
}

bool SceneFragmentStore::Delete(const Guid& sceneGuid, String& error)
{
    error.Clear();
    if (!sceneGuid.IsValid())
        return Fail(error, TEXT("Scene fragment owner GUID is invalid."));
    const String path = GetScenePath(sceneGuid);
    if (FileSystem::DirectoryExists(path) && FileSystem::DeleteDirectory(path))
        return Fail(error, TEXT("Cannot delete the private scene fragment directory."));
    return false;
}
