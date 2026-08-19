// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"

/// <summary>
/// Represents a single audio device.
/// </summary>
API_CLASS(NoSpawn) class AudioDevice : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AudioDevice);

    explicit AudioDevice()
        : ScriptingObject(SpawnParams(Guid::New(), TypeInitializer))
    {
    }

    AudioDevice(const AudioDevice& other)
        : ScriptingObject(SpawnParams(Guid::New(), TypeInitializer))
    {
        Name = other.Name;
        InternalName = other.InternalName;
        BackendName = other.BackendName;
        BackendIndex = other.BackendIndex;
    }

    AudioDevice& operator=(const AudioDevice& other)
    {
        Name = other.Name;
        InternalName = other.InternalName;
        BackendName = other.BackendName;
        BackendIndex = other.BackendIndex;
        return *this;
    }

public:
    /// <summary>
    /// The device name.
    /// </summary>
    API_FIELD(ReadOnly) String Name;

    /// <summary>
    /// The internal device name used by the audio backend.
    /// </summary>
    StringAnsi InternalName;

    /// <summary>
    /// The name of the backend that created this device entry (e.g. OpenAL, FMOD Studio).
    /// </summary>
    API_FIELD(ReadOnly) StringAnsi BackendName;

    /// <summary>
    /// The internal index of the device within its backend.
    /// </summary>
    API_FIELD(ReadOnly) int32 BackendIndex = -1;

    String ToString() const override
    {
        return Name;
    }
};
