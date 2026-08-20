// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetMeta.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/Crc.h"
#include <algorithm>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    typedef rapidjson_flax::Value JsonValue;
    typedef rapidjson_flax::Document JsonDocument;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    const Char* ToSourceKindName(AssetSourceKind kind)
    {
        switch (kind)
        {
        case AssetSourceKind::ImportedSource: return TEXT("ImportedSource");
        case AssetSourceKind::TextDocument: return TEXT("TextDocument");
        case AssetSourceKind::ExistingJson: return TEXT("ExistingJson");
        case AssetSourceKind::LegacyBinary: return TEXT("LegacyBinary");
        default: return TEXT("ImportedSource");
        }
    }

    bool TryParseSourceKind(const StringAnsiView& text, AssetSourceKind& kind)
    {
        if (text == "ImportedSource") kind = AssetSourceKind::ImportedSource;
        else if (text == "TextDocument") kind = AssetSourceKind::TextDocument;
        else if (text == "ExistingJson") kind = AssetSourceKind::ExistingJson;
        else if (text == "LegacyBinary") kind = AssetSourceKind::LegacyBinary;
        else return false;
        return true;
    }

    bool IsProcessorIdValid(const StringView& id)
    {
        if (id.IsEmpty())
            return false;
        for (int32 i = 0; i < id.Length(); i++)
        {
            const Char c = id[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
                return false;
        }
        return true;
    }

    bool IsKnown(const StringAnsiView& name, const char* const* fields, int32 count)
    {
        for (int32 i = 0; i < count; i++)
        {
            if (name == fields[i])
                return true;
        }
        return false;
    }

    bool CaptureUnknown(const JsonValue& object, const char* const* known, int32 knownCount, Dictionary<StringAnsi, StringAnsi>& output, AssetPipelineDiagnostic& diagnostic, const StringView& path)
    {
        CanonicalJsonError error;
        for (auto i = object.MemberBegin(); i != object.MemberEnd(); ++i)
        {
            const StringAnsiView name(i->name.GetString(), i->name.GetStringLength());
            if (IsKnown(name, known, knownCount))
                continue;
            StringAnsi json;
            if (CanonicalJsonWriter::Write(i->value, json, error))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Cannot preserve an unknown metadata field."));
            output.Add(StringAnsi(name), MoveTemp(json));
        }
        return false;
    }

    void AddStringMember(JsonValue& object, const char* name, const StringView& value, JsonDocument::AllocatorType& allocator)
    {
        const StringAnsi utf8(value);
        JsonValue key(name, allocator);
        JsonValue data(utf8.Get(), utf8.Length(), allocator);
        object.AddMember(key.Move(), data.Move(), allocator);
    }

    void AddAnsiStringMember(JsonValue& object, const char* name, const StringAnsiView& value, JsonDocument::AllocatorType& allocator)
    {
        JsonValue key(name, allocator);
        JsonValue data(value.Get(), value.Length(), allocator);
        object.AddMember(key.Move(), data.Move(), allocator);
    }

    void AddFragmentMember(JsonValue& object, const StringAnsiView& name, const StringAnsiView& fragment, JsonDocument::AllocatorType& allocator)
    {
        JsonDocument parsed;
        parsed.Parse(fragment.Get(), fragment.Length());
        JsonValue key(name.Get(), name.Length(), allocator);
        JsonValue data;
        data.CopyFrom(parsed, allocator);
        object.AddMember(key.Move(), data.Move(), allocator);
    }

    void AddUnknownMembers(JsonValue& object, const Dictionary<StringAnsi, StringAnsi>& fields, JsonDocument::AllocatorType& allocator)
    {
        for (const auto& field : fields)
            AddFragmentMember(object, field.Key, field.Value, allocator);
    }

    bool FlushWrittenFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        HANDLE handle = CreateFileW(*value, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#else
        return false;
#endif
    }

    bool AtomicReplace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath(destination);
        const String stagingPath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0;
#else
        return FileSystem::MoveFile(destination, staging, true);
#endif
    }
}

