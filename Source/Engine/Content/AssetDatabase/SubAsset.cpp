// Copyright (c) Wojciech Figat. All rights reserved.

#include "SubAsset.h"

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

namespace
{
    constexpr uint64 FnvOffset = 14695981039346656037ull;
    constexpr uint64 FnvPrime = 1099511628211ull;

    void HashByte(uint64& hash, byte value)
    {
        hash ^= value;
        hash *= FnvPrime;
    }

    void HashString(uint64& hash, const StringView& value)
    {
        const uint32 length = static_cast<uint32>(value.Length());
        for (int32 i = 0; i < 4; i++)
            HashByte(hash, static_cast<byte>(length >> (i * 8)));
        for (int32 i = 0; i < value.Length(); i++)
        {
            const uint16 codeUnit = static_cast<uint16>(value[i]);
            HashByte(hash, static_cast<byte>(codeUnit));
            HashByte(hash, static_cast<byte>(codeUnit >> 8));
        }
    }
}

int64 SubAssetPolicy::CalculateLocalId(const StringView& importerNamespace, const StringView& stableIdentifier, const StringView& objectCategory, uint32 collisionSalt)
{
    uint64 hash = FnvOffset;
    HashString(hash, importerNamespace);
    HashString(hash, stableIdentifier);
    HashString(hash, objectCategory);
    for (int32 i = 0; i < 4; i++)
        HashByte(hash, static_cast<byte>(collisionSalt >> (i * 8)));
    int64 result = static_cast<int64>(hash & MAX_int64);
    if (result <= 1)
        result = 2;
    return result;
}

int64 SubAssetPolicy::AllocateLocalId(const StringView& importerNamespace, const StringView& stableIdentifier, const StringView& objectCategory, const HashSet<int64>& reserved, uint32& collisionSalt)
{
    for (;; collisionSalt++)
    {
        const int64 candidate = CalculateLocalId(importerNamespace, stableIdentifier, objectCategory, collisionSalt);
        if (!reserved.Contains(candidate))
            return candidate;
    }
}
