// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Editor/Cooker/GameCooker.h"

/// <summary>
/// Cooking step that collects, validates, and deploys sound bank binaries and middleware libraries for the target platform.
/// </summary>
/// <seealso cref="GameCooker::BuildStep" />
class CookAudioStep : public GameCooker::BuildStep
{
public:
    // [BuildStep]
    bool Perform(CookingData& data) override;
};
