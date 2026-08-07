// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using FlaxEngine;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Newtonsoft.Json;

namespace FlaxEditor
{
    /// <summary>
    /// Explicitly unlocked in-process C# evaluation. This is intentionally not presented as a sandbox:
    /// code executes with the Editor's managed privileges and is audited before it runs.
    /// </summary>
    internal static class CliCSharpEvalCommands
    {
        private const int MaxCodeLength = 256 * 1024;
        private static DateTime _unlockedUntilUtc;
        private static string _unlockToken;

        [CliCommand("dev.unlock-csharp", Description = "Unlock arbitrary in-process C# execution for this Editor session.", Access = CliCommandAccess.MutatesProject)]
        public static object Unlock([CliOption("expires-seconds", Description = "Unlock lifetime, capped at five minutes.")] int expiresSeconds = 60)
        {
            expiresSeconds = Math.Clamp(expiresSeconds, 1, 300);
            _unlockedUntilUtc = DateTime.UtcNow.AddSeconds(expiresSeconds);
            _unlockToken = Guid.NewGuid().ToString("N");
            Editor.LogWarning($"CLI arbitrary C# execution unlocked for {expiresSeconds} seconds. Code runs in-process with Editor privileges.");
            return new { unlocked = true, mode = "arbitrary-csharp", expiresAtUtc = _unlockedUntilUtc, token = _unlockToken, warning = "Not a sandbox; code has Editor process privileges." };
        }

        [CliCommand("dev.eval-csharp", Description = "Compile and execute arbitrary C# in the current Editor process after explicit unlock.", Access = CliCommandAccess.MutatesProject)]
        public static object Eval([CliOption("code", Description = "C# statements or a return expression.", Required = true)] string code, [CliOption("token", Description = "Token returned by dev.unlock-csharp.", Required = true)] string token)
        {
            EnsureUnlocked(token);
            return CompileAndRun(code);
        }

        [CliCommand("dev.eval-csharp-file", Description = "Compile and execute a C# file under the project root after explicit unlock.", Access = CliCommandAccess.MutatesProject)]
        public static object EvalFile([CliOption("path", Description = "Project-relative C# file.", Required = true)] string path, [CliOption("token", Description = "Token returned by dev.unlock-csharp.", Required = true)] string token)
        {
            EnsureUnlocked(token);
            var root = Path.GetFullPath(Globals.ProjectFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var fullPath = Path.GetFullPath(path, root);
            if (!fullPath.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new UnauthorizedAccessException("C# eval files must remain under the project root.");
            if (!File.Exists(fullPath)) throw new FileNotFoundException("C# eval file was not found.", fullPath);
            return CompileAndRun(File.ReadAllText(fullPath));
        }

        private static void EnsureUnlocked(string token)
        {
            if (DateTime.UtcNow >= _unlockedUntilUtc || string.IsNullOrWhiteSpace(_unlockToken) || !CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(_unlockToken), Encoding.UTF8.GetBytes(token ?? string.Empty)))
                throw new UnauthorizedAccessException("Arbitrary C# execution is locked or the unlock token is invalid. Invoke dev.unlock-csharp first.");
        }

        private static object CompileAndRun(string code)
        {
            if (string.IsNullOrWhiteSpace(code) || code.Length > MaxCodeLength)
                throw new ArgumentException($"C# eval code must be between 1 and {MaxCodeLength} characters.", nameof(code));
            var sourceHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(code))).ToLowerInvariant();
            Audit(sourceHash, code.Length);
            var body = code.Contains("return", StringComparison.Ordinal) || code.Contains(";", StringComparison.Ordinal) ? code : "return " + code + ";";
            var source = "using System; using System.Linq; using System.Collections.Generic; using FlaxEngine; using FlaxEditor;\n" +
                         "public static class FlaxCliEvalEntry { public static object Execute() { " + body + " } }";
            var tree = CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(LanguageVersion.Latest));
            var references = AppDomain.CurrentDomain.GetAssemblies()
                .Where(x => !x.IsDynamic && !string.IsNullOrWhiteSpace(x.Location) && File.Exists(x.Location))
                .Select(x => MetadataReference.CreateFromFile(x.Location))
                .GroupBy(x => x.Display, StringComparer.OrdinalIgnoreCase).Select(x => x.First()).ToArray();
            var compilation = CSharpCompilation.Create("FlaxCliEval_" + sourceHash, new[] { tree }, references,
                new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release));
            using var assemblyStream = new MemoryStream();
            var emit = compilation.Emit(assemblyStream);
            if (!emit.Success)
            {
                var diagnostics = emit.Diagnostics.Where(x => x.Severity == DiagnosticSeverity.Error).Select(x => new { id = x.Id, message = x.GetMessage(), line = x.Location.GetLineSpan().StartLinePosition.Line + 1 }).ToArray();
                throw new InvalidOperationException("C# eval compilation failed: " + JsonConvert.SerializeObject(diagnostics));
            }
            assemblyStream.Position = 0;
            var assembly = Assembly.Load(assemblyStream.ToArray());
            var method = assembly.GetType("FlaxCliEvalEntry")?.GetMethod("Execute", BindingFlags.Public | BindingFlags.Static) ?? throw new InvalidOperationException("The generated C# entry point was not found.");
            var value = method.Invoke(null, null);
            return new { mode = "arbitrary-csharp", value, sourceHash, expiresAtUtc = _unlockedUntilUtc };
        }

        private static void Audit(string hash, int length)
        {
            try
            {
                var path = Path.Combine(Globals.ProjectFolder, ".flax", "cli-csharp-eval.audit.log");
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.AppendAllText(path, $"{DateTime.UtcNow:O}\t{hash}\t{length}\n");
            }
            catch { }
        }
    }
}
