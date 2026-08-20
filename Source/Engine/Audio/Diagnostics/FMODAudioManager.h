// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"
#include "Engine/Audio/Events/AudioEventTypes.h"

/// <summary>
/// Scene-level observer for FMOD runtime diagnostics. This actor never initializes,
/// owns, or disposes the audio backend.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/FMOD Audio Manager\")")
class FLAXENGINE_API FMODAudioManager : public Actor
{
    DECLARE_SCENE_OBJECT(FMODAudioManager);

private:
    AudioDiagnosticsSnapshot _snapshot;

public:
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Debug\")")
    bool ShowSceneOverlay = true;
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Debug\")")
    bool ShowGameOverlay = false;
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Debug\")")
    bool ShowEventLabels = true;
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Debug\")")
    bool ShowOcclusionRays = false;
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Debug\")")
    float MaxLabelDistance = 5000.0f;
    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Debug\")")
    int32 MaxLabels = 128;

    /// <summary>
    /// Most recently captured backend telemetry. This is read-only observer state.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(100), EditorDisplay(\"FMOD Runtime\")")
    const AudioDiagnosticsSnapshot& GetSnapshot() const { return _snapshot; }

    /// <summary>
    /// Captures current backend telemetry immediately.
    /// </summary>
    API_FUNCTION() void CaptureDiagnostics();

    /// <summary>
    /// Stops all event instances through the generic audio API.
    /// </summary>
    API_FUNCTION() void StopAllEvents();

protected:
    void OnEnable() override;
    void OnDisable() override;

private:
    void Update();
};
