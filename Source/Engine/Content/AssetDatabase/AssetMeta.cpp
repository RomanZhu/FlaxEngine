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

    const auto fileFormatVersion = document.FindMember("fileFormatVersion");
    const auto guid = document.FindMember("guid");
    const auto folderAsset = document.FindMember("folderAsset");
    const auto importer = document.FindMember("importer");
    const auto objectIds = document.FindMember("objectIds");
    const auto labels = document.FindMember("labels");
    if (fileFormatVersion == document.MemberEnd() || !fileFormatVersion->value.IsInt() || fileFormatVersion->value.GetInt() < 1 ||
        guid == document.MemberEnd() || !guid->value.IsString() ||
        folderAsset == document.MemberEnd() || !folderAsset->value.IsBool() ||
        importer == document.MemberEnd() || !importer->value.IsObject() ||
        labels == document.MemberEnd() || !labels->value.IsArray())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata is missing a required field or has an invalid field type."));

    result.FileFormatVersion = fileFormatVersion->value.GetInt();
    result.MetaUpgradeRequired = result.FileFormatVersion != CurrentFileFormatVersion;
    if (Guid::Parse(StringAnsiView(guid->value.GetString(), guid->value.GetStringLength()), result.ID) || !result.ID.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata GUID is invalid."));
    result.FolderAsset = folderAsset->value.GetBool();
    result.SourceKind = result.FolderAsset ? AssetSourceKind::Folder : AssetSourceKind::ImportedSource;

    const JsonValue& importerObject = importer->value;
    const auto importerId = importerObject.FindMember("id");
    const auto importerVersion = importerObject.FindMember("version");
    const auto settings = importerObject.FindMember("settings");
    if (importerId == importerObject.MemberEnd() || !importerId->value.IsString() ||
        importerVersion == importerObject.MemberEnd() || !importerVersion->value.IsInt() || importerVersion->value.GetInt() < 1 ||
        settings == importerObject.MemberEnd() || !settings->value.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata importer block is invalid."));
    result.Processor.ID = String(StringAnsiView(importerId->value.GetString(), importerId->value.GetStringLength()));
    if (!IsProcessorIdValid(result.Processor.ID))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata importer ID has an invalid shape."));
    result.Processor.SettingsVersion = importerVersion->value.GetInt();
    if (CanonicalJsonWriter::Write(settings->value, result.Processor.SettingsJson, canonicalError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Importer settings cannot be canonicalized."));
    const char* importerKnown[] = { "id", "version", "settings" };
    if (CaptureUnknown(importerObject, importerKnown, ARRAY_COUNT(importerKnown), result.Processor.UnknownFields, diagnostic, path))
        return true;

    HashSet<int64> localIds;
    localIds.Add(1);
    if (result.FolderAsset)
    {
        result.AssetType = TEXT("FlaxEngine.Folder");
        if (objectIds != document.MemberEnd())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Folder metadata must not contain an objectIds table."));
    }
    else
    {
        if (objectIds == document.MemberEnd() || !objectIds->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata objectIds table is missing or invalid."));
        const auto main = objectIds->value.FindMember("main");
        if (main == objectIds->value.MemberEnd() || !main->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata main object mapping is missing."));
        const auto mainFileId = main->value.FindMember("fileId");
        const auto mainType = main->value.FindMember("type");
        if (mainFileId == main->value.MemberEnd() || !mainFileId->value.IsInt64() || mainFileId->value.GetInt64() != 1 ||
            mainType == main->value.MemberEnd() || !mainType->value.IsString() || mainType->value.GetStringLength() == 0)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata main object must have fileId 1 and a type."));
        result.AssetType = String(StringAnsiView(mainType->value.GetString(), mainType->value.GetStringLength()));
        const char* mainKnown[] = { "fileId", "type" };
        if (CaptureUnknown(main->value, mainKnown, ARRAY_COUNT(mainKnown), result.MainObjectUnknownFields, diagnostic, path))
            return true;
    }

    if (!result.FolderAsset)
    {
        for (auto i = objectIds->value.MemberBegin(); i != objectIds->value.MemberEnd(); ++i)
        {
            if (StringAnsiView(i->name.GetString(), i->name.GetStringLength()) == "main")
                continue;
            const String rawKey(StringAnsiView(i->name.GetString(), i->name.GetStringLength()));
            const String stableKey = SubAssetPolicy::NormalizeKey(rawKey);
            if (!SubAssetPolicy::IsKeyValid(stableKey) || result.SubAssets.ContainsKey(stableKey) || !i->value.IsObject())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata contains an invalid or duplicate stable object identifier."));
            const JsonValue& object = i->value;
            const auto localId = object.FindMember("fileId");
            const auto collisionSalt = object.FindMember("collisionSalt");
            const auto type = object.FindMember("type");
            const auto name = object.FindMember("name");
            const auto removed = object.FindMember("removed");
            if (localId == object.MemberEnd() || !localId->value.IsInt64() || localId->value.GetInt64() <= 1 ||
                collisionSalt == object.MemberEnd() || !collisionSalt->value.IsUint() ||
                type == object.MemberEnd() || !type->value.IsString() || type->value.GetStringLength() == 0 ||
                (name != object.MemberEnd() && !name->value.IsString()) ||
                (removed != object.MemberEnd() && !removed->value.IsBool()))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata object mapping is invalid."));
            SubAssetMeta subAsset;
            subAsset.LocalId = localId->value.GetInt64();
            subAsset.CollisionSalt = collisionSalt->value.GetUint();
            if (!localIds.Add(subAsset.LocalId))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Asset metadata repeats a local file ID."));
            subAsset.TypeName = String(StringAnsiView(type->value.GetString(), type->value.GetStringLength()));
            subAsset.DisplayName = name == object.MemberEnd() ? stableKey : String(StringAnsiView(name->value.GetString(), name->value.GetStringLength()));
            subAsset.Removed = removed != object.MemberEnd() && removed->value.GetBool();
            const auto previousKeys = object.FindMember("previousIdentifiers");
            if (previousKeys != object.MemberEnd())
            {
                if (!previousKeys->value.IsArray())
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Object previousIdentifiers must be an array."));
                HashSet<String> previousSet;
                for (const JsonValue& previous : previousKeys->value.GetArray())
                {
                    if (!previous.IsString())
                        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Object previousIdentifiers values must be strings."));
                    const String previousKey = SubAssetPolicy::NormalizeKey(String(StringAnsiView(previous.GetString(), previous.GetStringLength())));
                    if (!SubAssetPolicy::IsKeyValid(previousKey) || previousKey == stableKey || !previousSet.Add(previousKey))
                        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, path, TEXT("Object previousIdentifiers contains an invalid or duplicate identifier."));
                    subAsset.PreviousKeys.Add(previousKey);
                }
            }
            const char* subAssetKnown[] = { "fileId", "collisionSalt", "type", "name", "removed", "previousIdentifiers" };
            if (CaptureUnknown(object, subAssetKnown, ARRAY_COUNT(subAssetKnown), subAsset.UnknownFields, diagnostic, path))
                return true;
            result.SubAssets.Add(stableKey, MoveTemp(subAsset));
            if (stableKey != rawKey)
                result.MetaUpgradeRequired = true;
        }
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
    const char* rootKnown[] = { "fileFormatVersion", "guid", "folderAsset", "importer", "objectIds", "labels", "userData" };
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
    if (FileFormatVersion != CurrentFileFormatVersion || !ID.IsValid() || !IsProcessorIdValid(Processor.ID) || Processor.SettingsVersion < 1 ||
        (!FolderAsset && AssetType.IsEmpty()))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Asset metadata has an invalid identity, importer, version, or main object type."));
    JsonDocument settingsDocument;
    settingsDocument.Parse(Processor.SettingsJson.Get(), Processor.SettingsJson.Length());
    CanonicalJsonError settingsError;
    if (settingsDocument.HasParseError() || !settingsDocument.IsObject() || CanonicalJsonWriter::Validate(settingsDocument, settingsError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Asset metadata importer settings must be a valid object."));
    if (UserDataJson.HasChars())
    {
        JsonDocument userDataDocument;
        userDataDocument.Parse(UserDataJson.Get(), UserDataJson.Length());
        CanonicalJsonError userDataError;
        if (userDataDocument.HasParseError() || !userDataDocument.IsObject() || CanonicalJsonWriter::Validate(userDataDocument, userDataError))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Asset metadata userData must be a valid object."));
    }

    JsonDocument document;
    document.SetObject();
    auto& allocator = document.GetAllocator();
    AddUnknownMembers(document, UnknownFields, allocator);
    document.AddMember("fileFormatVersion", FileFormatVersion, allocator);
    AddStringMember(document, "guid", ID.ToString(Guid::FormatType::N).ToLower(), allocator);
    document.AddMember("folderAsset", FolderAsset, allocator);

    JsonValue importer(rapidjson::kObjectType);
    AddUnknownMembers(importer, Processor.UnknownFields, allocator);
    AddStringMember(importer, "id", Processor.ID, allocator);
    importer.AddMember("version", Processor.SettingsVersion, allocator);
    AddFragmentMember(importer, "settings", Processor.SettingsJson, allocator);
    document.AddMember("importer", importer.Move(), allocator);

    JsonValue objectIds(rapidjson::kObjectType);
    if (!FolderAsset)
    {
        JsonValue mainObject(rapidjson::kObjectType);
        AddUnknownMembers(mainObject, MainObjectUnknownFields, allocator);
        mainObject.AddMember("fileId", 1, allocator);
        AddStringMember(mainObject, "type", AssetType, allocator);
        objectIds.AddMember("main", mainObject.Move(), allocator);
    }
    HashSet<int64> localIds;
    localIds.Add(1);
    for (const auto& entry : SubAssets)
    {
        if (FolderAsset || !SubAssetPolicy::IsKeyValid(entry.Key) || entry.Value.LocalId <= 1 || !localIds.Add(entry.Value.LocalId) || entry.Value.TypeName.IsEmpty())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Asset metadata contains an invalid or duplicate object mapping."));
        JsonValue subAsset(rapidjson::kObjectType);
        AddUnknownMembers(subAsset, entry.Value.UnknownFields, allocator);
        subAsset.AddMember("fileId", entry.Value.LocalId, allocator);
        subAsset.AddMember("collisionSalt", entry.Value.CollisionSalt, allocator);
        AddStringMember(subAsset, "type", entry.Value.TypeName, allocator);
        AddStringMember(subAsset, "name", entry.Value.DisplayName, allocator);
        subAsset.AddMember("removed", entry.Value.Removed, allocator);
        if (entry.Value.PreviousKeys.HasItems())
        {
            JsonValue previousKeys(rapidjson::kArrayType);
            Array<String> sorted = entry.Value.PreviousKeys;
            std::sort(sorted.Get(), sorted.Get() + sorted.Count(), [](const String& a, const String& b) { return a < b; });
            for (int32 i = 0; i < sorted.Count(); i++)
            {
                const String& previousKey = sorted[i];
                if (!SubAssetPolicy::IsKeyValid(previousKey) || previousKey == entry.Key || (i != 0 && previousKey == sorted[i - 1]))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Asset metadata contains an invalid or duplicate previous stable identifier."));
                const StringAnsi key(previousKey);
                previousKeys.PushBack(JsonValue(key.Get(), key.Length(), allocator).Move(), allocator);
            }
            subAsset.AddMember("previousIdentifiers", previousKeys.Move(), allocator);
        }
        const StringAnsi stableKey(entry.Key);
        objectIds.AddMember(JsonValue(stableKey.Get(), stableKey.Length(), allocator).Move(), subAsset.Move(), allocator);
    }
    if (!FolderAsset)
        document.AddMember("objectIds", objectIds.Move(), allocator);

    JsonValue labels(rapidjson::kArrayType);
    Array<String> sortedLabels = Labels;
    if (sortedLabels.Count() > 1)
        std::sort(sortedLabels.Get(), sortedLabels.Get() + sortedLabels.Count(), [](const String& a, const String& b) { return a < b; });
    for (int32 i = 0; i < sortedLabels.Count(); i++)
    {
        const String& label = sortedLabels[i];
        if (label.IsEmpty() || (i != 0 && label == sortedLabels[i - 1]))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Asset metadata labels must be unique non-empty strings."));
        const StringAnsi value(label);
        labels.PushBack(JsonValue(value.Get(), value.Length(), allocator).Move(), allocator);
    }
    document.AddMember("labels", labels.Move(), allocator);
    if (UserDataJson.HasChars())
        AddFragmentMember(document, "userData", UserDataJson, allocator);
    else
        AddFragmentMember(document, "userData", StringAnsiView("{}"), allocator);

    Array<StringAnsi> rootOrder;
    rootOrder.Add("fileFormatVersion");
    rootOrder.Add("guid");
    rootOrder.Add("folderAsset");
    rootOrder.Add("importer");
    rootOrder.Add("objectIds");
    rootOrder.Add("labels");
    rootOrder.Add("userData");
    Dictionary<StringAnsi, Array<StringAnsi>> objectOrders;
    Array<StringAnsi> importerOrder;
    importerOrder.Add("id");
    importerOrder.Add("version");
    importerOrder.Add("settings");
    objectOrders.Add("/importer", importerOrder);
    Array<StringAnsi> objectIdsOrder;
    objectIdsOrder.Add("main");
    objectOrders.Add("/objectIds", objectIdsOrder);
    Array<StringAnsi> mainObjectOrder;
    mainObjectOrder.Add("fileId");
    mainObjectOrder.Add("type");
    objectOrders.Add("/objectIds/main", mainObjectOrder);
    Array<StringAnsi> subAssetOrder;
    subAssetOrder.Add("fileId");
    subAssetOrder.Add("collisionSalt");
    subAssetOrder.Add("type");
    subAssetOrder.Add("name");
    subAssetOrder.Add("removed");
    subAssetOrder.Add("previousIdentifiers");
    for (const auto& entry : SubAssets)
        objectOrders.Add(StringAnsi("/objectIds/") + StringAnsi(entry.Key), subAssetOrder);
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
    if (Load(staging, reparsed, diagnostic) || reparsed.ID != value.ID || (!value.FolderAsset && reparsed.AssetType != value.AssetType))
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
    return result;
}
