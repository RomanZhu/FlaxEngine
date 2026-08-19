// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingType.h"

class AudioEmitter;
class AudioVolumeBase;

/// <summary>
/// Central registry and evaluation manager for scene audio emitters, zones, and volumes.
/// </summary>
API_CLASS(Static) class FLAXENGINE_API AudioWorld
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AudioWorld);

public:
    /// <summary>
    /// All active registered audio emitters.
    /// </summary>
    static Array<AudioEmitter*> Emitters;

    /// <summary>
    /// All active registered audio volumes (zones, area emitters).
    /// </summary>
    static Array<AudioVolumeBase*> Volumes;

public:
    /// <summary>
    /// Registers an audio emitter with the world manager.
    /// </summary>
    static void Register(AudioEmitter* emitter);

    /// <summary>
    /// Unregisters an audio emitter from the world manager.
    /// </summary>
    static void Unregister(AudioEmitter* emitter);

    /// <summary>
    /// Registers an audio volume with the world manager.
    /// </summary>
    static void Register(AudioVolumeBase* volume);

    /// <summary>
    /// Unregisters an audio volume from the world manager.
    /// </summary>
    static void Unregister(AudioVolumeBase* volume);

    /// <summary>
    /// Updates all registered emitters and volumes. Volume evaluation uses the first active
    /// listener during play as the source of the listener position for this milestone.
    /// </summary>
    /// <param name="dt">Delta time in seconds.</param>
    static void Update(float dt);
};
