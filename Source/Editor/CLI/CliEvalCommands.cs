// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Text.RegularExpressions;
using FlaxEditor.Utilities;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>
    /// Session-scoped diagnostic evaluation. This deliberately evaluates a small, deterministic
    /// expression language rather than claiming to sandbox arbitrary C# in the Editor process.
    /// </summary>
    internal static class CliEvalCommands
    {
        private const int MaxCodeLength = 64 * 1024;
        private static DateTime _unlockedUntilUtc;
        private static string _unlockToken;

        [CliCommand("dev.unlock-eval", Description = "Unlock bounded diagnostic eval for this running Editor session.", Access = CliCommandAccess.MutatesProject)]
        public static object UnlockEval([CliOption("expires-seconds", Description = "Unlock lifetime, capped at five minutes.")] int expiresSeconds = 120)
        {
            expiresSeconds = Math.Clamp(expiresSeconds, 1, 300);
            _unlockedUntilUtc = DateTime.UtcNow.AddSeconds(expiresSeconds);
            _unlockToken = Guid.NewGuid().ToString("N");
            Editor.LogWarning($"CLI diagnostic eval unlocked for {expiresSeconds} seconds. It is expression-only and cannot mutate project state.");
            return new { unlocked = true, expiresAtUtc = _unlockedUntilUtc, token = _unlockToken, mode = "guarded-expression" };
        }

        [CliCommand("dev.eval", Description = "Evaluate one bounded diagnostic expression after explicit session unlock.", Access = CliCommandAccess.ReadOnly)]
        public static object Eval([CliOption("code", Description = "Expression such as 'return Level.ScenesCount;'.", Required = true)] string code)
        {
            EnsureUnlocked();
            return Evaluate(code);
        }

        [CliCommand("dev.eval-file", Description = "Evaluate a bounded diagnostic expression file under the project root after explicit session unlock.", Access = CliCommandAccess.ReadOnly)]
        public static object EvalFile([CliOption("path", Description = "Project-relative expression file.", Required = true)] string path)
        {
            EnsureUnlocked();
            var root = Path.GetFullPath(Globals.ProjectFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var fullPath = Path.GetFullPath(path, root);
            if (!fullPath.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Eval files must remain under the project root.");
            if (!File.Exists(fullPath))
                throw new FileNotFoundException("Eval file was not found.", fullPath);
            return Evaluate(File.ReadAllText(fullPath));
        }

        private static void EnsureUnlocked()
        {
            if (DateTime.UtcNow >= _unlockedUntilUtc)
                throw new UnauthorizedAccessException("Diagnostic eval is locked. Invoke dev.unlock-eval in this Editor session first.");
        }

        private static object Evaluate(string code)
        {
            if (string.IsNullOrWhiteSpace(code) || code.Length > MaxCodeLength)
                throw new ArgumentException($"Eval code must be between 1 and {MaxCodeLength} characters.", nameof(code));
            var expression = code.Trim();
            if (expression.StartsWith("return ", StringComparison.OrdinalIgnoreCase))
                expression = expression[7..].Trim();
            if (expression.EndsWith(";", StringComparison.Ordinal))
                expression = expression[..^1].Trim();
            if (expression.Contains("=", StringComparison.Ordinal) || expression.Contains("new ", StringComparison.OrdinalIgnoreCase) || expression.Contains("Object.", StringComparison.OrdinalIgnoreCase) || expression.Contains("File.", StringComparison.OrdinalIgnoreCase) || expression.Contains("Process.", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Eval is expression-only. Use a typed command for mutations or external process/file access.");

            object value;
            switch (expression)
            {
            case "Level.ScenesCount":
                value = Level.ScenesCount;
                break;
            case "Level.IsAnySceneLoaded":
                value = Level.IsAnySceneLoaded;
                break;
            case "Editor.IsPlayMode":
                value = Editor.IsPlayMode;
                break;
            case "Navigation.IsBuildingNavMesh":
                value = Navigation.IsBuildingNavMesh;
                break;
            case "Time.DeltaTime":
                value = Time.DeltaTime;
                break;
            case "Math.PI":
                value = Math.PI;
                break;
            case "true":
                value = true;
                break;
            case "false":
                value = false;
                break;
            case "null":
                value = null;
                break;
            default:
                if ((expression.StartsWith("\"") && expression.EndsWith("\"")) || (expression.StartsWith("{") && expression.EndsWith("}")) || (expression.StartsWith("[") && expression.EndsWith("]")))
                {
                    try
                    {
                        value = expression.StartsWith("\"") ? JsonConvert.DeserializeObject<string>(expression) : JToken.Parse(expression);
                    }
                    catch (Exception ex)
                    {
                        throw new ArgumentException($"Eval literal is invalid: {ex.Message}");
                    }
                }
                else if (!Regex.IsMatch(expression, "^[0-9eE+\\-*/(). _^]+$"))
                {
                    throw new ArgumentException("Eval only supports documented engine queries, JSON/string/bool/null literals, and numeric arithmetic.");
                }
                else
                {
                    try
                    {
                        value = ShuntingYard.Parse(expression);
                    }
                    catch (Exception ex)
                    {
                        throw new ArgumentException($"Eval arithmetic failed: {ex.Message}");
                    }
                }
                break;
            }
            return new { mode = "guarded-expression", value, expiresAtUtc = _unlockedUntilUtc };
        }
    }
}
