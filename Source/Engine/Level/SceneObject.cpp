// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneObject.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/Joints/Joint.h"
#include "Engine/Content/Content.h"
#include "Engine/Level/Prefabs/Prefab.h"
#include "Engine/Scripting/BinaryModule.h"
#include "Engine/Scripting/Internal/ManagedSerialization.h"
#include "Engine/Serialization/ISerializeModifier.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Platform/StringUtils.h"

void SceneBeginData::OnDone()
{
    for (int32 i = 0; i < JointsToCreate.Count(); i++)
    {
        JointsToCreate[i]->Create();
    }

    JointsToCreate.Clear();
}

SceneObject::SceneObject(const SpawnParams& params)
    : Base(params)
    , _parent(nullptr)
    , _prefabID(Guid::Empty)
    , _prefabObjectID(Guid::Empty)
#if USE_EDITOR
    , _externalSiblingOrderParentId(Guid::Empty)
    , _externalLegacyOrderInParent(0)
    , _hasExternalLegacyOrderInParent(false)
#endif
{
}

SceneObject::~SceneObject()
{
}

#if USE_EDITOR

namespace
{
    constexpr uint16 ExternalOrderMiddleDigit = 32768;
    constexpr uint16 ExternalOrderMinDigit = 0;
    constexpr uint16 ExternalOrderMaxDigit = MAX_uint16;

    uint16 GetExternalOrderDigit(const ExternalSiblingOrderKey& key, int32 index)
    {
        return index < key.Digits.Count() ? key.Digits[index] : ExternalOrderMiddleDigit;
    }
}

int32 ExternalSiblingOrderKey::Compare(const ExternalSiblingOrderKey& other) const
{
    const int32 count = Digits.Count() > other.Digits.Count() ? Digits.Count() : other.Digits.Count();
    for (int32 i = 0; i < count; i++)
    {
        const uint16 a = GetExternalOrderDigit(*this, i);
        const uint16 b = GetExternalOrderDigit(other, i);
        if (a < b)
            return -1;
        if (a > b)
            return 1;
    }
    return 0;
}

String ExternalSiblingOrderKey::ToString() const
{
    static const Char HexDigits[] = TEXT("0123456789abcdef");
    String result;
    result.Resize(Digits.Count() * 4);
    for (int32 i = 0; i < Digits.Count(); i++)
    {
        const uint16 digit = Digits[i];
        result[i * 4 + 0] = HexDigits[(digit >> 12) & 15];
        result[i * 4 + 1] = HexDigits[(digit >> 8) & 15];
        result[i * 4 + 2] = HexDigits[(digit >> 4) & 15];
        result[i * 4 + 3] = HexDigits[digit & 15];
    }
    return result;
}

bool ExternalSiblingOrderKey::TryParse(const StringView& text, ExternalSiblingOrderKey& result)
{
    result.Digits.Clear();
    if (text.IsEmpty() || text.Length() % 4 != 0 || text.Length() > 4096)
        return false;

    result.Digits.Resize(text.Length() / 4);
    for (int32 i = 0; i < result.Digits.Count(); i++)
    {
        uint16 digit = 0;
        for (int32 j = 0; j < 4; j++)
        {
            const int32 value = StringUtils::HexDigit(text[i * 4 + j]);
            if (value < 0)
            {
                result.Digits.Clear();
                return false;
            }
            digit = static_cast<uint16>((digit << 4) | value);
        }
        result.Digits[i] = digit;
    }
    return true;
}

ExternalSiblingOrderKey ExternalSiblingOrderKey::FromLegacy(int64 value)
{
    ExternalSiblingOrderKey result;
    const uint64 sortableValue = static_cast<uint64>(value) ^ (static_cast<uint64>(1) << 63);
    result.Digits.Resize(4);
    result.Digits[0] = static_cast<uint16>(sortableValue >> 48);
    result.Digits[1] = static_cast<uint16>(sortableValue >> 32);
    result.Digits[2] = static_cast<uint16>(sortableValue >> 16);
    result.Digits[3] = static_cast<uint16>(sortableValue);
    return result;
}

