// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Content.Thumbnails
{
    /// <summary>
    /// Contains information about asset thumbnail rendering.
    /// </summary>
    [HideInEditor]
    public class ThumbnailRequest
    {
        private static readonly TimeSpan PublishedPreviewGrace = TimeSpan.FromSeconds(2);

        /// <summary>
        /// The request state types.
        /// </summary>
        public enum States
        {
            /// <summary>
            /// The initial state.
            /// </summary>
            Created,

            /// <summary>
            /// A lightweight canonical thumbnail artifact is being built.
            /// </summary>
            Waiting,

            /// <summary>
            /// Request has been prepared for the rendering but still may wait for resources to load fully.
            /// </summary>
            Prepared,

            /// <summary>
            /// The thumbnail has been rendered. Request can be finalized.
            /// </summary>
            Rendered,

            /// <summary>
            /// The finalized state.
            /// </summary>
            Disposed,

            /// <summary>
            /// The request has failed (eg. asset cannot be loaded).
            /// </summary>
            Failed,
        };

        /// <summary>
        /// Gets the state.
        /// </summary>
        public States State { get; private set; } = States.Created;

        /// <summary>
        /// The item.
        /// </summary>
        public readonly AssetItem Item;

        /// <summary>
        /// The proxy object for the asset item.
        /// </summary>
        public readonly AssetProxy Proxy;

        /// <summary>
        /// The asset reference. May be null if not cached yet.
        /// </summary>
        public Asset Asset;

        /// <summary>
        /// The custom tag object used by the thumbnails rendering pipeline. Can be used to store the data related to the thumbnail rendering by the asset proxy.
        /// </summary>
        public object Tag;

        /// <summary>
        /// Immutable artifact version represented by this request.
        /// </summary>
        public readonly Guid CacheVersion;

        /// <summary>
        /// Whether this request represents a live forced regeneration.
        /// </summary>
        public readonly bool ForceRegenerate;

        private DateTime _nextThumbnailLoadAttemptUtc;
        private DateTime _publishedPreviewDeadlineUtc;
        private Guid _buildAssetId;
        private bool _proxyPrepared;

        /// <summary>
        /// Gets the failure reported by the artifact pipeline.
        /// </summary>
        public string FailureMessage { get; private set; }

        /// <summary>
        /// Determines whether thumbnail can be drawn for the item.
        /// </summary>
        public bool IsReady => State == States.Prepared && Asset && Asset.IsLoaded && Proxy.CanDrawThumbnail(this);

        /// <summary>
        /// Initializes a new instance of the <see cref="ThumbnailRequest"/> class.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="proxy">The proxy.</param>
        /// <param name="cacheVersion">The immutable artifact version represented by the thumbnail.</param>
        /// <param name="forceRegenerate">True to regenerate even when the published artifact version is unchanged.</param>
        public ThumbnailRequest(AssetItem item, AssetProxy proxy, Guid cacheVersion, bool forceRegenerate)
        {
            Item = item;
            Proxy = proxy;
            CacheVersion = cacheVersion;
            ForceRegenerate = forceRegenerate;
        }

        internal void Update()
        {
            if (State == States.Waiting && DateTime.UtcNow >= _nextThumbnailLoadAttemptUtc)
            {
                var currentVersion = AssetDatabaseQueryService.GetCurrentRuntimeArtifactCacheID(Item.ID);
                if (CacheVersion != Guid.Empty && currentVersion != Guid.Empty && currentVersion != CacheVersion)
                {
                    FailureMessage = "The requested runtime artifact was superseded before its thumbnail could be loaded.";
                    State = States.Failed;
                    return;
                }
                Asset = CacheVersion != Guid.Empty && currentVersion == CacheVersion ? AssetDatabaseQueryService.LoadAssetPreview(Item.ID) : null;
                if (Asset && CacheVersion != Guid.Empty)
                {
                    Proxy.OnThumbnailDrawPrepare(this);
                    _proxyPrepared = true;
                    State = States.Prepared;
                }
                else
                {
                    var status = AssetPipelineService.GetBuildStatus(_buildAssetId);
                    var terminalFailure = status == "Failed" || status == "Cancelled" || status == "NotBuilt";
                    if (status == "ReadyExact")
                    {
                        if (_publishedPreviewDeadlineUtc == default)
                            _publishedPreviewDeadlineUtc = DateTime.UtcNow + PublishedPreviewGrace;
                        terminalFailure = DateTime.UtcNow >= _publishedPreviewDeadlineUtc;
                    }
                    if (terminalFailure)
                    {
                        var diagnostic = AssetPipelineService.GetBuildDiagnostic(_buildAssetId);
                        FailureMessage = !string.IsNullOrEmpty(diagnostic.Message)
                            ? diagnostic.Message
                            : status == "ReadyExact"
                                ? "Runtime artifact is current but its preview could not be loaded."
                                : $"Runtime artifact build ended with status {status}.";
                        State = States.Failed;
                    }
                    else
                    {
                        _nextThumbnailLoadAttemptUtc = DateTime.UtcNow.AddMilliseconds(100);
                    }
                }
                return;
            }
            if (State == States.Prepared && (!Asset || Asset.LastLoadFailed))
            {
                State = States.Failed;
            }
        }

        /// <summary>
        /// Prepares this request.
        /// </summary>
        public void Prepare()
        {
            if (State != States.Created)
                throw new InvalidOperationException();
            if (Item.IsCanonicalSource)
            {
                var currentVersion = AssetDatabaseQueryService.GetCurrentRuntimeArtifactCacheID(Item.ID);
                if (CacheVersion != Guid.Empty && currentVersion != Guid.Empty && currentVersion != CacheVersion)
                {
                    FailureMessage = "The requested runtime artifact was superseded before thumbnail preparation.";
                    State = States.Failed;
                    return;
                }
                Asset = CacheVersion != Guid.Empty && currentVersion == CacheVersion ? AssetDatabaseQueryService.LoadAssetPreview(Item.ID) : null;
                if (!Asset || CacheVersion == Guid.Empty)
                {
                    BeginArtifactBuild();
                    return;
                }
            }
            else
            {
                Asset = Item.LoadAsync();
            }
            Proxy.OnThumbnailDrawPrepare(this);
            _proxyPrepared = true;
            State = States.Prepared;
        }

        private void BeginArtifactBuild()
        {
            _buildAssetId = AssetDatabaseQueryService.GetBuildOwnerID(Item.ID);
            if (_buildAssetId == Guid.Empty || AssetPipelineService.BuildAssetForeground(_buildAssetId))
            {
                var diagnostic = AssetPipelineService.GetBuildDiagnostic(_buildAssetId);
                FailureMessage = !string.IsNullOrEmpty(diagnostic.Message)
                    ? diagnostic.Message
                    : "Runtime artifact build request was rejected.";
                State = States.Failed;
                return;
            }

            _nextThumbnailLoadAttemptUtc = DateTime.UtcNow.AddMilliseconds(100);
            State = States.Waiting;
        }

        /// <summary>
        /// Finishes the rendering and updates the item thumbnail.
        /// </summary>
        /// <param name="icon">The icon.</param>
        public void FinishRender(ref SpriteHandle icon)
        {
            if (State != States.Prepared)
                throw new InvalidOperationException();
            Item.Thumbnail = icon;
            State = States.Rendered;
        }

        /// <summary>
        /// Finalizes this request.
        /// </summary>
        public void Dispose()
        {
            if (State == States.Disposed)
                throw new InvalidOperationException();

            try
            {
                if (_proxyPrepared)
                    Proxy.OnThumbnailDrawCleanup(this);
            }
            finally
            {
                Asset = null;
                Tag = null;
                State = States.Disposed;
            }
        }
    }
}
