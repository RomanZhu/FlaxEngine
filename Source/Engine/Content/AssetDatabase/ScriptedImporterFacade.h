// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingType.h"

/// <summary>Native publication boundary for managed scripted importers.</summary>
API_CLASS(Static) class FLAXENGINE_API ScriptedImporterFacade
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(ScriptedImporterFacade);

public:
    /// <summary>Creates or selects metadata for a managed importer without starting a native compatibility build.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool EnsureMetadata(const StringView& sourcePath, const StringView& importerId, int32 settingsSchemaVersion);

    /// <summary>Reconciles managed outputs and publishes their immutable runtime artifacts into Library.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool Publish(const StringView& sourcePath, const StringView& resultJson);

    /// <summary>Gets the last managed-import bridge error.</summary>
    API_FUNCTION() static String GetLastError();
};
