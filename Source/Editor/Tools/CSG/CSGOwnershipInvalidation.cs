// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG
{
    /// <summary>
    /// Helper to invalidate and rebuild CSG compilation targets affected by hierarchy mutations.
    /// </summary>
    public static class CSGOwnershipInvalidation
    {
        /// <summary>
        /// Collects the CSG target (CSGModel or Scene) for an actor or parent.
        /// </summary>
        public static Actor ResolveTarget(Actor actor)
        {
            return CSGRebuildScheduler.ResolveTarget(actor);
        }

        /// <summary>
        /// Rebuilds all unique CSG targets in the provided collection.
        /// </summary>
        public static void InvalidateTargets(IEnumerable<Actor> targets)
        {
            if (targets == null)
                return;

            var unique = new HashSet<Actor>();
            foreach (var target in targets)
            {
                if (target != null)
                    unique.Add(target);
            }

            foreach (var target in unique)
            {
                CSGRebuildScheduler.Shared.RequestFinal(target);
            }
        }

        /// <summary>
        /// Collects targets for actors before and after a reparent operation and invalidates them.
        /// </summary>
        public static void InvalidateReparent(IEnumerable<Actor> actors, Actor newParent)
        {
            if (actors == null)
                return;

            var targets = new HashSet<Actor>();
            var newTarget = ResolveTarget(newParent);
            if (newTarget != null)
                targets.Add(newTarget);

            foreach (var actor in actors)
            {
                if (actor == null)
                    continue;
                if (actor is BoxBrush || actor is CSGScopeActor)
                {
                    var oldTarget = ResolveTarget(actor);
                    if (oldTarget != null)
                        targets.Add(oldTarget);
                }
            }

            InvalidateTargets(targets);
        }
    }
}
