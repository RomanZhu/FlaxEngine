// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>Managed immutable view of one ordered database revision.</summary>
    public sealed class AssetChangeSetModel
    {
        public ulong Revision { get; private set; }
        public Guid[] Added { get; private set; }
        public Guid[] Removed { get; private set; }
        public Guid[] Changed { get; private set; }
        public Guid[] StatusChanged { get; private set; }

        /// <summary>Reads every retained durable change after the supplied revision.</summary>
        public static AssetChangeSetModel[] ReadAfter(ulong revision, out bool requiresSnapshot)
        {
            var changes = AssetDatabaseQueryService.GetChangesAfter(revision, out requiresSnapshot);
            if (changes == null || changes.Length == 0)
                return Array.Empty<AssetChangeSetModel>();
            var result = new AssetChangeSetModel[changes.Length];
            for (int i = 0; i < changes.Length; i++)
            {
                var change = changes[i];
                result[i] = new AssetChangeSetModel
                {
                    Revision = change.Revision,
                    Added = change.Added ?? Array.Empty<Guid>(),
                    Removed = change.Removed ?? Array.Empty<Guid>(),
                    Changed = change.Changed ?? Array.Empty<Guid>(),
                    StatusChanged = change.StatusChanged ?? Array.Empty<Guid>(),
                };
            }
            return result;
        }
    }
}
