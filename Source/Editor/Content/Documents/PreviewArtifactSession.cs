// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Content.Documents
{
    /// <summary>Read-only preview pinned to a persistent object and published revision.</summary>
    public sealed class PreviewArtifactSession : IDisposable
    {
        public AssetObjectId ObjectID { get; }
        public ulong Revision { get; }
        public Asset Asset { get; private set; }

        public PreviewArtifactSession(AssetObjectId objectId)
        {
            ObjectID = objectId;
            if (!AssetWorkspaceQuery.TryGet(objectId, out var entry))
                throw new InvalidOperationException("The asset object is not present in the current database snapshot.");
            Revision = entry.Revision;
            Asset = AssetDatabaseQueryService.LoadAssetPreview(objectId);
        }

        public bool IsCurrent => AssetWorkspaceQuery.TryGet(ObjectID, out var entry) && entry.Revision == Revision;

        public void Dispose()
        {
            Asset = null;
        }
    }
}
