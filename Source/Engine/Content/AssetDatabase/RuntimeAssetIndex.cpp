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
            return GuidKey(a.ID) < GuidKey(b.ID);
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
            return Fail(diagnostic, TEXT("Runtime asset index entries require a GUID and packaged path."));
        if (ContainsLibraryPath(entry.PackagedPath))
            return Fail(diagnostic, TEXT("Runtime asset index must not refer to project Library storage."));
        JsonValue item(rapidjson::kObjectType);
        const StringAnsi typeName(entry.TypeName);
        item.AddMember("type", JsonValue(typeName.Get(), typeName.Length(), allocator), allocator);
        const StringAnsi packaged(entry.PackagedPath);
        item.AddMember("path", JsonValue(packaged.Get(), packaged.Length(), allocator), allocator);
        assets.AddMember(JsonValue(GuidKey(entry.ID).Get(), 32, allocator), item, allocator);
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
