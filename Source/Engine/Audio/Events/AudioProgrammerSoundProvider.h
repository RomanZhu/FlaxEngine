// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Scripting/ScriptingObject.h"

/// <summary>Backend-neutral media supplied to an authored programmer-sound instrument.</summary>
API_STRUCT() struct FLAXENGINE_API AudioProgrammerSoundData
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioProgrammerSoundData);

    /// <summary>Absolute or engine-resolvable media path.</summary>
    API_FIELD() String Path;

    /// <summary>Optional subsound index for container formats. Use -1 for the default sound.</summary>
    API_FIELD() int32 SubsoundIndex = -1;
};

/// <summary>
/// Resolves gameplay keys without exposing middleware types. Resolution happens on the engine thread;
/// the audio callback consumes only a fixed copy of the result.
/// </summary>
API_CLASS(Abstract) class FLAXENGINE_API AudioProgrammerSoundProvider : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE(AudioProgrammerSoundProvider);

public:
    /// <summary>Resolves a programmer-sound key into playable media.</summary>
    API_FUNCTION() virtual bool Resolve(const StringView& key, API_PARAM(Out) AudioProgrammerSoundData& outData)
    {
        return false;
    }
};
