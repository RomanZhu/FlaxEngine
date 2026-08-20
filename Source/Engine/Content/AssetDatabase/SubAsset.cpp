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

void SubAssetPolicy::RegenerateGuids(Dictionary<String, SubAssetMeta>& mappings)
{
    for (auto& entry : mappings)
        entry.Value.ID = Guid::New();
}
