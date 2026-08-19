// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Core/Collections/Array.h"

/// <summary>
/// Serializable audio bank asset referencing a compiled middleware sound bank.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Audio/Audio Bank\")")
class FLAXENGINE_API AudioBank final : public ScriptingObject, public ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(AudioBank, ScriptingObject);

public:
    /// <summary>
    /// Unique backend GUID of the sound bank.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Bank\")")
    Guid BackendId = Guid::Empty;

    /// <summary>
    /// Path or name of the sound bank file.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Bank\")")
    String Path;

    /// <summary>
    /// If true, the bank sample data is loaded asynchronously.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Bank\")")
    bool NonBlocking = false;

    /// <summary>
    /// List of event IDs included in this bank.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Content\")")
    Array<Guid> IncludedEvents;

    /// <summary>
    /// Dependent banks required by this bank (e.g. Master.strings.bank).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Content\")")
    Array<Guid> ReferencedBanks;
};