ExternalSiblingOrderKey ExternalSiblingOrderKey::CreateBetween(const ExternalSiblingOrderKey* previous, const ExternalSiblingOrderKey* next, const Guid& objectId)
{
    ASSERT(!previous || !next || previous->Compare(*next) < 0);

    ExternalSiblingOrderKey result;
    bool previousTight = previous != nullptr;
    bool nextTight = next != nullptr;
    uint32 random = objectId.Values[0] ^ (objectId.Values[1] * 0x9e3779b9u) ^ (objectId.Values[2] * 0x85ebca6bu) ^ (objectId.Values[3] * 0xc2b2ae35u);
    for (int32 depth = 0;; depth++)
    {
        const uint16 previousDigit = previousTight ? GetExternalOrderDigit(*previous, depth) : ExternalOrderMinDigit;
        const uint16 nextDigit = nextTight ? GetExternalOrderDigit(*next, depth) : ExternalOrderMaxDigit;
        ASSERT(previousDigit <= nextDigit);

        const uint32 gap = static_cast<uint32>(nextDigit) - previousDigit;
        if (gap > 1)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            result.Digits.Add(static_cast<uint16>(previousDigit + 1 + random % (gap - 1)));
            return result;
        }

        result.Digits.Add(previousDigit);
        if (previousDigit < nextDigit)
            nextTight = false;
    }
}

const ExternalSiblingOrderKey& SceneObject::GetExternalSiblingOrderKey() const
{
    return _externalSiblingOrderKey;
}

bool SceneObject::HasExternalSiblingOrderKeyForCurrentParent() const
{
    return _externalSiblingOrderKey.IsValid() && _parent && _externalSiblingOrderParentId == _parent->GetID();
}

bool SceneObject::HasExternalLegacyOrderInParent() const
{
    return _hasExternalLegacyOrderInParent;
}

int64 SceneObject::GetExternalOrderInParent() const
{
    return _externalLegacyOrderInParent;
}

void SceneObject::SetExternalOrderInParent(int64 value)
{
    _externalSiblingOrderKey = ExternalSiblingOrderKey::FromLegacy(value);
    _externalSiblingOrderParentId = _parent ? _parent->GetID() : Guid::Empty;
    _externalLegacyOrderInParent = value;
    _hasExternalLegacyOrderInParent = true;
}

void SceneObject::SetExternalSiblingOrderKey(const ExternalSiblingOrderKey& value)
{
    ASSERT(value.IsValid());
    _externalSiblingOrderKey = value;
    _externalSiblingOrderParentId = _parent ? _parent->GetID() : Guid::Empty;
    _externalLegacyOrderInParent = 0;
    _hasExternalLegacyOrderInParent = false;
}

#endif

void SceneObject::LinkPrefab(const Guid& prefabId, const Guid& prefabObjectId)
{
    ASSERT(prefabId.IsValid());

    // Link
    _prefabID = prefabId;
    _prefabObjectID = prefabObjectId;

    if (_prefabID.IsValid() && _prefabObjectID.IsValid())
    {
        auto prefab = Content::LoadAsync<Prefab>(_prefabID);
        if (prefab == nullptr || prefab->WaitForLoaded())
        {
            _prefabID = Guid::Empty;
            _prefabObjectID = Guid::Empty;
            LOG(Warning, "Failed to load prefab linked to the actor.");
        }
    }
}

void SceneObject::BreakPrefabLink()
{
    // Invalidate link
    _prefabID = Guid::Empty;
    _prefabObjectID = Guid::Empty;
}

String SceneObject::GetNamePath(Char separatorChar) const
{
    Array<StringView, InlinedAllocation<8>> names;
    const Actor* a = dynamic_cast<const Actor*>(this);
    if (!a)
        a = GetParent();
    while (a)
    {
        names.Add(a->GetName());
        a = a->GetParent();
    }
    if (names.IsEmpty())
        return String::Empty;
    int32 length = names.Count() - 1;
    for (int32 i = 0; i < names.Count(); i++)
        length += names[i].Length();
    if (length == 0)
        return String::Empty;
    String result;
    result.ReserveSpace(length);
    Char* ptr = result.Get();
    for (int32 i = names.Count() - 1; i >= 0; i--)
    {
        const String& name = names[i];
        Platform::MemoryCopy(ptr, name.Get(), name.Length() * sizeof(Char));
        ptr += name.Length();
        if (i != 0)
            *ptr++ = separatorChar;
    }
    *ptr = 0;
    return result.ToString();
}

