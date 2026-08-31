// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"

/// <summary>Managed bridge for generic authored source creation, structural edits, and dirty saves.</summary>
API_CLASS(Static) class FLAXENGINE_API AuthoredAssetFacade
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AuthoredAssetFacade);

public:
    /// <summary>Creates a canonical generic authored source. Returns an empty GUID on failure.</summary>
    API_FUNCTION() static Guid CreateAsset(const StringView& path, const Guid& objectInstanceID, const StringView& typeName,
        const StringView& name, const StringView& dataJson);

    /// <summary>Adds an authored object and returns its stable local ID, or zero on failure.</summary>
    API_FUNCTION() static int64 AddObjectToAsset(const StringView& path, const Guid& objectInstanceID, const StringView& typeName,
        const StringView& name, const StringView& dataJson);

    /// <summary>Removes an authored object and tombstones its local ID. Returns true on failure.</summary>
    API_FUNCTION() static bool RemoveObjectFromAsset(const StringView& path, int64 localId);

    /// <summary>Persists the authored document's main-object selection. Returns true on failure.</summary>
    API_FUNCTION() static bool SetMainObject(const StringView& path, int64 localId);

    /// <summary>Stages serialized data for a source object without touching disk. Returns true on failure.</summary>
    API_FUNCTION() static bool StageObjectData(const StringView& path, int64 localId, const StringView& dataJson, const StringView& reason);

    API_FUNCTION() static bool IsDirty(const StringView& path);

    /// <summary>Saves one dirty authored source. Returns true on failure.</summary>
    API_FUNCTION() static bool SaveAssetIfDirty(const StringView& path);

    /// <summary>Returns callback-ready dirty physical paths separated by newlines.</summary>
    API_FUNCTION() static String GetDirtyPaths();

    /// <summary>Canonicalizes one generic authored source, optionally rebuilding its metadata object table.</summary>
    API_FUNCTION() static bool ForceReserialize(const StringView& path, bool includeMetadata);

    API_FUNCTION() static String GetLastError();
};
