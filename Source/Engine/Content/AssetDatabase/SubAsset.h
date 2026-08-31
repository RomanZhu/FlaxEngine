// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

/// <summary>Durable sidecar mapping for one processor-owned object under its source file GUID.</summary>
struct FLAXENGINE_API SubAssetMeta
{
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

/// <summary>Stable imported-object key and identity allocation rules.</summary>
class FLAXENGINE_API SubAssetPolicy
{
public:
    static String NormalizeKey(const StringView& key);
    static bool IsKeyValid(const StringView& key);

    /// <summary>Allocates a deterministic positive local file ID and reserves it against live IDs and tombstones.</summary>
    static int64 AllocateLocalId(const StringView& importerId, const StringView& stableKey, const StringView& typeName, HashSet<int64>& reserved);

    /// <summary>Converts a legacy per-object GUID into a migration-only local file ID candidate.</summary>
    static int64 LegacyLocalIdFromGuid(const Guid& id);

    /// <summary>Returns the deterministic engine backing GUID for an object ID. This is never persistent project identity.</summary>
    static Guid GetBackingAssetId(const Guid& fileGuid, int64 localId);
};
