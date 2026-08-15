// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using FlaxEngine;

namespace FlaxEditor.Content
{
    internal static class ContentMutationPathUtils
    {
        public static readonly StringComparer Comparer = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        public static readonly StringComparison Comparison = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

        public static string Normalize(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return null;
            try
            {
                return StringUtils.NormalizePath(Path.GetFullPath(path));
            }
            catch (ArgumentException)
            {
                return null;
            }
            catch (NotSupportedException)
            {
                return null;
            }
            catch (PathTooLongException)
            {
                return null;
            }
        }

        public static bool AreEquivalent(string left, string right)
        {
            if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right))
                return false;
            return Comparer.Equals(Normalize(left), Normalize(right));
        }

        public static bool IsCaseOnlyRename(string sourcePath, string destinationPath)
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows) || string.IsNullOrWhiteSpace(sourcePath) || string.IsNullOrWhiteSpace(destinationPath))
                return false;
            var source = Normalize(sourcePath);
            var destination = Normalize(destinationPath);
            return Comparer.Equals(source, destination) && !string.Equals(source, destination, StringComparison.Ordinal);
        }

        public static bool IsWithinRoot(string path, string root, bool allowRoot = true)
        {
            path = Normalize(path);
            root = Normalize(root)?.TrimEnd('/', '\\');
            if (path == null || root == null)
                return false;
            if (Comparer.Equals(path, root))
                return allowRoot;
            return path.StartsWith(root + "/", Comparison) || path.StartsWith(root + "\\", Comparison);
        }

        public static bool Exists(string path)
        {
            return !string.IsNullOrEmpty(path) && (File.Exists(path) || Directory.Exists(path));
        }

        public static bool IsSameVolume(string left, string right)
        {
            left = Normalize(left);
            right = Normalize(right);
            if (left == null || right == null)
                return false;
            var leftRoot = Path.GetPathRoot(left);
            var rightRoot = Path.GetPathRoot(right);
            return !string.IsNullOrEmpty(leftRoot) && !string.IsNullOrEmpty(rightRoot) && Comparer.Equals(leftRoot, rightRoot);
        }

        public static bool TryValidateDestinationPath(string path, out string message)
        {
            path = Normalize(path);
            if (path == null)
            {
                message = "The destination path is invalid.";
                return false;
            }

            var root = Path.GetPathRoot(path);
            var relative = string.IsNullOrEmpty(root) ? path : path.Substring(root.Length);
            var segments = relative.Split(new[] { '/', '\\' }, StringSplitOptions.RemoveEmptyEntries);
            var invalidCharacters = Path.GetInvalidFileNameChars();
            for (int i = 0; i < segments.Length; i++)
            {
                var segment = segments[i];
                if (segment == "." || segment == ".." || segment.IndexOfAny(invalidCharacters) != -1)
                {
                    message = $"The destination path contains an invalid name segment '{segment}'.";
                    return false;
                }
                if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                {
                    if (segment.EndsWith(" ", StringComparison.Ordinal) || segment.EndsWith(".", StringComparison.Ordinal))
                    {
                        message = $"The destination name '{segment}' cannot end with a space or period on Windows.";
                        return false;
                    }
                    var deviceName = Path.GetFileNameWithoutExtension(segment);
                    if (IsReservedWindowsDeviceName(deviceName))
                    {
                        message = $"The destination name '{segment}' is reserved by Windows.";
                        return false;
                    }
                }
            }

            message = null;
            return true;
        }

        private static bool IsReservedWindowsDeviceName(string name)
        {
            if (string.IsNullOrEmpty(name))
                return false;
            if (string.Equals(name, "CON", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "PRN", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "AUX", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "NUL", StringComparison.OrdinalIgnoreCase))
                return true;
            if (name.Length == 4 && (name.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || name.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)))
                return name[3] >= '1' && name[3] <= '9';
            return false;
        }

        public static bool IsReparsePoint(string path)
        {
            try
            {
                return Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
            }
            catch
            {
                return true;
            }
        }

        public static bool ContainsReparsePoint(string path, bool isDirectory)
        {
            if (!Exists(path))
                return false;
            if (IsReparsePoint(path))
                return true;
            if (!isDirectory)
                return false;

            try
            {
                var pending = new Stack<string>();
                pending.Push(path);
                while (pending.Count != 0)
                {
                    var folder = pending.Pop();
                    foreach (var entry in Directory.EnumerateFileSystemEntries(folder))
                    {
                        var attributes = File.GetAttributes(entry);
                        if ((attributes & FileAttributes.ReparsePoint) != 0)
                            return true;
                        if ((attributes & FileAttributes.Directory) != 0)
                            pending.Push(entry);
                    }
                }
                return false;
            }
            catch
            {
                // Inaccessible source state is unsafe to mutate and is treated conservatively.
                return true;
            }
        }

        public static string CreateTemporarySibling(string path, string marker)
        {
            var directory = Path.GetDirectoryName(path);
            var filename = Path.GetFileName(path);
            for (int i = 0; i < 128; i++)
            {
                var candidate = Path.Combine(directory, $".{filename}.{marker}.{Guid.NewGuid():N}.tmp");
                if (!Exists(candidate))
                    return Normalize(candidate);
            }
            throw new IOException($"Cannot reserve a temporary sibling for '{path}'.");
        }

        public static string GetExternalActorsSidecarPath(string contentPath, bool isFolder, bool isScene)
        {
            if ((!isFolder && !isScene) || string.IsNullOrWhiteSpace(contentPath))
                return null;
            var path = Normalize(contentPath);
            var contentRoot = Normalize(Globals.ProjectContentFolder);
            if (!IsWithinRoot(path, contentRoot))
                return null;
            var relativePath = Path.GetRelativePath(contentRoot, isFolder ? path : Path.ChangeExtension(path, null));
            if (relativePath == ".")
                return null;
            return Normalize(Path.Combine(Globals.ProjectFolder, "SceneActors", relativePath));
        }
    }
}
