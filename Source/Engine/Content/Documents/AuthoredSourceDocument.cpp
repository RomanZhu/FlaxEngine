// Copyright (c) Wojciech Figat. All rights reserved.

#include "AuthoredSourceDocument.h"
#include "CanonicalJsonWriter.h"
#include "Engine/Content/AssetDatabase/SubAsset.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Serialization/Json.h"
#include <algorithm>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAllocator;

    void AddString(JsonValue& object, const char* key, const StringView& value, JsonAllocator& allocator)
    {
        const StringAnsi text(value);
        object.AddMember(JsonValue(key, allocator), JsonValue(text.Get(), text.Length(), allocator), allocator);
    }

    bool ReadString(const JsonValue& object, const char* key, String& value, bool required = true)
    {
        const auto member = object.FindMember(key);
        if (member == object.MemberEnd())
            return required;
        if (!member->value.IsString())
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    bool CanonicalFragment(const JsonValue& value, StringAnsi& output, String& error)
    {
        CanonicalJsonError canonicalError;
        if (CanonicalJsonWriter::Write(value, output, canonicalError))
        {
            error = canonicalError.Message;
            return true;
        }
        return false;
    }

    bool AddFragment(JsonValue& object, const char* key, const StringAnsiView& json, JsonAllocator& allocator, String& error)
    {
        JsonDocument fragment;
        fragment.Parse(json.Get(), json.Length());
        if (fragment.HasParseError())
        {
            error = String::Format(TEXT("Authored object field '{0}' contains malformed JSON."), String(key));
            return true;
        }
        JsonValue value;
        value.CopyFrom(fragment, allocator);
        object.AddMember(JsonValue(key, allocator), value.Move(), allocator);
        return false;
    }

    bool IsKnownRootField(const StringAnsiView& name)
    {
        return name == "flaxSourceVersion" || name == "documentType" || name == "objects" ||
               name == "mainObjectFileId" || name == "tombstones";
    }

    bool IsKnownObjectField(const StringAnsiView& name)
    {
        return name == "fileId" || name == "stableKey" || name == "type" || name == "schemaVersion" ||
               name == "name" || name == "data";
    }

    bool IsKnownTombstoneField(const StringAnsiView& name)
    {
        return name == "fileId" || name == "stableKey" || name == "type" || name == "name";
    }

    bool CaptureUnknown(const JsonValue& object, bool (*isKnown)(const StringAnsiView&),
        Dictionary<StringAnsi, StringAnsi>& output, String& error)
    {
        for (auto i = object.MemberBegin(); i != object.MemberEnd(); ++i)
        {
            const StringAnsiView name(i->name.GetString(), i->name.GetStringLength());
            if (isKnown(name))
                continue;
            StringAnsi json;
            if (CanonicalFragment(i->value, json, error))
                return true;
            output[StringAnsi(name)] = MoveTemp(json);
        }
        return false;
    }

    bool AddUnknown(JsonValue& object, const Dictionary<StringAnsi, StringAnsi>& fields, JsonAllocator& allocator, String& error)
    {
        for (const auto& field : fields)
        {
            JsonDocument fragment;
            fragment.Parse(field.Value.Get(), field.Value.Length());
            if (fragment.HasParseError())
            {
                error = TEXT("An authored source extension field contains malformed JSON.");
                return true;
            }
            JsonValue value;
            value.CopyFrom(fragment, allocator);
            object.AddMember(JsonValue(field.Key.Get(), field.Key.Length(), allocator), value.Move(), allocator);
        }
        return false;
    }

    bool IsValidPayload(const StringAnsiView& json)
    {
        JsonDocument value;
        value.Parse(json.Get(), json.Length());
        return !value.HasParseError();
    }
}

