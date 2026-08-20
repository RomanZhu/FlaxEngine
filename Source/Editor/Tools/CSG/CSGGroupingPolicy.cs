// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using System.Linq;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG
{
    internal enum CSGGroupingKind
    {
        Group,
        CSGStack,
        CSGModel,
    }

    internal readonly struct CSGGroupingPlan
    {
        public readonly CSGGroupingKind Kind;
        public readonly bool WrapLooseBrushesInStack;

        public CSGGroupingPlan(CSGGroupingKind kind, bool wrapLooseBrushesInStack = false)
        {
            Kind = kind;
            WrapLooseBrushesInStack = wrapLooseBrushesInStack;
        }
    }

    internal static class CSGGroupingPolicy
    {
        public static CSGGroupingPlan Classify(IReadOnlyList<Actor> actors)
        {
            if (actors == null || actors.Count == 0)
                return new CSGGroupingPlan(CSGGroupingKind.Group);

            bool anyModel = actors.Any(x => x is CSGModel);
            bool anyNonCsg = actors.Any(x => x is not BoxBrush && x is not CSGStack && x is not CSGModel);
            if (anyModel || anyNonCsg)
                return new CSGGroupingPlan(CSGGroupingKind.Group);

            bool anyStack = actors.Any(x => x is CSGStack);
            bool anyBrush = actors.Any(x => x is BoxBrush);

            if (anyStack)
                return new CSGGroupingPlan(CSGGroupingKind.CSGModel, wrapLooseBrushesInStack: anyBrush);

            if (anyBrush)
                return new CSGGroupingPlan(CSGGroupingKind.CSGStack);

            return new CSGGroupingPlan(CSGGroupingKind.Group);
        }
    }
}
