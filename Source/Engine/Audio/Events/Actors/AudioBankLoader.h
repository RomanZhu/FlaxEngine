// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Audio/Events/Assets/AudioBank.h"
#include "Engine/Audio/Events/AudioActivation.h"

/// <summary>
/// Scene actor that loads and manages the lifetime of sound banks for the scene or level.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Bank Loader\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioBankLoader : public Actor
{
    DECLARE_SCENE_OBJECT(AudioBankLoader);

private:
    AudioActivationState _loadActivationState;
    AudioActivationState _unloadActivationState;

public:
    /// <summary>
    /// The bank assets to load.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Banks\")")
    Array<JsonAssetReference<AudioBank>> Banks;

    /// <summary>
    /// Explicit bank file paths to load.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Banks\"), HideInEditor")
    Array<String> BankPaths;

    /// <summary>
    /// If true, banks are automatically loaded when the level starts or actor enables.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Loading\")")
    bool LoadOnStart = true;

    /// <summary>
    /// If true, banks are automatically unloaded when the actor disables or scene unloads.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Loading\")")
    bool UnloadOnDisable = true;

    /// <summary>Loads sample data after each bank finishes loading.</summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Loading\")")
    bool PreloadSampleData = false;

    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Activation\", \"Load Event\")")
    AudioActivationBinding LoadActivation;

    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Activation\", \"Unload Event\")")
    AudioActivationBinding UnloadActivation;

public:
    /// <summary>
    /// Loads all configured sound banks.
    /// </summary>
    API_FUNCTION() void LoadBanks();

    /// <summary>
    /// Unloads all configured sound banks.
    /// </summary>
    API_FUNCTION() void UnloadBanks();

    API_FUNCTION() bool SignalActivation(AudioActivationEvent activationEvent, Actor* source = nullptr, Actor* target = nullptr);

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override
    {
        const Vector3 size(40.0f);
        return BoundingBox(_transform.Translation - size, _transform.Translation + size);
    }
#endif
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void BeginPlay(SceneBeginData* data) override;
    void EndPlay() override;
};
