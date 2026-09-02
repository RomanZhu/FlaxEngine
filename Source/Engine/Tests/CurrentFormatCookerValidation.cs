// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    internal static class CurrentFormatCookerValidation
    {
        public static void AssertCooks(Guid id, string caseName, System.Diagnostics.Stopwatch aggregateTimer,
            TimeSpan aggregateDeadline)
        {
            var remaining = aggregateDeadline - aggregateTimer.Elapsed;
            Assert.Greater(remaining.TotalMilliseconds, 0.0,
                "Cook validation aggregate deadline expired before " + caseName + ".");
            Debug.Log("Cook validation case begin: " + caseName + ", object=" + id +
                      ", remainingMs=" + (long)remaining.TotalMilliseconds);
            Assert.IsFalse(FlaxEditor.GameCooker.ValidateAssetCookForTesting(id,
                    Math.Max(1.0, remaining.TotalMilliseconds)),
                "Registered cooker validation failed for " + caseName + " (" + id + ").");
            Assert.Less(aggregateTimer.Elapsed, aggregateDeadline,
                "Cook validation aggregate deadline expired during " + caseName + ".");
            Debug.Log("Cook validation case end: " + caseName + ", object=" + id +
                      ", elapsedMs=" + aggregateTimer.ElapsedMilliseconds);
        }
    }
}
#endif
