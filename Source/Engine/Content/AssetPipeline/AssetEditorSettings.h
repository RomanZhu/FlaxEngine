// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config/Settings.h"
#include "Engine/Core/Math/Ray.h"

/// <summary>Project-scoped editor asset-pipeline policy stored as a mandatory authored settings source.</summary>
API_CLASS(sealed, Namespace="FlaxEditor.Content.Settings") class FLAXENGINE_API AssetEditorSettings : public SettingsBase
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetEditorSettings);
    API_AUTO_SERIALIZATION();

public:
    /// <summary>Default scene spawn position and view direction.</summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Project\")")
    Ray DefaultSceneSpawn = Ray(Vector3::Zero, Vector3::Forward);

    /// <summary>Whether filesystem changes automatically start a convergence refresh.</summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Asset Pipeline\")")
    bool AutoRefresh = true;

    /// <summary>Whether focusing the editor schedules a bounded validation scan.</summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Asset Pipeline\")")
    bool ValidateOnFocus = true;

    /// <summary>Whether interactive mutations use recoverable project trash by default.</summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Asset Pipeline\")")
    bool RecoverableDelete = true;
};
