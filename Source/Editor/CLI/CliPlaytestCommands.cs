// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using FlaxEngine;
using Object = FlaxEngine.Object;

namespace FlaxEditor
{
    /// <summary>
    /// Built-in deterministic playtest observation and assertion commands.
    /// </summary>
    internal static class CliPlaytestCommands
    {
        [CliCommand("playtest.status", Description = "Read the current play mode and runtime scene snapshot.", Access = CliCommandAccess.ReadOnly)]
        public static object Status()
        {
            var actors = EnumerateActors().ToArray();
            return new
            {
                playMode = Editor.IsPlayMode,
                paused = Editor.IsPlayMode && Editor.Instance.StateMachine.PlayingState.IsPaused,
                loadedScenes = Level.ScenesCount,
                runtimeActors = actors.Length,
                scenes = Level.Scenes.Select(x => new { id = x.ID, name = x.Name }).ToArray(),
                timestampUtc = DateTime.UtcNow,
            };
        }

        [CliCommand("playtest.find", Description = "Find runtime Actors by stable ID, name, type, or active state.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static object Find([CliOption("actor", Description = "Exact Actor ID.")] Guid? actor = null,
            [CliOption("name", Description = "Exact Actor name.")] string name = null,
            [CliOption("type", Description = "Actor TypeName or CLR full name.")] string type = null,
            [CliOption("active", Description = "Filter by active state.")] bool? active = null,
            [CliOption("limit", Description = "Maximum number of matches.")] int limit = 100)
        {
            if (limit < 1 || limit > 10000)
                throw new ArgumentOutOfRangeException(nameof(limit), "limit must be between 1 and 10000.");
            var matches = FindMatches(actor, name, type, active).Take(limit).Select(DescribeActor).ToArray();
            return new
            {
                found = matches.Length != 0,
                count = matches.Length,
                matches,
            };
        }

        [CliCommand("playtest.assert", Description = "Assert that a runtime Actor exists and optionally matches active state.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static CliCommandResult Assert([CliOption("actor", Description = "Exact Actor ID.")] Guid? actor = null,
            [CliOption("name", Description = "Exact Actor name.")] string name = null,
            [CliOption("type", Description = "Actor TypeName or CLR full name.")] string type = null,
            [CliOption("active", Description = "Expected active state.")] bool? active = null,
            [CliOption("exists", Description = "Whether at least one match must exist.")] bool exists = true)
        {
            var match = FindMatches(actor, name, type, null).FirstOrDefault();
            var found = match != null;
            var activeMatches = !active.HasValue || (found && match.IsActive == active.Value);
            var passed = found == exists && activeMatches;
            var data = new
            {
                passed,
                expected = new { actor, name, type, active, exists },
                actual = found ? DescribeActor(match) : null,
            };
            return passed
                ? CliCommandResult.Success(data)
                : CliCommandResult.Failure("FLX-PLAYTEST-ASSERT-0004", "The playtest assertion failed.", data);
        }

        [CliCommand("playtest.wait", Description = "Wait cooperatively for a runtime Actor to match the supplied condition.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static CliCommandOperation Wait([CliOption("actor", Description = "Exact Actor ID.")] Guid? actor = null,
            [CliOption("name", Description = "Exact Actor name.")] string name = null,
            [CliOption("type", Description = "Actor TypeName or CLR full name.")] string type = null,
            [CliOption("active", Description = "Filter by active state.")] bool? active = null,
            [CliOption("timeout-seconds", Description = "Maximum wait duration.")] double timeoutSeconds = 5.0,
            CliCommandContext context = null)
        {
            if (timeoutSeconds < 0.0 || timeoutSeconds > 3600.0)
                throw new ArgumentOutOfRangeException(nameof(timeoutSeconds), "timeout-seconds must be between 0 and 3600.");
            return new WaitOperation(actor, name, type, active, timeoutSeconds, context);
        }

        private sealed class WaitOperation : CliCommandOperation
        {
            private readonly Guid? _actor;
            private readonly string _name;
            private readonly string _type;
            private readonly bool? _active;
            private readonly double _timeoutSeconds;
            private readonly CliCommandContext _context;
            private readonly Stopwatch _clock = Stopwatch.StartNew();
            private CliCommandResult _result;

            public WaitOperation(Guid? actor, string name, string type, bool? active, double timeoutSeconds, CliCommandContext context)
            {
                _actor = actor;
                _name = name;
                _type = type;
                _active = active;
                _timeoutSeconds = timeoutSeconds;
                _context = context;
            }

            public override bool IsCompleted => _result != null;

            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (_result != null)
                    return;
                _context?.CancellationToken.ThrowIfCancellationRequested();
                var match = FindMatches(_actor, _name, _type, _active).FirstOrDefault();
                if (match != null)
                {
                    _context?.ReportProgress("Playtest condition satisfied", 1.0f);
                    _result = CliCommandResult.Success(new
                    {
                        satisfied = true,
                        elapsedSeconds = _clock.Elapsed.TotalSeconds,
                        actor = DescribeActor(match),
                    });
                    return;
                }

                var elapsed = _clock.Elapsed.TotalSeconds;
                if (elapsed >= _timeoutSeconds)
                {
                    _result = CliCommandResult.Failure("FLX-PLAYTEST-WAIT-0004", "Timed out waiting for the playtest condition.", new
                    {
                        satisfied = false,
                        elapsedSeconds = elapsed,
                        timeoutSeconds = _timeoutSeconds,
                        expected = new { actor = _actor, name = _name, type = _type, active = _active },
                    });
                    return;
                }

                _context?.ReportProgress("Waiting for playtest condition", (float)Math.Min(0.99, elapsed / Math.Max(0.001, _timeoutSeconds)));
            }

            public override void Cancel()
            {
                if (_result == null)
                    _result = CliCommandResult.Failure("FLX-PLAYTEST-WAIT-0005", "Playtest wait was cancelled.");
            }
        }

        private static IEnumerable<Actor> EnumerateActors()
        {
            foreach (var scene in Level.Scenes)
            {
                for (var i = 0; i < scene.ChildrenCount; i++)
                {
                    foreach (var actor in Enumerate(scene.GetChild(i)))
                        yield return actor;
                }
            }
        }

        private static IEnumerable<Actor> Enumerate(Actor root)
        {
            yield return root;
            for (var i = 0; i < root.ChildrenCount; i++)
            {
                foreach (var actor in Enumerate(root.GetChild(i)))
                    yield return actor;
            }
        }

        private static IEnumerable<Actor> FindMatches(Guid? actorId, string name, string type, bool? active)
        {
            if (actorId.HasValue)
            {
                var id = actorId.Value;
                var exact = Object.Find<Actor>(ref id);
                if (exact != null && exact.HasScene && Matches(exact, name, type, active))
                    yield return exact;
                yield break;
            }

            foreach (var candidate in EnumerateActors())
            {
                if (Matches(candidate, name, type, active))
                    yield return candidate;
            }
        }

        private static bool Matches(Actor actor, string name, string type, bool? active)
        {
            if (!string.IsNullOrWhiteSpace(name) && !string.Equals(actor.Name, name, StringComparison.Ordinal))
                return false;
            if (!string.IsNullOrWhiteSpace(type) && !string.Equals(actor.TypeName, type, StringComparison.OrdinalIgnoreCase) && !string.Equals(actor.GetType().FullName, type, StringComparison.OrdinalIgnoreCase))
                return false;
            return !active.HasValue || actor.IsActive == active.Value;
        }

        private static object DescribeActor(Actor actor)
        {
            var names = new Stack<string>();
            for (var current = actor; current != null; current = current.Parent)
                names.Push(current.Name);
            return new
            {
                sceneId = actor.Scene?.ID ?? Guid.Empty,
                actorId = actor.ID,
                name = actor.Name,
                type = actor.TypeName,
                active = actor.IsActive,
                path = string.Join("/", names),
            };
        }
    }
}
