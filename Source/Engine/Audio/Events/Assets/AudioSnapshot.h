// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"

/// <summary>
/// Serializable audio mixer snapshot asset.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Audio/Audio Snapshot\")")
class FLAXENGINE_API AudioSnapshot final : public ScriptingObject, public ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(AudioSnapshot, ScriptingObject);

public:
    /// <summary>
    /// Unique backend GUID of the snapshot.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Snapshot\")")
    Guid BackendId = Guid::Empty;

    /// <summary>
    /// Path of the snapshot in the middleware hierarchy (e.g. snapshot:/UndergroundCave).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Snapshot\")")
    String Path;
};
