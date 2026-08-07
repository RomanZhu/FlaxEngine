// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    /// <summary>
    /// Provides information about the current multiplayer play mode instance.
    /// </summary>
    public static class MultiplayerPlayMode
    {
        /// <summary>
        /// Gets a value indicating whether multiplayer play mode is active.
        /// </summary>
        public static bool IsActive { get; private set; }

        /// <summary>
        /// Gets a value indicating whether this process is a read-only editor replica.
        /// </summary>
        public static bool IsReplica { get; private set; }

        /// <summary>
        /// Gets the zero-based instance index.
        /// </summary>
        public static int InstanceIndex { get; private set; }

        /// <summary>
        /// Gets the total number of configured instances.
        /// </summary>
        public static int InstanceCount { get; private set; } = 1;

        /// <summary>
        /// Gets the tag assigned to this instance.
        /// </summary>
        public static string InstanceTag { get; private set; } = string.Empty;

        /// <summary>
        /// Gets the tags assigned to this instance.
        /// </summary>
        public static string[] InstanceTags { get; private set; } = Array.Empty<string>();

        /// <summary>
        /// Checks whether this instance has the specified tag.
        /// </summary>
        /// <param name="tag">The tag.</param>
        /// <returns>True if the tag is assigned.</returns>
        public static bool HasTag(string tag)
        {
            for (int i = 0; i < InstanceTags.Length; i++)
            {
                if (string.Equals(InstanceTags[i], tag, StringComparison.Ordinal))
                    return true;
            }
            return false;
        }

        internal static void Configure(bool active, bool replica, int index, int count, string[] tags)
        {
            IsActive = active;
            IsReplica = replica;
            InstanceIndex = index;
            InstanceCount = count;
            InstanceTags = tags ?? Array.Empty<string>();
            InstanceTag = InstanceTags.Length != 0 ? InstanceTags[0] : string.Empty;
        }
    }
}
