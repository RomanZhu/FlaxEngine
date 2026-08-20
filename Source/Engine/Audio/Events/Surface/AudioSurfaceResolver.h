// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Tags.h"

struct AudioSurfaceProfile;
class AudioSurfaceLibrary;

/// <summary>Hierarchical physical-material tag resolver for interaction audio.</summary>
class FLAXENGINE_API AudioSurfaceResolver
{
public:
    static const AudioSurfaceProfile* Resolve(const AudioSurfaceLibrary& library, Tag tag);
};
