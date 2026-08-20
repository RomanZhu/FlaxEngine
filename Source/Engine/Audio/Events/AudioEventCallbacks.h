// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioEventHandle.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// Musical timeline beat information emitted by an event backend.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioTimelineBeat
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioTimelineBeat);

    API_FIELD() int32 PositionMs = 0;
    API_FIELD() int32 Bar = 0;
    API_FIELD() int32 Beat = 0;
    API_FIELD() float Tempo = 0.0f;
    API_FIELD() int32 TimeSignatureUpper = 0;
    API_FIELD() int32 TimeSignatureLower = 0;
};

/// <summary>
/// A lifecycle notification emitted by an audio event backend.
/// </summary>
API_ENUM() enum class AudioEventCallbackType : uint8
{
    Starting,
    Started,
    Stopped,
    Restarted,
    TimelineMarker,
    TimelineBeat,
    RealToVirtual,
    VirtualToReal,
    StartFailed,
};

/// <summary>
/// Data associated with an audio event lifecycle notification. Callbacks are raised on the main thread.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioEventCallback
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioEventCallback);

    API_FIELD() AudioEventHandle Handle;
    API_FIELD() AudioEventCallbackType Type = AudioEventCallbackType::Started;
    API_FIELD() String Marker;
    API_FIELD() int32 TimelinePositionMs = 0;
    API_FIELD() int32 Bar = 0;
    API_FIELD() int32 Beat = 0;
    API_FIELD() float Tempo = 0.0f;
    API_FIELD() int32 TimeSignatureUpper = 0;
    API_FIELD() int32 TimeSignatureLower = 0;
};