void SceneObject::Serialize(SerializeStream& stream, const void* otherObj)
{
    SERIALIZE_GET_OTHER_OBJ(SceneObject);

    stream.JKEY("ID");
    stream.Guid(_id);

    if (other && HasPrefabLink())
    {
        stream.JKEY("PrefabID");
        stream.Guid(_prefabID);

        stream.JKEY("PrefabObjectID");
        stream.Guid(_prefabObjectID);
    }
    else
    {
        stream.JKEY("TypeName");
        stream.String(GetType().Fullname);
    }

    if (_parent)
    {
        stream.JKEY("ParentID");
        stream.Guid(_parent->GetID());
    }

#if !COMPILE_WITHOUT_CSHARP
    // Handle C# objects data serialization
    if (EnumHasAnyFlags(Flags, ObjectFlags::IsManagedType))
    {
        stream.JKEY("V");
        if (other)
        {
            ManagedSerialization::SerializeDiff(stream, GetOrCreateManagedInstance(), other->GetOrCreateManagedInstance());
        }
        else
        {
            ManagedSerialization::Serialize(stream, GetOrCreateManagedInstance());
        }
    }
#endif

    // Handle custom scripting objects data serialization
    if (EnumHasAnyFlags(Flags, ObjectFlags::IsCustomScriptingType))
    {
        stream.JKEY("D");
        _type.Module->SerializeObject(stream, this, other);
    }
}

void SceneObject::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    // _id is deserialized by Actor/Script impl
    // _parent is deserialized by Actor/Script impl
    // _prefabID is deserialized by Actor/Script impl
    DESERIALIZE_MEMBER(PrefabObjectID, _prefabObjectID);

#if USE_EDITOR
    bool hasExternalOrder = false;
    const auto siblingOrderKeyMember = SERIALIZE_FIND_MEMBER(stream, "SiblingOrderKey");
    if (siblingOrderKeyMember != stream.MemberEnd() && siblingOrderKeyMember->value.IsString())
    {
        ExternalSiblingOrderKey key;
        if (ExternalSiblingOrderKey::TryParse(siblingOrderKeyMember->value.GetText(), key))
        {
            _externalSiblingOrderKey = MoveTemp(key);
            _externalLegacyOrderInParent = 0;
            _hasExternalLegacyOrderInParent = false;
            hasExternalOrder = true;
        }
    }
    if (!hasExternalOrder)
    {
        const auto legacyOrderMember = SERIALIZE_FIND_MEMBER(stream, "OrderInParent");
        if (legacyOrderMember != stream.MemberEnd() && legacyOrderMember->value.IsInt64())
        {
            _externalLegacyOrderInParent = legacyOrderMember->value.GetInt64();
            _externalSiblingOrderKey = ExternalSiblingOrderKey::FromLegacy(_externalLegacyOrderInParent);
            _hasExternalLegacyOrderInParent = true;
            hasExternalOrder = true;
        }
    }
    if (hasExternalOrder)
    {
        _externalSiblingOrderParentId = Guid::Empty;
        const auto parentMember = SERIALIZE_FIND_MEMBER(stream, "ParentID");
        if (parentMember != stream.MemberEnd())
            Serialization::Deserialize(parentMember->value, _externalSiblingOrderParentId, modifier);
    }
#endif

#if !COMPILE_WITHOUT_CSHARP
    // Handle C# objects data serialization
    if (EnumHasAnyFlags(Flags, ObjectFlags::IsManagedType))
    {
        auto* const v = SERIALIZE_FIND_MEMBER(stream, "V");
        if (v != stream.MemberEnd() && v->value.IsObject() && v->value.MemberCount() != 0)
        {
            ManagedSerialization::Deserialize(v->value, GetOrCreateManagedInstance());
        }
    }
#endif

    // Handle custom scripting objects data serialization
    if (EnumHasAnyFlags(Flags, ObjectFlags::IsCustomScriptingType))
    {
        auto* const v = SERIALIZE_FIND_MEMBER(stream, "D");
        if (v != stream.MemberEnd() && v->value.IsObject() && v->value.MemberCount() != 0)
        {
            _type.Module->DeserializeObject(v->value, this, modifier);
        }
    }
}