bool AssetMeta::Parse(const StringAnsiView& json, const StringView& path, AssetMeta& result, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    result = AssetMeta();
    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError())
    {
        diagnostic.Location.Column = (int32)document.GetErrorOffset();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaParseError, path, TEXT("Asset metadata JSON parsing failed."));
    }
    CanonicalJsonError canonicalError;
    if (CanonicalJsonWriter::Validate(document, canonicalError))
        return Fail(diagnostic, canonicalError.Code == CanonicalJsonErrorCode::DuplicateKey ? AssetPipelineDiagnosticCode::MetaParseError : AssetPipelineDiagnosticCode::InvalidMeta, path, canonicalError.Message);
    if (!document.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata root must be an object."));

    const auto metaVersion = document.FindMember("metaVersion");
    const auto guid = document.FindMember("guid");
    const auto assetType = document.FindMember("assetType");
    const auto sourceKind = document.FindMember("sourceKind");
    const auto processor = document.FindMember("processor");
    const auto subAssets = document.FindMember("subAssets");
    const auto labels = document.FindMember("labels");
    if (metaVersion == document.MemberEnd() || !metaVersion->value.IsInt() || metaVersion->value.GetInt() < 0 ||
        guid == document.MemberEnd() || !guid->value.IsString() ||
        assetType == document.MemberEnd() || !assetType->value.IsString() || assetType->value.GetStringLength() == 0 ||
        sourceKind == document.MemberEnd() || !sourceKind->value.IsString() ||
        processor == document.MemberEnd() || !processor->value.IsObject() ||
        subAssets == document.MemberEnd() || !subAssets->value.IsObject() ||
        labels == document.MemberEnd() || !labels->value.IsArray())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata is missing a required field or has an invalid field type."));

    result.MetaVersion = metaVersion->value.GetInt();
    result.MetaUpgradeRequired = result.MetaVersion != CurrentMetaVersion;
    if (Guid::Parse(StringAnsiView(guid->value.GetString(), guid->value.GetStringLength()), result.ID) || !result.ID.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata GUID is invalid."));
    result.AssetType = String(StringAnsiView(assetType->value.GetString(), assetType->value.GetStringLength()));
    if (!TryParseSourceKind(StringAnsiView(sourceKind->value.GetString(), sourceKind->value.GetStringLength()), result.SourceKind))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata sourceKind is invalid."));

    const JsonValue& processorObject = processor->value;
    const auto processorId = processorObject.FindMember("id");
    const auto settingsVersion = processorObject.FindMember("settingsVersion");
    const auto settings = processorObject.FindMember("settings");
    if (processorId == processorObject.MemberEnd() || !processorId->value.IsString() ||
        settingsVersion == processorObject.MemberEnd() || !settingsVersion->value.IsInt() || settingsVersion->value.GetInt() < 1 ||
        settings == processorObject.MemberEnd() || !settings->value.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata processor block is invalid."));
    result.Processor.ID = String(StringAnsiView(processorId->value.GetString(), processorId->value.GetStringLength()));
    if (!IsProcessorIdValid(result.Processor.ID))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata processor ID has an invalid shape."));
    result.Processor.SettingsVersion = settingsVersion->value.GetInt();
    if (CanonicalJsonWriter::Write(settings->value, result.Processor.SettingsJson, canonicalError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Processor settings cannot be canonicalized."));
    const char* processorKnown[] = { "id", "settingsVersion", "settings" };
    if (CaptureUnknown(processorObject, processorKnown, ARRAY_COUNT(processorKnown), result.Processor.UnknownFields, diagnostic, path))
        return true;

    HashSet<Guid> ids;
    ids.Add(result.ID);
    for (auto i = subAssets->value.MemberBegin(); i != subAssets->value.MemberEnd(); ++i)
    {
        const String rawKey(StringAnsiView(i->name.GetString(), i->name.GetStringLength()));
        const String stableKey = SubAssetPolicy::NormalizeKey(rawKey);
        if (!SubAssetPolicy::IsKeyValid(stableKey) || result.SubAssets.ContainsKey(stableKey) || !i->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata contains an invalid or duplicate subasset key."));
        const JsonValue& object = i->value;
        const auto id = object.FindMember("guid");
        const auto type = object.FindMember("type");
        const auto name = object.FindMember("name");
        const auto removed = object.FindMember("removed");
        if (id == object.MemberEnd() || !id->value.IsString() || type == object.MemberEnd() || !type->value.IsString() || type->value.GetStringLength() == 0 ||
            name == object.MemberEnd() || !name->value.IsString() || removed == object.MemberEnd() || !removed->value.IsBool())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata subasset mapping is invalid."));
        SubAssetMeta subAsset;
        if (Guid::Parse(StringAnsiView(id->value.GetString(), id->value.GetStringLength()), subAsset.ID) || !subAsset.ID.IsValid())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Subasset GUID is invalid."));
        if (ids.Contains(subAsset.ID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::DuplicateGuid, path, TEXT("Asset metadata repeats a root or subasset GUID."));
        ids.Add(subAsset.ID);
        subAsset.TypeName = String(StringAnsiView(type->value.GetString(), type->value.GetStringLength()));
        subAsset.DisplayName = String(StringAnsiView(name->value.GetString(), name->value.GetStringLength()));
        subAsset.Removed = removed->value.GetBool();
        const auto previousKeys = object.FindMember("previousKeys");
        if (previousKeys != object.MemberEnd())
        {
            if (!previousKeys->value.IsArray())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Subasset previousKeys must be an array."));
            HashSet<String> previousSet;
            for (const JsonValue& previous : previousKeys->value.GetArray())
            {
                if (!previous.IsString())
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Subasset previousKeys values must be strings."));
                const String previousKey = SubAssetPolicy::NormalizeKey(String(StringAnsiView(previous.GetString(), previous.GetStringLength())));
                if (!SubAssetPolicy::IsKeyValid(previousKey) || previousSet.Contains(previousKey))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Subasset previousKeys contains an invalid or duplicate key."));
                previousSet.Add(previousKey);
                subAsset.PreviousKeys.Add(previousKey);
            }
        }
        const char* subAssetKnown[] = { "guid", "type", "name", "removed", "previousKeys" };
        if (CaptureUnknown(object, subAssetKnown, ARRAY_COUNT(subAssetKnown), subAsset.UnknownFields, diagnostic, path))
            return true;
        result.SubAssets.Add(stableKey, MoveTemp(subAsset));
        if (stableKey != rawKey)
            result.MetaUpgradeRequired = true;
    }

    HashSet<String> labelSet;
    for (const JsonValue& label : labels->value.GetArray())
    {
        if (!label.IsString() || label.GetStringLength() == 0)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata labels must be non-empty strings."));
        const String value(StringAnsiView(label.GetString(), label.GetStringLength()));
        if (labelSet.Contains(value))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata labels must be unique."));
        labelSet.Add(value);
        result.Labels.Add(value);
    }
    const auto userData = document.FindMember("userData");
    if (userData != document.MemberEnd())
    {
        if (!userData->value.IsObject() || CanonicalJsonWriter::Write(userData->value, result.UserDataJson, canonicalError))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata userData must be an object."));
    }
    const char* rootKnown[] = { "metaVersion", "guid", "assetType", "sourceKind", "processor", "subAssets", "labels", "userData" };
    if (CaptureUnknown(document, rootKnown, ARRAY_COUNT(rootKnown), result.UnknownFields, diagnostic, path))
        return true;
    diagnostic.AssetGuid = result.ID;
    return false;
}

bool AssetMeta::Load(const StringView& path, AssetMeta& result, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (File::ReadAllText(path, json))
        return Fail(diagnostic, FileSystem::FileExists(path) ? AssetPipelineDiagnosticCode::MetaParseError : AssetPipelineDiagnosticCode::MissingMeta, path, TEXT("Cannot read asset metadata sidecar."));
    return Parse(json, path, result, diagnostic);
}

bool AssetMeta::ToJson(StringAnsi& output, AssetPipelineDiagnostic& diagnostic) const
{
    diagnostic = AssetPipelineDiagnostic();
    JsonDocument document;
    document.SetObject();
    auto& allocator = document.GetAllocator();
    AddUnknownMembers(document, UnknownFields, allocator);
    document.AddMember("metaVersion", MetaVersion, allocator);
    AddStringMember(document, "guid", ID.ToString(Guid::FormatType::N).ToLower(), allocator);
    AddStringMember(document, "assetType", AssetType, allocator);
    AddStringMember(document, "sourceKind", ToSourceKindName(SourceKind), allocator);

    JsonValue processor(rapidjson::kObjectType);
    AddUnknownMembers(processor, Processor.UnknownFields, allocator);
    AddStringMember(processor, "id", Processor.ID, allocator);
    processor.AddMember("settingsVersion", Processor.SettingsVersion, allocator);
    AddFragmentMember(processor, "settings", Processor.SettingsJson, allocator);
    document.AddMember("processor", processor.Move(), allocator);

    JsonValue subAssets(rapidjson::kObjectType);
    for (const auto& entry : SubAssets)
    {
        JsonValue subAsset(rapidjson::kObjectType);
        AddUnknownMembers(subAsset, entry.Value.UnknownFields, allocator);
        AddStringMember(subAsset, "guid", entry.Value.ID.ToString(Guid::FormatType::N).ToLower(), allocator);
        AddStringMember(subAsset, "type", entry.Value.TypeName, allocator);
        AddStringMember(subAsset, "name", entry.Value.DisplayName, allocator);
        subAsset.AddMember("removed", entry.Value.Removed, allocator);
        if (entry.Value.PreviousKeys.HasItems())
        {
            JsonValue previousKeys(rapidjson::kArrayType);
            Array<String> sorted = entry.Value.PreviousKeys;
            std::sort(sorted.Get(), sorted.Get() + sorted.Count(), [](const String& a, const String& b) { return a < b; });
            for (const String& previousKey : sorted)
            {
                const StringAnsi key(previousKey);
                previousKeys.PushBack(JsonValue(key.Get(), key.Length(), allocator).Move(), allocator);
            }
            subAsset.AddMember("previousKeys", previousKeys.Move(), allocator);
        }
        const StringAnsi stableKey(entry.Key);
        subAssets.AddMember(JsonValue(stableKey.Get(), stableKey.Length(), allocator).Move(), subAsset.Move(), allocator);
    }
    document.AddMember("subAssets", subAssets.Move(), allocator);

    JsonValue labels(rapidjson::kArrayType);
    Array<String> sortedLabels = Labels;
    if (sortedLabels.Count() > 1)
        std::sort(sortedLabels.Get(), sortedLabels.Get() + sortedLabels.Count(), [](const String& a, const String& b) { return a < b; });
    for (const String& label : sortedLabels)
    {
        const StringAnsi value(label);
        labels.PushBack(JsonValue(value.Get(), value.Length(), allocator).Move(), allocator);
    }
    document.AddMember("labels", labels.Move(), allocator);
    if (UserDataJson.HasChars())
        AddFragmentMember(document, "userData", UserDataJson, allocator);

    Array<StringAnsi> rootOrder;
    rootOrder.Add("metaVersion");
    rootOrder.Add("guid");
    rootOrder.Add("assetType");
    rootOrder.Add("sourceKind");
    rootOrder.Add("processor");
    rootOrder.Add("subAssets");
    rootOrder.Add("labels");
    rootOrder.Add("userData");
    Dictionary<StringAnsi, Array<StringAnsi>> objectOrders;
    Array<StringAnsi> processorOrder;
    processorOrder.Add("id");
    processorOrder.Add("settingsVersion");
    processorOrder.Add("settings");
    objectOrders.Add("/processor", processorOrder);
    Array<StringAnsi> subAssetOrder;
    subAssetOrder.Add("guid");
    subAssetOrder.Add("type");
    subAssetOrder.Add("name");
    subAssetOrder.Add("removed");
    subAssetOrder.Add("previousKeys");
    subAssetOrder.Add("userData");
    for (const auto& entry : SubAssets)
        objectOrders.Add(StringAnsi("/subAssets/") + StringAnsi(entry.Key), subAssetOrder);
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(document, output, error, &rootOrder, &objectOrders))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, error.Message);
    return false;
}

bool AssetMeta::SaveAtomic(const StringView& path, const AssetMeta& value, AssetPipelineDiagnostic& diagnostic, uint32* selfWriteHash, AssetMetaWriteFailurePoint failurePoint)
{
    if (failurePoint == AssetMetaWriteFailurePoint::BeforeWrite)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Injected metadata failure before write."));
    StringAnsi json;
    if (value.ToJson(json, diagnostic))
        return true;
    const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
    SCOPE_EXIT { FileSystem::DeleteFile(staging); };
    if (File::WriteAllBytes(staging, json.Get(), json.Length()) || FlushWrittenFile(staging))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Cannot write or flush metadata staging file."));
    if (failurePoint == AssetMetaWriteFailurePoint::AfterWrite)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Injected metadata failure after write."));

    AssetMeta reparsed;
    if (Load(staging, reparsed, diagnostic) || reparsed.ID != value.ID || reparsed.AssetType != value.AssetType)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Metadata staging validation failed."));
    if (failurePoint == AssetMetaWriteFailurePoint::AfterValidate || failurePoint == AssetMetaWriteFailurePoint::BeforeReplace)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Injected metadata failure before replace."));
    if (FileSystem::FileExists(path) && FileSystem::IsReadOnly(path))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Metadata sidecar is read-only."));
    if (AtomicReplace(path, staging))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Cannot atomically replace metadata sidecar."));
    if (selfWriteHash)
        *selfWriteHash = Crc::MemCrc32(json.Get(), json.Length());
    return false;
}

AssetMeta AssetMeta::CloneWithNewIdentities() const
{
    AssetMeta result = *this;
    result.ID = Guid::New();
    SubAssetPolicy::RegenerateGuids(result.SubAssets);
    return result;
}
