// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetIdentitySerialization.h"
#include "Engine/Serialization/JsonWriter.h"
#include "Engine/Serialization/Serialization.h"

namespace
{
    bool ReadGuidMember(ISerializable::DeserializeStream& stream, const char* name, AssetGuid& value)
    {
        const auto member = stream.FindMember(name);
        if (member == stream.MemberEnd())
            return true;
        Serialization::Deserialize(member->value, value, nullptr);
        return !value.IsValid();
    }

    bool ReadInt64Member(ISerializable::DeserializeStream& stream, const char* name, int64& value)
    {
        const auto member = stream.FindMember(name);
        if (member == stream.MemberEnd() || !member->value.IsInt64())
            return true;
        value = member->value.GetInt64();
        return false;
    }
}

bool Serialization::ShouldSerialize(const AssetGuid& value, const void* otherObj)
{
    return !otherObj || value != *static_cast<const AssetGuid*>(otherObj);
}

void Serialization::Serialize(ISerializable::SerializeStream& stream, const AssetGuid& value, const void* otherObj)
{
    stream.Guid(value.Value);
}

void Serialization::Deserialize(ISerializable::DeserializeStream& stream, AssetGuid& value, ISerializeModifier* modifier)
{
    Guid guid;
    Serialization::Deserialize(stream, guid, modifier);
    value = AssetGuid(guid);
}

bool Serialization::ShouldSerialize(const AssetObjectId& value, const void* otherObj)
{
    return !otherObj || value != *static_cast<const AssetObjectId*>(otherObj);
}

void Serialization::Serialize(ISerializable::SerializeStream& stream, const AssetObjectId& value, const void* otherObj)
{
    stream.StartObject();
    stream.JKEY("guid");
    stream.Guid(value.Asset.Value);
    stream.JKEY("fileId");
    stream.Int64(value.LocalId);
    stream.EndObject();
}

void Serialization::Deserialize(ISerializable::DeserializeStream& stream, AssetObjectId& value, ISerializeModifier* modifier)
{
    value = AssetObjectId();
    if (!stream.IsObject())
        return;
    AssetGuid source;
    int64 localId = 0;
    if (ReadGuidMember(stream, "guid", source) || ReadInt64Member(stream, "fileId", localId) || localId == 0)
        return;
    value = AssetObjectId(source, localId);
}

bool Serialization::ShouldSerialize(const GlobalAssetObjectId& value, const void* otherObj)
{
    return !otherObj || value != *static_cast<const GlobalAssetObjectId*>(otherObj);
}

void Serialization::Serialize(ISerializable::SerializeStream& stream, const GlobalAssetObjectId& value, const void* otherObj)
{
    stream.StartObject();
    stream.JKEY("kind");
    stream.Int(static_cast<int32>(value.Kind));
    stream.JKEY("guid");
    stream.Guid(value.SourceAsset.Value);
    stream.JKEY("fileId");
    stream.Int64(value.LocalFileId);
    stream.JKEY("prefabInstanceFileId");
    stream.Int64(value.PrefabInstanceFileId);
    stream.EndObject();
}

void Serialization::Deserialize(ISerializable::DeserializeStream& stream, GlobalAssetObjectId& value, ISerializeModifier* modifier)
{
    value = GlobalAssetObjectId();
    if (!stream.IsObject())
        return;
    const auto kind = stream.FindMember("kind");
    if (kind == stream.MemberEnd() || !kind->value.IsInt())
        return;
    AssetGuid source;
    int64 localId = 0;
    int64 prefabInstanceFileId = 0;
    if (ReadGuidMember(stream, "guid", source) || ReadInt64Member(stream, "fileId", localId) ||
        ReadInt64Member(stream, "prefabInstanceFileId", prefabInstanceFileId) || localId == 0)
        return;
    const int32 kindValue = kind->value.GetInt();
    if (kindValue < static_cast<int32>(GlobalObjectKind::ImportedAssetObject) ||
        kindValue > static_cast<int32>(GlobalObjectKind::BuiltinObject))
        return;
    value.Kind = static_cast<GlobalObjectKind>(kindValue);
    value.SourceAsset = source;
    value.LocalFileId = localId;
    value.PrefabInstanceFileId = prefabInstanceFileId;
}