bool AuthoredSourceDocument::Parse(const StringAnsiView& json, AuthoredSourceDocument& result, String& error)
{
    result = AuthoredSourceDocument();
    error.Clear();
    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError() || !document.IsObject())
    {
        error = TEXT("Authored source document is malformed or is not an object.");
        return true;
    }
    if (document.HasMember("guid") || document.HasMember("ID"))
    {
        error = TEXT("Authored source identity belongs only in adjacent metadata.");
        return true;
    }
    const auto version = document.FindMember("flaxSourceVersion");
    const auto objects = document.FindMember("objects");
    const auto mainObject = document.FindMember("mainObjectFileId");
    const auto tombstones = document.FindMember("tombstones");
    if (version == document.MemberEnd() || !version->value.IsInt() ||
        objects == document.MemberEnd() || !objects->value.IsArray() ||
        mainObject == document.MemberEnd() || !mainObject->value.IsInt64() ||
        ReadString(document, "documentType", result.DocumentType) || result.DocumentType.IsEmpty() ||
        (tombstones != document.MemberEnd() && !tombstones->value.IsArray()))
    {
        error = TEXT("Authored source document is missing a required field or has an invalid field type.");
        return true;
    }
    result.Version = version->value.GetInt();
    if (result.Version != CurrentVersion)
    {
        error = result.Version > CurrentVersion
            ? TEXT("Authored source document uses a newer unsupported schema and is read-only.")
            : TEXT("Authored source document requires an explicit migration before editing.");
        return true;
    }
    result.MainObjectLocalId = mainObject->value.GetInt64();
    HashSet<int64> ids;
    HashSet<String> keys;
    for (const JsonValue& value : objects->value.GetArray())
    {
        if (!value.IsObject())
        {
            error = TEXT("Authored source objects must be JSON objects.");
            return true;
        }
        const auto localId = value.FindMember("fileId");
        const auto schemaVersion = value.FindMember("schemaVersion");
        const auto data = value.FindMember("data");
        AuthoredSourceObject object;
        if (localId == value.MemberEnd() || !localId->value.IsInt64() || localId->value.GetInt64() <= 0 ||
            schemaVersion == value.MemberEnd() || !schemaVersion->value.IsInt() || schemaVersion->value.GetInt() <= 0 ||
            data == value.MemberEnd() || ReadString(value, "stableKey", object.StableKey) ||
            ReadString(value, "type", object.TypeName) || object.TypeName.IsEmpty() ||
            ReadString(value, "name", object.Name, false))
        {
            error = TEXT("An authored source object has invalid identity, type, schema, name, or data.");
            return true;
        }
        object.LocalId = localId->value.GetInt64();
        object.SchemaVersion = schemaVersion->value.GetInt();
        object.StableKey = SubAssetPolicy::NormalizeKey(object.StableKey);
        if (!SubAssetPolicy::IsKeyValid(object.StableKey) || !ids.Add(object.LocalId) || !keys.Add(object.StableKey) ||
            CanonicalFragment(data->value, object.DataJson, error) || CaptureUnknown(value, &IsKnownObjectField, object.UnknownFields, error))
        {
            if (error.IsEmpty())
                error = TEXT("Authored source live object identities and stable keys must be valid and unique.");
            return true;
        }
        result.Objects.Add(MoveTemp(object));
    }
    if (result.Objects.IsEmpty() || result.FindObject(result.MainObjectLocalId) == nullptr)
    {
        error = TEXT("Authored source document must contain its selected main object.");
        return true;
    }
    if (tombstones != document.MemberEnd())
    {
        for (const JsonValue& value : tombstones->value.GetArray())
        {
            if (!value.IsObject())
            {
                error = TEXT("An authored source tombstone is invalid.");
                return true;
            }
            AuthoredSourceTombstone tombstone;
            const auto localId = value.FindMember("fileId");
            if (localId == value.MemberEnd() || !localId->value.IsInt64() || localId->value.GetInt64() <= 0 ||
                ReadString(value, "stableKey", tombstone.StableKey) || ReadString(value, "type", tombstone.TypeName) ||
                tombstone.TypeName.IsEmpty() || ReadString(value, "name", tombstone.Name, false))
            {
                error = TEXT("An authored source tombstone is invalid.");
                return true;
            }
            tombstone.LocalId = localId->value.GetInt64();
            tombstone.StableKey = SubAssetPolicy::NormalizeKey(tombstone.StableKey);
            if (!SubAssetPolicy::IsKeyValid(tombstone.StableKey) || !ids.Add(tombstone.LocalId) || !keys.Add(tombstone.StableKey) ||
                CaptureUnknown(value, &IsKnownTombstoneField, tombstone.UnknownFields, error))
            {
                if (error.IsEmpty())
                    error = TEXT("Authored source tombstones reserve unique identities and stable keys.");
                return true;
            }
            result.Tombstones.Add(MoveTemp(tombstone));
        }
    }
    if (CaptureUnknown(document, &IsKnownRootField, result.UnknownFields, error))
        return true;
    return false;
}

