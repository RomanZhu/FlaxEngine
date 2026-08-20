// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/AABB.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Types/Guid.h"
#include "Brush.h"
#include "Types.h"

#if COMPILE_WITH_CSG_BUILDER

namespace CSG
{
    class Mesh;

    /// <summary>
    /// Immutable snapshot of a single CSG operand for evaluation.
    /// </summary>
    struct Operand
    {
        Brush* SourceBrush = nullptr;
        Guid BrushId = Guid::Empty;
        Mode Mode = Mode::Additive;
        bool FlipNormals = false;
        int32 OperationIndex = 0;
        AABB Bounds;
        Array<Surface> Surfaces;
    };

    /// <summary>
    /// Point evaluation result tracking solid state and the latest containing operand index.
    /// </summary>
    struct PointState
    {
        bool Solid = false;
        int32 LastInfluencingOperation = -1;
    };

    /// <summary>
    /// Statistics captured during stack evaluation.
    /// </summary>
    struct StackBuildStats
    {
        int32 OperandCount = 0;
        int32 CandidatePolygonCount = 0;
        int32 SplitCount = 0;
        int32 FinalFragmentCount = 0;
        int32 DiscardedInternalCount = 0;
        int32 DuplicateFragmentCount = 0;
        int32 OverlappingPairsCount = 0;
        int32 DisjointPairsCount = 0;
    };
}

#endif
