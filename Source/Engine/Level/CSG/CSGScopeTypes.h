// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// The kind of CSG semantic/compiler scope.
/// </summary>
API_ENUM() enum class CSGScopeKind : uint8
{
    /// <summary>
    /// Boolean interaction scope. Brushes inside this stack interact with one another.
    /// </summary>
    BooleanStack = 0,

};