bool AuthoredSourceDocument::ToCanonicalJson(StringAnsi& json, String& error) const
{
    json.Clear();
    error.Clear();
    if (Version != CurrentVersion || DocumentType.IsEmpty() || Objects.IsEmpty() || FindObject(MainObjectLocalId) == nullptr)
    {
        error = TEXT("Authored source document state is incomplete or unsupported.");
        return true;
    }
    AuthoredSourceDocument verified = *this;
    if (verified.Objects.Count() > 1)
        std::sort(verified.Objects.Get(), verified.Objects.Get() + verified.Objects.Count(), [](const AuthoredSourceObject& a, const AuthoredSourceObject& b) { return a.LocalId < b.LocalId; });
    if (verified.Tombstones.Count() > 1)
        std::sort(verified.Tombstones.Get(), verified.Tombstones.Get() + verified.Tombstones.Count(), [](const AuthoredSourceTombstone& a, const AuthoredSourceTombstone& b) { return a.LocalId < b.LocalId; });

    JsonDocument document;
    document.SetObject();
    JsonAllocator& allocator = document.GetAllocator();
    if (AddUnknown(document, verified.UnknownFields, allocator, error))
        return true;
    document.AddMember("flaxSourceVersion", CurrentVersion, allocator);
    AddString(document, "documentType", verified.DocumentType, allocator);
    JsonValue objects(rapidjson::kArrayType);
    Dictionary<StringAnsi, Array<StringAnsi>> objectOrders;
    for (int32 i = 0; i < verified.Objects.Count(); i++)
    {
        const AuthoredSourceObject& object = verified.Objects[i];
        JsonValue value(rapidjson::kObjectType);
        if (AddUnknown(value, object.UnknownFields, allocator, error))
            return true;
        value.AddMember("fileId", object.LocalId, allocator);
        AddString(value, "stableKey", object.StableKey, allocator);
        AddString(value, "type", object.TypeName, allocator);
        value.AddMember("schemaVersion", object.SchemaVersion, allocator);
        AddString(value, "name", object.Name, allocator);
        if (AddFragment(value, "data", object.DataJson, allocator, error))
            return true;
        objects.PushBack(value.Move(), allocator);
        Array<StringAnsi> order;
        order.Add("fileId"); order.Add("stableKey"); order.Add("type"); order.Add("schemaVersion"); order.Add("name"); order.Add("data");
        objectOrders.Add(StringAnsi("/objects/") + StringAnsi(StringUtils::ToString(i)), order);
    }
    document.AddMember("objects", objects.Move(), allocator);
    document.AddMember("mainObjectFileId", MainObjectLocalId, allocator);
    JsonValue tombstones(rapidjson::kArrayType);
    for (int32 i = 0; i < verified.Tombstones.Count(); i++)
    {
        const AuthoredSourceTombstone& tombstone = verified.Tombstones[i];
        JsonValue value(rapidjson::kObjectType);
        if (AddUnknown(value, tombstone.UnknownFields, allocator, error))
            return true;
        value.AddMember("fileId", tombstone.LocalId, allocator);
        AddString(value, "stableKey", tombstone.StableKey, allocator);
        AddString(value, "type", tombstone.TypeName, allocator);
        AddString(value, "name", tombstone.Name, allocator);
        tombstones.PushBack(value.Move(), allocator);
        Array<StringAnsi> order;
        order.Add("fileId"); order.Add("stableKey"); order.Add("type"); order.Add("name");
        objectOrders.Add(StringAnsi("/tombstones/") + StringAnsi(StringUtils::ToString(i)), order);
    }
    document.AddMember("tombstones", tombstones.Move(), allocator);
    Array<StringAnsi> rootOrder;
    rootOrder.Add("flaxSourceVersion"); rootOrder.Add("documentType"); rootOrder.Add("objects");
    rootOrder.Add("mainObjectFileId"); rootOrder.Add("tombstones");
    CanonicalJsonError canonicalError;
    if (CanonicalJsonWriter::Write(document, json, canonicalError, &rootOrder, &objectOrders))
    {
        error = canonicalError.Message;
        return true;
    }
    return false;
}

