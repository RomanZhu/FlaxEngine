// Copyright (c) Wojciech Figat. All rights reserved.

#include "RuntimeAssetIndex.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
        diagnostic.Message = message;
        return true;
    }

    StringAnsi GuidKey(const Guid& id)
    {
        const String wide = id.ToString(Guid::FormatType::N);
        StringAnsi result;
        result.Resize(wide.Length());
        for (int32 i = 0; i < wide.Length(); i++)
        {
            const Char c = wide[i];
            result[i] = (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        }
        return result;
    }

    StringAnsi ObjectKey(const AssetObjectId& id)
    {
        return StringAnsi::Format("{0}:{1}", GuidKey(id.Guid), id.LocalId);
    }

    void AddString(JsonValue& object, const char* name, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi text(value);
        object.AddMember(JsonValue(name, allocator), JsonValue(text.Get(), text.Length(), allocator), allocator);
    }

    void AddAnsi(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    bool ValidateEntry(const RuntimeAssetIndexEntry& entry, AssetPipelineDiagnostic& diagnostic)
    {
        if (!entry.ID.IsValid() || !entry.BackingAssetID.IsValid() || !entry.PackageID.IsValid() || entry.TypeName.IsEmpty() || entry.PackagedPath.IsEmpty() || entry.Size == 0)
            return Fail(diagnostic, TEXT("Runtime asset index entries require object identity, backing GUID, type, package identity, and a non-empty package range."));
        if (RuntimeAssetIndex::ContainsLibraryPath(entry.PackagedPath) || RuntimeAssetIndex::ContainsLibraryPath(entry.CanonicalPath))
            return Fail(diagnostic, TEXT("Runtime asset index must not refer to project Library storage."));
        if (EnumHasAnyFlags(entry.Flags, RuntimeAssetIndexFlags::ExactArtifact) && entry.ExactArtifact.IsZero())
            return Fail(diagnostic, TEXT("Runtime asset index exact-artifact entries require the immutable artifact key."));
        return false;
    }

    bool BuildAssetsObject(const Array<RuntimeAssetIndexEntry>& sorted, JsonValue& assets, JsonAlloc& allocator, AssetPipelineDiagnostic& diagnostic)
    {
        assets.SetObject();
        AssetObjectId previous;
        for (const RuntimeAssetIndexEntry& entry : sorted)
        {
            if (ValidateEntry(entry, diagnostic))
                return true;
            if (previous.IsValid() && previous == entry.ID)
                return Fail(diagnostic, TEXT("Runtime asset index contains a duplicate file GUID/local file ID."));
            previous = entry.ID;

            JsonValue item(rapidjson::kObjectType);
            AddAnsi(item, "fileGuid", GuidKey(entry.ID.Guid), allocator);
            item.AddMember("localId", entry.ID.LocalId, allocator);
            AddAnsi(item, "backingGuid", GuidKey(entry.BackingAssetID), allocator);
            AddString(item, "type", entry.TypeName, allocator);
            AddString(item, "canonicalPath", entry.CanonicalPath, allocator);
            AddString(item, "packagePath", entry.PackagedPath, allocator);
            AddAnsi(item, "packageId", GuidKey(entry.PackageID), allocator);
            item.AddMember("chunkId", entry.ChunkID, allocator);
            item.AddMember("offset", entry.Offset, allocator);
            item.AddMember("size", entry.Size, allocator);
            item.AddMember("assetFormatVersion", entry.AssetFormatVersion, allocator);
            item.AddMember("flags", static_cast<uint32>(entry.Flags), allocator);
            if (entry.ExactArtifact.IsZero())
                item.AddMember("exactArtifact", JsonValue(rapidjson::kNullType), allocator);
            else
                AddAnsi(item, "exactArtifact", entry.ExactArtifact.ToString(), allocator);
            const StringAnsi key = ObjectKey(entry.ID);
            assets.AddMember(JsonValue(key.Get(), key.Length(), allocator), item, allocator);
        }
        return false;
    }

    bool Serialize(JsonDocument& json, StringAnsi& output, const Array<StringAnsi>& order, AssetPipelineDiagnostic& diagnostic)
    {
        CanonicalJsonError error;
        if (CanonicalJsonWriter::Write(json, output, error, &order))
            return Fail(diagnostic, TEXT("Runtime asset index canonical serialization failed."));
        return false;
    }

    const JsonValue* Member(const JsonValue& object, const char* name)
    {
        const auto member = object.FindMember(name);
        return member == object.MemberEnd() ? nullptr : &member->value;
    }

    bool ReadString(const JsonValue& object, const char* name, String& value)
    {
        const JsonValue* member = Member(object, name);
        if (!member || !member->IsString())
            return true;
        value = String(StringAnsiView(member->GetString(), member->GetStringLength()));
        return false;
    }

    bool ReadGuid(const JsonValue& object, const char* name, Guid& value)
    {
        const JsonValue* member = Member(object, name);
        return !member || !member->IsString() || Guid::Parse(StringAnsiView(member->GetString(), member->GetStringLength()), value);
    }
}

bool RuntimeAssetIndex::ContainsLibraryPath(const StringView& path)
{
    String value(path);
    value.Replace('\\', '/');
    value = value.ToLower();
    return value.Contains(TEXT("/library/")) || value.StartsWith(TEXT("library/"));
}

bool RuntimeAssetIndex::WriteCanonicalJson(const Array<RuntimeAssetIndexEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    Array<RuntimeAssetIndexEntry> sorted = entries;
    if (sorted.Count() > 1)
    {
        std::sort(sorted.Get(), sorted.Get() + sorted.Count(), [](const RuntimeAssetIndexEntry& a, const RuntimeAssetIndexEntry& b)
        {
            const StringAnsi aGuid = GuidKey(a.ID.Guid);
            const StringAnsi bGuid = GuidKey(b.ID.Guid);
            return aGuid == bGuid ? a.ID.LocalId < b.ID.LocalId : aGuid < bGuid;
        });
    }

    JsonDocument payload;
    payload.SetObject();
    JsonAlloc& payloadAllocator = payload.GetAllocator();
    payload.AddMember("formatVersion", FormatVersion, payloadAllocator);
    JsonValue payloadAssets;
    if (BuildAssetsObject(sorted, payloadAssets, payloadAllocator, diagnostic))
        return true;
    payload.AddMember("assets", payloadAssets, payloadAllocator);
    Array<StringAnsi> payloadOrder;
    payloadOrder.Add("formatVersion");
    payloadOrder.Add("assets");
    StringAnsi payloadText;
    if (Serialize(payload, payloadText, payloadOrder, diagnostic))
        return true;
    const StringAnsi contentHash = ContentHash::Compute(payloadText.Get(), payloadText.Length()).ToString();

    JsonDocument json;
    json.SetObject();
    JsonAlloc& allocator = json.GetAllocator();
    json.AddMember("formatVersion", FormatVersion, allocator);
    AddAnsi(json, "contentHash", contentHash, allocator);
    JsonValue assets;
    if (BuildAssetsObject(sorted, assets, allocator, diagnostic))
        return true;
    json.AddMember("assets", assets, allocator);
    Array<StringAnsi> rootOrder;
    rootOrder.Add("formatVersion");
    rootOrder.Add("contentHash");
    rootOrder.Add("assets");
    if (Serialize(json, output, rootOrder, diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::Parse(const StringAnsiView& input, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
{
    entries.Clear();
    JsonDocument json;
    json.Parse(input.Get(), input.Length());
    if (json.HasParseError() || !json.IsObject() || json.MemberCount() != 3)
        return Fail(diagnostic, TEXT("Runtime asset index is malformed or has unexpected root fields."));
    const JsonValue* version = Member(json, "formatVersion");
    const JsonValue* hash = Member(json, "contentHash");
    const JsonValue* assets = Member(json, "assets");
    if (!version || !version->IsInt() || version->GetInt() != FormatVersion || !hash || !hash->IsString() || !assets || !assets->IsObject())
        return Fail(diagnostic, TEXT("Runtime asset index version, hash, or assets table is invalid."));

    HashSet<AssetObjectId> objectIds;
    HashSet<Guid> backingIds;
    entries.EnsureCapacity(static_cast<int32>(assets->MemberCount()));
    for (auto i = assets->MemberBegin(); i != assets->MemberEnd(); ++i)
    {
        if (!i->name.IsString() || !i->value.IsObject() || i->value.MemberCount() != 13)
            return Fail(diagnostic, TEXT("Runtime asset index contains a malformed asset location."));
        const JsonValue& item = i->value;
        RuntimeAssetIndexEntry entry;
        const JsonValue* localId = Member(item, "localId");
        const JsonValue* chunkId = Member(item, "chunkId");
        const JsonValue* offset = Member(item, "offset");
        const JsonValue* size = Member(item, "size");
        const JsonValue* assetFormatVersion = Member(item, "assetFormatVersion");
        const JsonValue* flags = Member(item, "flags");
        const JsonValue* artifact = Member(item, "exactArtifact");
        if (ReadGuid(item, "fileGuid", entry.ID.Guid) || !localId || !localId->IsInt64() ||
            ReadGuid(item, "backingGuid", entry.BackingAssetID) || ReadString(item, "type", entry.TypeName) ||
            ReadString(item, "canonicalPath", entry.CanonicalPath) || ReadString(item, "packagePath", entry.PackagedPath) ||
            ReadGuid(item, "packageId", entry.PackageID) || !chunkId || !chunkId->IsUint() || !offset || !offset->IsUint64() ||
            !size || !size->IsUint64() || !assetFormatVersion || !assetFormatVersion->IsUint() || !flags || !flags->IsUint() || !artifact)
            return Fail(diagnostic, TEXT("Runtime asset index asset location fields have invalid types."));
        entry.ID.LocalId = localId->GetInt64();
        entry.ChunkID = chunkId->GetUint();
        entry.Offset = offset->GetUint64();
        entry.Size = size->GetUint64();
        entry.AssetFormatVersion = assetFormatVersion->GetUint();
        entry.Flags = static_cast<RuntimeAssetIndexFlags>(flags->GetUint());
        if (!artifact->IsNull() && (!artifact->IsString() || ArtifactKey::Parse(StringAnsiView(artifact->GetString(), artifact->GetStringLength()), entry.ExactArtifact)))
            return Fail(diagnostic, TEXT("Runtime asset index exact artifact key is invalid."));
        if (ValidateEntry(entry, diagnostic) || ObjectKey(entry.ID) != StringAnsiView(i->name.GetString(), i->name.GetStringLength()) ||
            !objectIds.Add(entry.ID) || !backingIds.Add(entry.BackingAssetID))
            return Fail(diagnostic, TEXT("Runtime asset index identity keys are inconsistent or duplicated."));
        entries.Add(MoveTemp(entry));
    }

    StringAnsi canonical;
    if (WriteCanonicalJson(entries, canonical, diagnostic))
        return true;
    if (canonical.Length() != input.Length() || Platform::MemoryCompare(canonical.Get(), input.Get(), input.Length()) != 0)
        return Fail(diagnostic, TEXT("Runtime asset index is not canonical or failed content-hash verification."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool RuntimeAssetIndex::Load(const StringView& path, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (File::ReadAllText(path, json))
        return Fail(diagnostic, TEXT("Runtime asset index could not be read."));
    return Parse(json, entries, diagnostic);
}

bool RuntimeAssetIndex::SaveAtomic(const StringView& path, const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (WriteCanonicalJson(entries, json, diagnostic))
        return true;
    const String destination(path);
    const String staging = destination + TEXT(".tmp");
    if (File::WriteAllBytes(staging, json.Get(), json.Length()) || FileSystem::MoveFile(destination, staging, true))
    {
        FileSystem::DeleteFile(staging);
        return Fail(diagnostic, TEXT("Runtime asset index could not be written atomically."));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
