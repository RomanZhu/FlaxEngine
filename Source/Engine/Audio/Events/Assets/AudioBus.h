// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"

/// <summary>
/// Serializable audio mixer bus asset.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Audio/Audio Bus\")")
class FLAXENGINE_API AudioBus final : public ScriptingObject, public ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(AudioBus, ScriptingObject);

public:
    /// <summary>
    /// Unique backend GUID of the mixer bus.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Bus\")")
    Guid BackendId = Guid::Empty;

    /// <summary>
    /// Path of the mixer bus in the middleware hierarchy (e.g. bus:/SFX/Weapons).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Bus\")")
    String Path;
};
