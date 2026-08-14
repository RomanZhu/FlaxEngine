// Copyright (c) Wojciech Figat. All rights reserved.

using System.Threading;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Opt-in diagnostics for Content Window input, focus, and filesystem mutations.
    /// </summary>
    internal static class ContentMutationDiagnostics
    {
        private const int MaxValueLength = 240;
        private static long _sequence;
        private static bool _enabled;

        /// <summary>
        /// Gets whether Content Window diagnostics are enabled.
        /// </summary>
        public static bool Enabled => _enabled;

        /// <summary>
        /// Writes an ordered Content Window diagnostic entry when tracing is enabled.
        /// </summary>
        public static void Log(string eventName, string details = null)
        {
            if (!_enabled)
                return;

            var sequence = Interlocked.Increment(ref _sequence);
            if (string.IsNullOrEmpty(details))
                Editor.Log($"[ContentDebug #{sequence}] {eventName}");
            else
                Editor.Log($"[ContentDebug #{sequence}] {eventName} | {Sanitize(details)}");
        }

        /// <summary>
        /// Converts user-controlled text to a bounded single-line diagnostic value.
        /// </summary>
        public static string Sanitize(string value)
        {
            if (value == null)
                return "<null>";
            value = value.Replace("\r", "\\r").Replace("\n", "\\n").Replace("\t", "\\t");
            return value.Length <= MaxValueLength ? value : value.Substring(0, MaxValueLength) + "...";
        }

        [EditorCommand("content.debug", "Toggle ordered Content Window input, focus, and mutation diagnostics.")]
        private static string Toggle()
        {
            _enabled = !_enabled;
            return SetStateMessage();
        }

        [EditorCommand("content.debug.on", "Enable ordered Content Window input, focus, and mutation diagnostics.")]
        private static string Enable()
        {
            _enabled = true;
            return SetStateMessage();
        }

        [EditorCommand("content.debug.off", "Disable Content Window diagnostics.")]
        private static string Disable()
        {
            _enabled = false;
            return SetStateMessage();
        }

        private static string SetStateMessage()
        {
            var state = _enabled ? "enabled" : "disabled";
            var message = $"Content Window diagnostics {state}. Log prefix: [ContentDebug]";
            Editor.Log(message);
            return message;
        }
    }
}
