// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingType.h"

/// <summary>Scene-side diagnostics that complement backend event/bank telemetry.</summary>
API_STRUCT() struct FLAXENGINE_API AudioSceneDiagnostics
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioSceneDiagnostics);

    API_FIELD() int32 Emitters = 0;
    API_FIELD() int32 Volumes = 0;
    API_FIELD() int32 ActiveZones = 0;
    API_FIELD() int32 OcclusionQueries = 0;
    API_FIELD() int32 OcclusionDeferred = 0;
    API_FIELD() int32 PersistentInteractions = 0;
};
