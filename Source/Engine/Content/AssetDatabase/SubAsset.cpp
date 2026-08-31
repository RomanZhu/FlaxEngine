// Copyright (c) Wojciech Figat. All rights reserved.

#include "SubAsset.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"

namespace
{
    void HashUInt32(ContentHasher& hasher, uint32 value)
    {
        byte bytes[4];
        for (int32 i = 0; i < 4; i++)
            bytes[3 - i] = static_cast<byte>(value >> (i * 8));
        hasher.Update(bytes, ARRAY_COUNT(bytes));
    }

    void HashUInt64(ContentHasher& hasher, uint64 value)
    {
        byte bytes[8];
        for (int32 i = 0; i < 8; i++)
            bytes[7 - i] = static_cast<byte>(value >> (i * 8));
        hasher.Update(bytes, ARRAY_COUNT(bytes));
    }

    void HashString(ContentHasher& hasher, const StringView& value)
    {
        const StringAnsi utf8(value);
        HashUInt32(hasher, static_cast<uint32>(utf8.Length()));
        hasher.Update(utf8.Get(), utf8.Length());
    }

    uint32 ReadUInt32(const byte* bytes)
    {
        return (static_cast<uint32>(bytes[0]) << 24) | (static_cast<uint32>(bytes[1]) << 16) |
               (static_cast<uint32>(bytes[2]) << 8) | static_cast<uint32>(bytes[3]);
    }
}

String SubAssetPolicy::NormalizeKey(const StringView& key)
{
    String result(key);
    result.Replace((Char)92, '/');
    while (result.Contains(TEXT("//")))
        result.Replace(TEXT("//"), TEXT("/"));
    return result;
}

bool SubAssetPolicy::IsKeyValid(const StringView& key)
{
    if (key.IsEmpty() || !key.Contains(TEXT(":")) || key.StartsWith('/') || key.EndsWith('/') || key.Contains(TEXT("../")) || key.Contains(TEXT("/..")))
        return false;
    for (int32 i = 0; i < key.Length(); i++)
    {
        if (key[i] < 0x20)
            return false;
    }
    return NormalizeKey(key) == key;
}

int64 SubAssetPolicy::AllocateLocalId(const StringView& importerId, const StringView& stableKey, const StringView& typeName, HashSet<int64>& reserved)
{
    for (uint32 probe = 0;; probe++)
    {
        ContentHasher hasher;
        static const char Domain[] = "flax-local-file-id-v1";
        hasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
        HashString(hasher, importerId);
        HashString(hasher, stableKey);
        HashString(hasher, typeName);
        HashUInt32(hasher, probe);
        const ContentHash hash = hasher.Finalize();
        uint64 value = 0;
        for (int32 i = 0; i < 8; i++)
            value = (value << 8) | hash.Bytes[i];
        value &= MAX_int64;
        if (value > 1 && reserved.Add(static_cast<int64>(value)))
            return static_cast<int64>(value);
    }
}

int64 SubAssetPolicy::LegacyLocalIdFromGuid(const Guid& id)
{
    uint64 value = (static_cast<uint64>(id.A) << 32) | id.B;
    value &= MAX_int64;
    if (value <= 1)
    {
        value = ((static_cast<uint64>(id.C) << 32) | id.D) & MAX_int64;
        if (value <= 1)
            value = 2;
    }
    return static_cast<int64>(value);
}

Guid SubAssetPolicy::GetBackingAssetId(const Guid& fileGuid, int64 localId)
{
    if (!fileGuid.IsValid() || localId == 0)
        return Guid::Empty;
    if (localId == 1)
        return fileGuid;
    ContentHasher hasher;
    static const char Domain[] = "flax-object-backing-guid-v1";
    hasher.Update(Domain, ARRAY_COUNT(Domain) - 1);
    for (int32 i = 0; i < 4; i++)
        HashUInt32(hasher, fileGuid.Values[i]);
    HashUInt64(hasher, static_cast<uint64>(localId));
    const ContentHash hash = hasher.Finalize();
    Guid result(ReadUInt32(hash.Bytes), ReadUInt32(hash.Bytes + 4), ReadUInt32(hash.Bytes + 8), ReadUInt32(hash.Bytes + 12));
    if (!result.IsValid() || result == fileGuid)
        result.D ^= 1;
    return result;
}
