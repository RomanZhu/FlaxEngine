// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include "Engine/Content/JsonAssetReference.h"
#include "AudioBank.h"

/// <summary>
/// Serializable audio event asset containing metadata, paths, and parameter definitions for an audio middleware event.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Audio/Audio Event\")")
class FLAXENGINE_API AudioEvent final : public ScriptingObject, public ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(AudioEvent, ScriptingObject);

public:
    /// <summary>
    /// Unique backend GUID of the audio event.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Event\")")
    Guid BackendId = Guid::Empty;

    /// <summary>
    /// Path of the event in the audio middleware project (e.g. event:/Weapons/Pistol_Shot).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Event\")")
    String Path;

    /// <summary>
    /// True if the event has 3D spatial positioning.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Event\")")
    bool Is3D = true;

    /// <summary>
    /// True if the event is a one-shot without infinite loops or sustain points.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Event\")")
    bool IsOneShot = false;

    /// <summary>
    /// Minimum distance for 3D attenuation.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"3D Attributes\")")
    float MinDistance = 1.0f;

    /// <summary>
    /// Maximum distance for 3D attenuation.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"3D Attributes\")")
    float MaxDistance = 100.0f;

    /// <summary>
    /// Total duration of the event in seconds (0 for infinite/looping).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Event\")")
    float Length = 0.0f;

    /// <summary>
    /// Exposed parameters for this audio event.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(70), EditorDisplay(\"Parameters\")")
    Array<AudioParameterId> Parameters;

    /// <summary>Bank GUIDs that must be loaded before an instance is created.</summary>
    API_FIELD(Attributes="EditorOrder(80), EditorDisplay(\"Content\")")
    Array<Guid> BankDependencies;

    /// <summary>Typed bank assets used to register and load dependencies without relying on startup-bank side effects.</summary>
    API_FIELD(Attributes="EditorOrder(90), EditorDisplay(\"Content\")")
    Array<JsonAssetReference<AudioBank>> BankAssets;
};