AuthoredSourceObject* AuthoredSourceDocument::FindObject(int64 localId)
{
    for (AuthoredSourceObject& object : Objects)
    {
        if (object.LocalId == localId)
            return &object;
    }
    return nullptr;
}

const AuthoredSourceObject* AuthoredSourceDocument::FindObject(int64 localId) const
{
    for (const AuthoredSourceObject& object : Objects)
    {
        if (object.LocalId == localId)
            return &object;
    }
    return nullptr;
}

bool AuthoredSourceDocument::AddObject(const StringView& stableKey, const StringView& typeName, const StringView& name,
    const StringAnsiView& dataJson, int64& localId, String& error)
{
    const String key = SubAssetPolicy::NormalizeKey(stableKey);
    if (!SubAssetPolicy::IsKeyValid(key) || typeName.IsEmpty() || !IsValidPayload(dataJson))
    {
        error = TEXT("Authored object stable key, type, or serialized data is invalid.");
        return true;
    }
    HashSet<int64> reserved;
    reserved.Add(1);
    for (const AuthoredSourceObject& object : Objects)
    {
        if (object.StableKey == key)
        {
            error = TEXT("The authored object stable key already exists.");
            return true;
        }
        reserved.Add(object.LocalId);
    }
    for (const AuthoredSourceTombstone& tombstone : Tombstones)
    {
        if (tombstone.StableKey == key)
        {
            error = TEXT("The authored object stable key is permanently reserved by a tombstone.");
            return true;
        }
        reserved.Add(tombstone.LocalId);
    }
    AuthoredSourceObject object;
    object.LocalId = SubAssetPolicy::AllocateLocalId(TEXT("Flax.AuthoredObject"), key, typeName, reserved);
    object.StableKey = key;
    object.TypeName = typeName;
    object.Name = name;
    object.DataJson = StringAnsi(dataJson);
    localId = object.LocalId;
    Objects.Add(MoveTemp(object));
    return false;
}

bool AuthoredSourceDocument::RemoveObject(int64 localId, String& error)
{
    if (localId == MainObjectLocalId || localId == 1)
    {
        error = TEXT("The selected or compatibility-root object cannot be removed.");
        return true;
    }
    for (int32 i = 0; i < Objects.Count(); i++)
    {
        if (Objects[i].LocalId != localId)
            continue;
        AuthoredSourceTombstone tombstone;
        tombstone.LocalId = Objects[i].LocalId;
        tombstone.StableKey = Objects[i].StableKey;
        tombstone.TypeName = Objects[i].TypeName;
        tombstone.Name = Objects[i].Name;
        Tombstones.Add(MoveTemp(tombstone));
        Objects.RemoveAt(i);
        return false;
    }
    error = TEXT("The authored object is not present in this source document.");
    return true;
}

bool AuthoredSourceDocument::SetMainObject(int64 localId, String& error)
{
    if (!FindObject(localId))
    {
        error = TEXT("The selected main object is not present in this source document.");
        return true;
    }
    MainObjectLocalId = localId;
    return false;
}

bool AuthoredSourceDocument::SetObjectData(int64 localId, const StringAnsiView& dataJson, String& error)
{
    AuthoredSourceObject* object = FindObject(localId);
    if (!object || !IsValidPayload(dataJson))
    {
        error = object ? TEXT("Authored object data is malformed JSON.") : TEXT("The authored object is not present in this source document.");
        return true;
    }
    object->DataJson = StringAnsi(dataJson);
    return false;
}
