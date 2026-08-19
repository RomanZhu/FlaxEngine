// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config.h"
#include "Engine/Content/Config.h"

// The maximum amount of listeners used at once
#define AUDIO_MAX_LISTENERS 1

// The maximum amount of audio emitter buffers
#define AUDIO_MAX_SOURCE_BUFFERS (ASSET_FILE_DATA_CHUNKS)

// Enables/disables Audio Event subsystem support
#ifndef COMPILE_WITH_AUDIO_EVENTS
#define COMPILE_WITH_AUDIO_EVENTS 1
#endif

// Enables/disables Null Audio Event backend
#ifndef AUDIO_EVENT_API_NONE
#define AUDIO_EVENT_API_NONE 0
#endif

// Enables/disables FMOD Studio Audio Event backend
#ifndef AUDIO_EVENT_API_FMOD
#define AUDIO_EVENT_API_FMOD 0
#endif
