// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using FlaxEngine;

namespace FlaxEditor.Utilities
{
    /// <summary>
    /// Resolves reflected editor members back to the engine checkout and opens their declarations.
    /// </summary>
    internal static class MemberSourceNavigator
    {
        private readonly struct SourceLocation
        {
            public readonly string Path;
            public readonly int Line;

            public SourceLocation(string path, int line)
            {
                Path = path;
                Line = line;
            }
        }

        private static readonly Dictionary<MemberInfo, SourceLocation> Cache = new Dictionary<MemberInfo, SourceLocation>();

        /// <summary>
        /// Opens the declaration of a reflected member in the configured source code editor.
        /// </summary>
        /// <param name="member">Member or type to open.</param>
        public static void Open(MemberInfo member)
        {
            if (member == null)
                return;

            if (!TryResolve(member, out var path, out var line))
            {
                Debug.LogWarning($"Cannot find the source declaration for {member.DeclaringType?.FullName ?? member.Name}.{member.Name}.");
                return;
            }

            Editor.Instance.CodeEditing.OpenFile(path, line);
        }

        /// <summary>
        /// Resolves the source file and one-based declaration line for a reflected member.
        /// </summary>
        internal static bool TryResolve(MemberInfo member, out string path, out int line)
        {
            if (member == null)
            {
                path = null;
                line = 0;
                return false;
            }

            if (!Cache.TryGetValue(member, out var location))
            {
                location = Resolve(member);
                Cache.Add(member, location);
            }

            path = location.Path;
            line = location.Line;
            return path != null;
        }

        private static SourceLocation Resolve(MemberInfo member)
        {
            var type = member as Type ?? member.DeclaringType;
            while (type?.IsNested == true)
                type = type.DeclaringType;
            if (type == null)
                return default;

            var sourceRoot = Path.Combine(Globals.StartupFolder, "Source");
            if (!Directory.Exists(sourceRoot))
                return default;

            var typeName = type.Name;
            int genericMarker = typeName.IndexOf('`');
            if (genericMarker != -1)
                typeName = typeName.Substring(0, genericMarker);

            var candidates = new List<string>();
            AddFiles(candidates, sourceRoot, typeName + ".cs");
            AddFiles(candidates, sourceRoot, typeName + ".*.cs");

            for (int i = 0; i < candidates.Count; i++)
            {
                int sourceLine = FindDeclaration(candidates[i], member, typeName);
                if (sourceLine != 0)
                    return new SourceLocation(candidates[i], sourceLine);
            }

            return default;
        }

        private static void AddFiles(List<string> result, string root, string pattern)
        {
            string[] files;
            try
            {
                files = Directory.GetFiles(root, pattern, SearchOption.AllDirectories);
            }
            catch
            {
                return;
            }

            for (int i = 0; i < files.Length; i++)
            {
                if (!result.Contains(files[i]))
                    result.Add(files[i]);
            }
        }

        private static int FindDeclaration(string path, MemberInfo member, string typeName)
        {
            string[] lines;
            try
            {
                lines = File.ReadAllLines(path);
            }
            catch
            {
                return 0;
            }

            if (member is Type)
            {
                var typePattern = new Regex(@"\b(?:class|struct|interface|enum|record)\s+" + Regex.Escape(typeName) + @"(?:\s|[:<{])");
                for (int i = 0; i < lines.Length; i++)
                {
                    if (typePattern.IsMatch(lines[i]))
                        return i + 1;
                }
                return 0;
            }

            var memberName = Regex.Escape(member.Name);
            var declarationPattern = new Regex(
                @"^\s*(?:\[[^\]]+\]\s*)*(?:public|protected|internal|private)\s+" +
                @"(?:(?:new|static|readonly|const|virtual|override|abstract|sealed|unsafe|volatile|required|partial)\s+)*" +
                @"(?:[\w.@?<>,\[\]]+\s+)+" + memberName + @"\s*(?:$|[;={])");
            for (int i = 0; i < lines.Length; i++)
            {
                if (declarationPattern.IsMatch(lines[i]))
                    return i + 1;
            }

            // Handles unusual declarations while still requiring a declaration access modifier.
            var fallbackPattern = new Regex(@"^\s*(?:public|protected|internal|private)\b.*\b" + memberName + @"\b");
            for (int i = 0; i < lines.Length; i++)
            {
                if (fallbackPattern.IsMatch(lines[i]))
                    return i + 1;
            }
            return 0;
        }
    }
}
