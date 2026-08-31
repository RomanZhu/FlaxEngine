// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Content.Documents
{
    /// <summary>Editor session for one persistent asset object and one observed database revision.</summary>
    public sealed class AssetDocumentSession : IDisposable
    {
        /// <summary>Gets the persistent identity of the edited object.</summary>
        public AssetObjectId ObjectID { get; }

        /// <summary>Gets the database revision observed by this session.</summary>
        public ulong ObservedRevision { get; private set; }

        /// <summary>Gets the canonical source path.</summary>
        public string SourcePath { get; private set; }

        /// <summary>Gets whether the document has unsaved changes.</summary>
        public bool IsDirty { get; private set; }

        /// <summary>Gets whether the database changed since the previous refresh.</summary>
        public bool IsStale { get; private set; }

        /// <summary>Gets the loaded asset object.</summary>
        public Asset Asset { get; private set; }

        /// <summary>Occurs when the session state changes.</summary>
        public event Action<AssetDocumentSession> Changed;

        internal AssetDocumentSession(AssetObjectId objectId)
        {
            if (objectId.IsNull)
                throw new ArgumentException("A document session requires a persistent asset object ID.", nameof(objectId));
            ObjectID = objectId;
            Refresh();
        }

        /// <summary>Marks the document as having unsaved changes.</summary>
        public void MarkDirty()
        {
            if (IsDirty)
                return;
            IsDirty = true;
            Changed?.Invoke(this);
        }

        /// <summary>Refreshes the session from the asset database.</summary>
        public void Refresh()
        {
            if (!AssetWorkspaceQuery.TryGet(ObjectID, out var entry))
            {
                IsStale = true;
                Changed?.Invoke(this);
                return;
            }
            IsStale = ObservedRevision != 0 && ObservedRevision != entry.Revision;
            ObservedRevision = entry.Revision;
            SourcePath = entry.SourcePath;
            Asset = FlaxEngine.Content.LoadAssetAsync(ObjectID);
            Changed?.Invoke(this);
        }

        /// <summary>Saves the canonical source and synchronously imports it.</summary>
        /// <param name="saveSource">Callback that writes the edited source.</param>
        /// <returns>True when the source was saved and the session remains current.</returns>
        public bool Save(Func<AssetDocumentSession, bool> saveSource)
        {
            if (!IsDirty)
                return true;
            if (IsStale || saveSource == null || !saveSource(this))
                return false;
            IsDirty = false;
            AssetDatabase.ImportAsset(SourcePath, ImportAssetOptions.ForceSynchronousImport);
            Refresh();
            return !IsStale;
        }

        /// <inheritdoc />
        public void Dispose()
        {
            Asset = null;
            Changed = null;
        }
    }
}
