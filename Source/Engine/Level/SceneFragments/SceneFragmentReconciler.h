// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "SceneFragmentStore.h"

/// <summary>Validates private scene fragment state without registering public assets.</summary>
class FLAXENGINE_API SceneFragmentReconciler
{
public:
    static void Reconcile(const Guid& sceneGuid, Array<SceneFragmentDiagnostic>& diagnostics);
};
