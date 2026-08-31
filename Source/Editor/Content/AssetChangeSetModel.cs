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

        public static AssetChangeSetModel ReadLatest()
        {
            var change = AssetDatabaseFacade.GetLastChange();
            return new AssetChangeSetModel
            {
                Revision = change.Revision,
                Added = change.Added ?? Array.Empty<Guid>(),
                Removed = change.Removed ?? Array.Empty<Guid>(),
                Changed = change.Changed ?? Array.Empty<Guid>(),
                StatusChanged = change.StatusChanged ?? Array.Empty<Guid>(),
            };
        }
    }
}
