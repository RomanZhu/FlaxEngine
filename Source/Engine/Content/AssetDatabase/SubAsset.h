// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

/// <summary>Durable sidecar mapping for one processor-owned subasset.</summary>
struct FLAXENGINE_API SubAssetMeta
{
    Guid ID;
    int64 LocalId = 0;
    String TypeName;
    String DisplayName;
    bool Removed = false;
    Array<String> PreviousKeys;
    Dictionary<StringAnsi, StringAnsi> UnknownFields;
};

/// <summary>Prepared processor output identity candidate.</summary>
struct FLAXENGINE_API SubAssetCandidate
{
    String StableKey;
    String TypeName;
    String DisplayName;
    Array<String> PreviousKeys;
    bool RenameEvidenceReliable = false;
};

/// <summary>Stable subasset key rules and cloning helpers.</summary>
class FLAXENGINE_API SubAssetPolicy
{
public:
    static String NormalizeKey(const StringView& key);
    static bool IsKeyValid(const StringView& key);
    static int64 LocalIdFromGuid(const Guid& id);
    static void RegenerateGuids(Dictionary<String, SubAssetMeta>& mappings);
};
