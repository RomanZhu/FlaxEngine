// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "SceneFragmentId.h"
#include "Engine/Core/Types/String.h"

/// <summary>Classifies private scene fragment reconciliation failures.</summary>
enum class SceneFragmentDiagnosticCode : byte
{
    None,
    IndexMissing,
    Malformed,
    FutureVersion,
    OwnerMismatch,
    MissingFragment,
    DuplicateLocalId,
    MisplacedFragment,
    OrphanFragment,
    ContentMismatch,
};

/// <summary>One scene-owned private fragment diagnostic.</summary>
struct FLAXENGINE_API SceneFragmentDiagnostic
{
    SceneFragmentDiagnosticCode Code = SceneFragmentDiagnosticCode::None;
    SceneFragmentId Fragment;
    String Path;
    String Message;
};
