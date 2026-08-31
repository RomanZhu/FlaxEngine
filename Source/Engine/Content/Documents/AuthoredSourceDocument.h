// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Types/String.h"

/// <summary>One live object serialized directly into a generic authored source document.</summary>
struct FLAXENGINE_API AuthoredSourceObject
{
    int64 LocalId = 0;
    String StableKey;
    String TypeName;
    int32 SchemaVersion = 1;
    String Name;
    StringAnsi DataJson = "{}\n";
    Dictionary<StringAnsi, StringAnsi> UnknownFields;
};

/// <summary>Persistent identity reservation for an object removed from an authored source.</summary>
struct FLAXENGINE_API AuthoredSourceTombstone
{
    int64 LocalId = 0;
    String StableKey;
    String TypeName;
    String Name;
    Dictionary<StringAnsi, StringAnsi> UnknownFields;
};

/// <summary>Canonical source-owned multi-object envelope used by generic authored assets.</summary>
class FLAXENGINE_API AuthoredSourceDocument
{
public:
    static constexpr int32 CurrentVersion = 2;

    int32 Version = CurrentVersion;
    String DocumentType;
    Array<AuthoredSourceObject> Objects;
    int64 MainObjectLocalId = 0;
    Array<AuthoredSourceTombstone> Tombstones;
    Dictionary<StringAnsi, StringAnsi> UnknownFields;

    /// <summary>Parses and validates a canonical authored source envelope.</summary>
    static bool Parse(const StringAnsiView& json, AuthoredSourceDocument& result, String& error);

    /// <summary>Writes deterministic current-version JSON while preserving extension fields.</summary>
    bool ToCanonicalJson(StringAnsi& json, String& error) const;

    AuthoredSourceObject* FindObject(int64 localId);
    const AuthoredSourceObject* FindObject(int64 localId) const;

    /// <summary>Allocates a stable positive local ID against all live IDs and tombstones.</summary>
    bool AddObject(const StringView& stableKey, const StringView& typeName, const StringView& name,
        const StringAnsiView& dataJson, int64& localId, String& error);

    /// <summary>Removes a non-main object and permanently tombstones its ID.</summary>
    bool RemoveObject(int64 localId, String& error);

    /// <summary>Changes the persisted main-object selection without changing object identity.</summary>
    bool SetMainObject(int64 localId, String& error);

    /// <summary>Replaces one object's serialized payload without changing identity.</summary>
    bool SetObjectData(int64 localId, const StringAnsiView& dataJson, String& error);
};
