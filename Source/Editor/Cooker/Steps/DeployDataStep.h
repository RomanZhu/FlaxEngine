// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Editor/Cooker/GameCooker.h"

class GameSettings;

/// <summary>
/// Engine and game content and data files deployment step.
/// </summary>
/// <seealso cref="GameCooker::BuildStep" />
class DeployDataStep : public GameCooker::BuildStep
{
public:

    /// <summary>Collects runtime settings referenced by GameSettings for the target platform.</summary>
    static void CollectGameSettingsRoots(const GameSettings& settings, BuildPlatform platform, Array<Guid>& roots);

    // [BuildStep]
    bool Perform(CookingData& data) override;
};
