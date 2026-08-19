// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"

/// <summary>
/// Serializable audio VCA (Voltage-Controlled Amplifier) asset.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Audio/Audio VCA\")")
class FLAXENGINE_API AudioVCA final : public ScriptingObject, public ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(AudioVCA, ScriptingObject);

public:
    /// <summary>
    /// Unique backend GUID of the VCA.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"VCA\")")
    Guid BackendId = Guid::Empty;

    /// <summary>
    /// Path of the VCA in the middleware hierarchy (e.g. vca:/Dialogue).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"VCA\")")
    String Path;
};
