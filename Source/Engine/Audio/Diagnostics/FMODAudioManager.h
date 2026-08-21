// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include "AudioRuntimeDiagnostics.h"
#include "Engine/Core/Math/Color.h"

/// <summary>
/// Scene-level observer for FMOD runtime diagnostics. This actor never initializes,
/// owns, or disposes the audio backend.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/FMOD Audio Manager\")")
class FLAXENGINE_API FMODAudioManager : public Actor
{
    DECLARE_SCENE_OBJECT(FMODAudioManager);

private:
    AudioSceneDiagnostics _sceneDiagnostics;

public:
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Debug\")")
    bool ShowSceneOverlay = true;
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Debug\")")
    bool ShowGameOverlay = false;
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Debug\")")
    bool ShowEventLabels = true;
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Debug\")")
    bool ShowOcclusionRays = false;
    API_FIELD(Attributes="EditorOrder(35), EditorDisplay(\"Debug\")")
    bool ShowInactiveEmitters = false;
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Debug\")")
    float MaxLabelDistance = 5000.0f;
    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Debug\")")
    int32 MaxLabels = 128;
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Debug\")")
    bool HideDistance = false;
    API_FIELD(Attributes="EditorOrder(61), EditorDisplay(\"Debug\")")
    int32 LabelFontSize = 18;
    API_FIELD(Attributes="EditorOrder(62), EditorDisplay(\"Debug\")")
    float LabelVolumeOpacity = 1.0f;
    API_FIELD(Attributes="EditorOrder(63), EditorDisplay(\"Debug\")")
    float LabelLifetimeOpacity = 1.0f;
    API_FIELD(Attributes="EditorOrder(64), EditorDisplay(\"Debug\")")
    float LabelFadeLength = 0.5f;
    API_FIELD(Attributes="EditorOrder(65), EditorDisplay(\"Debug\")")
    Color LabelStartColor = Color(0.0f, 0.75f, 1.0f);
    API_FIELD(Attributes="EditorOrder(66), EditorDisplay(\"Debug\")")
    Color LabelEndColor = Color::Gray;
    API_FIELD(Attributes="EditorOrder(67), EditorDisplay(\"Debug\")")
    Color LabelOutlineColor = Color::Black;
    API_FIELD(Attributes="EditorOrder(67), EditorDisplay(\"Debug\")")
    Color LabelStaleColor = Color::Gray;
    API_FIELD(Attributes="EditorOrder(67), EditorDisplay(\"Debug\")")
    Color LabelStartedPulseColor = Color::Orange;
    API_FIELD(Attributes="EditorOrder(67), EditorDisplay(\"Debug\")")
    Color LabelStoppedPulseColor = Color::OrangeRed;
    API_FIELD(Attributes="EditorOrder(68), EditorDisplay(\"Debug\")")
    bool ShowStaleLabels = true;
    API_FIELD(Attributes="EditorOrder(69), EditorDisplay(\"Debug\")")
    float StaleLabelLifetime = 1.0f;

    /// <summary>Writes a throttled summary to the engine console for diagnostics capture.</summary>
    API_FIELD(Attributes="EditorOrder(70), EditorDisplay(\"Console Logging\")")
    bool EnableConsoleLogging = false;
    API_FIELD(Attributes="EditorOrder(71), EditorDisplay(\"Console Logging\")")
    float ConsoleLogInterval = 1.0f;
    API_FIELD(Attributes="EditorOrder(72), EditorDisplay(\"Console Logging\")")
    bool LogOnlyOnChanges = true;
    API_FIELD(Attributes="EditorOrder(80), EditorDisplay(\"Diagnostics\"), Limit(0.05f, 10, 0.05f)")
    float DiagnosticsRefreshInterval = 0.2f;

    /// <summary>
    /// Most recently captured backend telemetry. This is read-only observer state.
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor, NoSerialize")
    AudioDiagnosticsSnapshot GetSnapshot() const;
    API_PROPERTY(Attributes="EditorOrder(110), EditorDisplay(\"Scene Audio\")")
    const AudioSceneDiagnostics& GetSceneDiagnostics() const { return _sceneDiagnostics; }

    /// <summary>
    /// Captures current backend telemetry immediately.
    /// </summary>
    API_FUNCTION() void CaptureDiagnostics();

    /// <summary>
    /// Stops all event instances through the generic audio API.
    /// </summary>
    API_FUNCTION() void StopAllEvents();

    // [Actor]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    void OnEnable() override;
    void OnDisable() override;
#if USE_EDITOR
    void OnDebugDraw() override;
#endif

private:
    float _consoleLogElapsed = 0.0f;
    float _captureElapsed = 0.0f;
    int32 _lastLoggedActiveInstances = -1;
    int32 _lastLoggedLoadedBanks = -1;
    uint64 _lastLoggedDroppedCallbacks = 0;
    bool _lastLoggedInitialized = false;

    void Update();
};
