// Copyright (c) Wojciech Figat. All rights reserved.

#include "RuntimeAssetIndex.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
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
    JsonDocument json;
    json.SetObject();
    JsonAlloc& allocator = json.GetAllocator();
    json.AddMember("formatVersion", FormatVersion, allocator);
    JsonValue assets(rapidjson::kObjectType);
    for (const RuntimeAssetIndexEntry& entry : sorted)
    {
        if (!entry.ID.IsValid() || entry.PackagedPath.IsEmpty())
            return Fail(diagnostic, TEXT("Runtime asset index entries require an asset GUID, non-zero local file ID, and packaged path."));
        if (ContainsLibraryPath(entry.PackagedPath))
            return Fail(diagnostic, TEXT("Runtime asset index must not refer to project Library storage."));
        JsonValue item(rapidjson::kObjectType);
        const StringAnsi typeName(entry.TypeName);
        item.AddMember("type", JsonValue(typeName.Get(), typeName.Length(), allocator), allocator);
        const StringAnsi packaged(entry.PackagedPath);
        item.AddMember("path", JsonValue(packaged.Get(), packaged.Length(), allocator), allocator);
        const StringAnsi key = ObjectKey(entry.ID);
        assets.AddMember(JsonValue(key.Get(), key.Length(), allocator), item, allocator);
    }
    json.AddMember("assets", assets, allocator);
    CanonicalJsonError error;
    Array<StringAnsi> rootOrder;
    rootOrder.Add("formatVersion");
    rootOrder.Add("assets");
    if (CanonicalJsonWriter::Write(json, output, error, &rootOrder))
        return Fail(diagnostic, TEXT("Runtime asset index canonical serialization failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
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
