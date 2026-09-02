// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using FlaxEditor.Modules;
using FlaxEngine;
using FlaxEngine.GUI;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Content.Thumbnails
{
    /// <summary>
    /// Manages asset thumbnails rendering and presentation.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class ThumbnailsModule : EditorModule, IContentItemOwner
    {
        private const int CacheVersion = 5;
        private const int PrepareChecksPerUpdate = 32;
        private const int ReadyChecksPerRender = 64;
        private const int MaxRendersPerFrame = 4;
        private const int MaxQueuedRequests = 256;
        private const int MaxPendingRequests = 256;
        private const int MaxFlushesPerInterval = 2;
        private const double RenderBudgetMilliseconds = 3.0;
        private static readonly TimeSpan FlushInterval = TimeSpan.FromSeconds(1);

        /// <summary>
        /// The minimum required quality (in range [0;1]) for content streaming resources to be loaded in order to generate thumbnail for them.
        /// </summary>
        public const float MinimumRequiredResourcesQuality = 0.8f;

        private readonly List<PreviewsCache> _cache = new List<PreviewsCache>(4);
        private readonly string _cacheFolder;
        private readonly List<ThumbnailRequest> _requests = new List<ThumbnailRequest>(128);
        private readonly HashSet<ContentItem> _pendingRequests = new HashSet<ContentItem>();
        private readonly HashSet<ContentItem> _pendingForcedRequests = new HashSet<ContentItem>();
        private readonly HashSet<ContentItem> _pendingHighPriorityRequests = new HashSet<ContentItem>();
        private readonly HashSet<ContentItem> _demandItems = new HashSet<ContentItem>();
        private readonly List<ContentItem> _expiredDemandItems = new List<ContentItem>();
        private readonly PreviewRoot _guiRoot = new PreviewRoot();
        private DateTime _lastFlushTime;
        private int _prepareCursor;
        private int _renderCursor;
        private int _flushCursor;
        private RenderTask _task;
        private GPUTexture _output;

        internal ThumbnailsModule(Editor editor)
        : base(editor)
        {
            _cacheFolder = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "Thumbnails", $"v{CacheVersion}");
            _lastFlushTime = DateTime.UtcNow;
        }

        /// <summary>
        /// Requests the item preview.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="forceRegenerate">Whether to regenerate while retaining the current pixels.</param>
        /// <param name="highPriority">Whether to move the request to the front of the queue.</param>
        /// <exception cref="System.ArgumentNullException"></exception>
        public void RequestPreview(ContentItem item, bool forceRegenerate = false, bool highPriority = false)
        {
            if (item == null)
                throw new ArgumentNullException();
            if (_task == null)
            {
                if (!_pendingRequests.Contains(item) && _pendingRequests.Count >= MaxPendingRequests)
                    EvictPendingRequest();
                _pendingRequests.Add(item);
                if (forceRegenerate)
                    _pendingForcedRequests.Add(item);
                if (highPriority)
                    _pendingHighPriorityRequests.Add(item);
                return;
            }

            // Check if use default icon
            var defaultThumbnail = item.DefaultThumbnail;
            if (defaultThumbnail.IsValid)
            {
                item.Thumbnail = defaultThumbnail;
                return;
            }

            // We cache previews only for items with 'ID', for now we support only AssetItems
            var assetItem = item as AssetItem;
            if (assetItem == null)
            {
                item.NotifyThumbnailRequestFailed();
                return;
            }

            // Ensure that there is valid proxy for that item
            var proxy = Editor.ContentDatabase.GetProxy(item) as AssetProxy;
            if (proxy == null)
            {
                Editor.LogWarning($"Cannot generate preview for item {item.Path}. Cannot find proxy for it.");
                item.NotifyThumbnailRequestFailed();
                return;
            }
            var cacheVersion = GetCacheVersion(assetItem);

            lock (_requests)
            {
                var existingRequest = FindRequest(assetItem);
                if (existingRequest != null)
                {
                    if (!forceRegenerate && (existingRequest.CacheVersion == cacheVersion || cacheVersion == Guid.Empty))
                    {
                        if (highPriority)
                            PrioritizeRequest(existingRequest);
                        return;
                    }
                    RemoveRequest(existingRequest);
                }

                SpriteHandle staleThumbnail = SpriteHandle.Invalid;
                for (int i = 0; i < _cache.Count; i++)
                {
                    if (!forceRegenerate && (!assetItem.IsCanonicalSource || cacheVersion != Guid.Empty))
                    {
                        var sprite = _cache[i].FindSlotVersioned(assetItem.ID, cacheVersion);
                        if (sprite.IsValid)
                        {
                            item.Thumbnail = sprite;
                            return;
                        }
                    }

                    if (!staleThumbnail.IsValid)
                        staleThumbnail = _cache[i].FindSlot(assetItem.ID);
                }

                if (!staleThumbnail.IsValid && forceRegenerate && item.Thumbnail.IsValid)
                    staleThumbnail = item.Thumbnail;
                if (staleThumbnail.IsValid)
                    item.SetStaleThumbnail(staleThumbnail);
                AddRequest(assetItem, proxy, cacheVersion, forceRegenerate, forceRegenerate || highPriority);
            }
        }

        private static Guid GetCacheVersion(AssetItem item)
        {
            if (!item.IsCanonicalSource)
                return Guid.Empty;
            return AssetDatabaseQueryService.GetCurrentRuntimeArtifactCacheID(item.ID);
        }

        internal void TrackThumbnailDemand(ContentItem item)
        {
            _demandItems.Add(item);
        }

        private void EvictPendingRequest()
        {
            ContentItem evicted = null;
            foreach (var item in _pendingRequests)
            {
                evicted = item;
                break;
            }
            if (evicted == null)
                return;
            _pendingRequests.Remove(evicted);
            _pendingForcedRequests.Remove(evicted);
            _pendingHighPriorityRequests.Remove(evicted);
            evicted.NotifyThumbnailRequestCancelled();
        }

        /// <summary>
        /// Cancels queued work after the last visible consumer releases an item.
        /// </summary>
        /// <param name="item">The item.</param>
        internal void CancelPreviewRequest(ContentItem item)
        {
            _pendingRequests.Remove(item);
            _pendingForcedRequests.Remove(item);
            _pendingHighPriorityRequests.Remove(item);
            if (item is AssetItem assetItem)
            {
                lock (_requests)
                    RemoveRequest(assetItem);
            }
        }

        /// <summary>
        /// Deletes the item preview from the cache.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <exception cref="System.ArgumentNullException"></exception>
        public void DeletePreview(ContentItem item)
        {
            if (item == null)
                throw new ArgumentNullException();

            // We cache previews only for items with 'ID', for now we support only AssetItems
            var assetItem = item as AssetItem;
            if (assetItem == null)
                return;

            _pendingRequests.Remove(item);
            _pendingForcedRequests.Remove(item);
            _pendingHighPriorityRequests.Remove(item);
            DeletePreview(assetItem.ID);
            item.Thumbnail = SpriteHandle.Invalid;
        }

        /// <summary>
        /// Deletes an asset preview from the cache by identity.
        /// </summary>
        /// <param name="assetId">The asset identity.</param>
        public void DeletePreview(Guid assetId)
        {
            _pendingRequests.RemoveWhere(item => item is AssetItem assetItem && assetItem.ID == assetId);
            _pendingForcedRequests.RemoveWhere(item => item is AssetItem assetItem && assetItem.ID == assetId);
            _pendingHighPriorityRequests.RemoveWhere(item => item is AssetItem assetItem && assetItem.ID == assetId);
            lock (_requests)
            {
                // Cancel loading
                for (int i = _requests.Count - 1; i >= 0; i--)
                {
                    if (_requests[i].Item.ID == assetId)
                        RemoveRequest(_requests[i]);
                }

                // Find atlas with preview and remove it
                for (int i = 0; i < _cache.Count; i++)
                {
                    if (_cache[i].ReleaseSlot(assetId))
                        break;
                }
            }
        }

        internal static bool HasMinimumQuality(TextureBase asset)
        {
            // Don't block thumbnails queue when texture fails to stream in (eg. unsupported format)
            if (asset.HasStreamingError)
                return true;

            // Check if enough mip levels are loaded
            var mipLevels = asset.MipLevels;
            var minMipLevels = Mathf.Min(mipLevels, 7);
            if (asset.IsLoaded && asset.ResidentMipLevels >= Mathf.Max(minMipLevels, (int)(mipLevels * MinimumRequiredResourcesQuality)))
                return true;

            // Inform streaming about resource usage to stream it in for the thumbnail
            asset.SetStreamingVisible();

            return false;
        }

        internal static bool HasMinimumQuality(ModelBase asset)
        {
            if (!asset.IsLoaded)
                return false;
            var lods = asset.LODsCount;
            var slots = asset.MaterialSlots;

            // Check if all materials are loaded (incl. dependent resources)
            foreach (var slot in slots)
            {
                if (slot.Material && !HasMinimumQuality(slot.Material))
                    return false;
            }

            if (lods == 0)
                return true;

            // Check if enough LODs are loaded
            if (asset.LoadedLODs >= Mathf.Max(1, (int)(lods * MinimumRequiredResourcesQuality)))
                return true;

            // TODO: impl SetStreamingVisible for models similar to textures (ModelsStreamingHandler needs to use it)

            return false;
        }

        internal static bool HasMinimumQuality(MaterialBase asset)
        {
            if (asset is MaterialInstance asInstance)
                return HasMinimumQuality(asInstance);
            return HasMinimumQualityInternal(asset);
        }

        internal static bool HasMinimumQuality(Material asset)
        {
            return HasMinimumQualityInternal(asset);
        }

        internal static bool HasMinimumQuality(MaterialInstance asset)
        {
            if (!HasMinimumQualityInternal(asset))
                return false;
            var baseMaterial = asset.BaseMaterial;
            return baseMaterial == null || HasMinimumQualityInternal(baseMaterial);
        }

        internal static bool HasMinimumQuality(Prefab asset)
        {
            if (!asset.IsLoaded)
                return false;
            var defaultInstance = asset.GetDefaultInstance();
            return defaultInstance == null || HasMinimumQualityInternal(defaultInstance);
        }

        private static bool HasMinimumQualityInternal(MaterialBase asset)
        {
            if (!asset.IsLoaded)
                return false;
            var parameters = asset.Parameters;
            foreach (var parameter in parameters)
            {
                if (parameter.Value is TextureBase asTexture && !HasMinimumQuality(asTexture))
                    return false;
            }
            return true;
        }

        private static bool HasMinimumQualityInternal(Actor actor)
        {
            if (!actor.IsActive)
                return true;

            if (actor is ModelInstanceActor modelInstance)
            {
                var model = modelInstance.GetModel();
                if (model && !HasMinimumQuality(model))
                    return false;
                var slots = modelInstance.MaterialSlots;
                foreach (var slot in slots)
                {
                    if (slot.Material && !HasMinimumQuality(slot.Material))
                        return false;
                }
            }
            if (actor is SpriteRender spriteRender)
            {
                if (spriteRender.Material && !HasMinimumQuality(spriteRender.Material))
                    return false;
            }
            if (actor is TextRender textRender)
            {
                if (textRender.Material && !HasMinimumQuality(textRender.Material))
                    return false;
            }

            for (int i = 0; i < actor.ChildrenCount; i++)
            {
                if (!HasMinimumQualityInternal(actor.GetChild(i)))
                    return false;
            }

            return true;
        }

        #region IContentItemOwner

        /// <inheritdoc />
        void IContentItemOwner.OnItemDeleted(ContentItem item)
        {
            _demandItems.Remove(item);
            DeletePreview(item);
        }

        /// <inheritdoc />
        void IContentItemOwner.OnItemRenamed(ContentItem item)
        {
        }

        /// <inheritdoc />
        void IContentItemOwner.OnItemReimported(ContentItem item)
        {
        }

        /// <inheritdoc />
        void IContentItemOwner.OnItemDispose(ContentItem item)
        {
            _demandItems.Remove(item);
            if (item is AssetItem assetItem)
            {
                lock (_requests)
                {
                    RemoveRequest(assetItem);
                }
            }
        }

        #endregion

        /// <inheritdoc />
        public override void OnInit()
        {
            var gpuDevice = GPUDevice.Instance;
            if (Editor.IsHeadlessMode || gpuDevice == null || gpuDevice.RendererType == RendererType.Null)
                return;

            // Create cache folder
            if (!Directory.Exists(_cacheFolder))
                Directory.CreateDirectory(_cacheFolder);

            // Find atlases in a Editor cache directory
            var files = Directory.GetFiles(_cacheFolder, "cache_*.flax", SearchOption.TopDirectoryOnly);
            int atlases = 0;
            for (int i = 0; i < files.Length; i++)
            {
                // Load asset
                var asset = FlaxEngine.Content.LoadAsync(files[i]);
                if (asset == null)
                    continue;

                // Validate type
                if (asset is PreviewsCache atlas)
                {
                    // Cache atlas
                    atlases++;
                    _cache.Add(atlas);
                }
                else
                {
                    // Skip asset
                    Editor.LogWarning(string.Format("Asset \'{0}\' is inside Editor\'s private directory for Assets Thumbnails Cache. Please move it.", asset.Path));
                }
            }
            Editor.Log(string.Format("Previews cache count: {0} (capacity for {1} icons)", atlases, atlases * PreviewsCache.AssetIconsPerAtlas));

            // Prepare at least one atlas
            if (_cache.Count == 0)
            {
                GetValidAtlas();
            }

            // Create render task but disabled for now
            _output = gpuDevice.CreateTexture("ThumbnailsOutput");
            var desc = GPUTextureDescription.New2D(PreviewsCache.AssetIconSize, PreviewsCache.AssetIconSize, PreviewsCache.AssetIconsAtlasFormat);
            _output.Init(ref desc);
            _task = Object.New<RenderTask>();
            _task.Order = 50; // Render this task later
            _task.Enabled = false;
            _task.Render += OnRender;

            Editor.Undo.UndoDone += OnUndoRedo;
            Editor.Undo.RedoDone += OnUndoRedo;

            if (_pendingRequests.Count != 0)
            {
                var pendingRequests = new List<ContentItem>(_pendingRequests);
                _pendingRequests.Clear();
                for (int i = 0; i < pendingRequests.Count; i++)
                {
                    if (!pendingRequests[i].IsDisposing)
                        RequestPreview(pendingRequests[i], _pendingForcedRequests.Remove(pendingRequests[i]),
                            _pendingHighPriorityRequests.Remove(pendingRequests[i]));
                }
                _pendingForcedRequests.Clear();
                _pendingHighPriorityRequests.Clear();
            }
        }

        private void OnUndoRedo(IUndoAction action)
        {
            if (UndoActionMetadata.DoesNotModifyData(action))
                return;

            var info = UndoActionMetadata.GetActionInfo(action);
            AssetItem item = null;
            if (info.OwnerId != Guid.Empty)
                item = Editor.ContentDatabase.FindLoadedAsset(info.OwnerId);
            else if (info.TargetType == UndoActionTargetType.Asset && info.TargetId != Guid.Empty)
                item = Editor.ContentDatabase.FindLoadedAsset(info.TargetId);

            var path = !string.IsNullOrEmpty(info.OwnerPath) ? info.OwnerPath : info.TargetPath;
            if (item == null && !string.IsNullOrEmpty(path))
                item = Editor.ContentDatabase.FindLoadedAsset(path);

            if (item?.HasThumbnailReference == true)
                item.RefreshThumbnail();
        }

        private void OnRender(RenderTask task, GPUContext context)
        {
            lock (_requests)
            {
                var stopwatch = Stopwatch.StartNew();
                for (int rendered = 0; rendered < MaxRendersPerFrame; rendered++)
                {
                    var request = GetReadyRequest(ReadyChecksPerRender);
                    if (request == null)
                    {
                        _task.Enabled = false;
                        break;
                    }

                    if (!RenderPreview(request, context) || stopwatch.Elapsed.TotalMilliseconds >= RenderBudgetMilliseconds)
                        break;
                }
            }
        }

        private bool RenderPreview(ThumbnailRequest request, GPUContext context)
        {
            // Find atlas with a free slot
            var atlas = GetValidAtlas(request.Item.ID);
            if (atlas == null)
            {
                _task.Enabled = false;
                ClearRequests();
                Editor.LogError("Failed to get atlas.");
                return false;
            }

            // Wait for atlas being loaded
            if (!atlas.IsReady || atlas.IsFlushing)
                return false;

            try
            {
                // Setup
                _guiRoot.RemoveChildren();
                _guiRoot.AccentColor = request.Proxy.AccentColor;

                // Call proxy to prepare for thumbnail rendering
                request.Proxy.OnThumbnailDrawBegin(request, _guiRoot, context);
                _guiRoot.UnlockChildrenRecursive();

                // Draw preview
                context.Clear(_output.View(), Color.Black);
                Render2D.CallDrawing(_guiRoot, context, _output);

                // Call proxy and cleanup UI (delete create controls, shared controls should be unlinked during OnThumbnailDrawEnd event)
                request.Proxy.OnThumbnailDrawEnd(request, _guiRoot);
            }
            catch (Exception ex)
            {
                // Handle internal errors gracefully (eg. when asset is corrupted and proxy fails)
                Editor.LogError("Failed to render thumbnail icon for asset: " + request.Item);
                Editor.LogWarning(ex);
                request.FinishRender(ref SpriteHandle.Invalid);
                request.Item.NotifyThumbnailRequestFailed(request.ForceRegenerate);
                RemoveRequest(request);
                return true;
            }
            finally
            {
                _guiRoot.DisposeChildren();
            }

            // Copy backbuffer with rendered preview into atlas
            SpriteHandle icon = atlas.OccupySlotVersioned(_output, request.Item.ID, request.CacheVersion);
            if (!icon.IsValid)
            {
                _task.Enabled = false;
                ClearRequests();
                Editor.LogError("Failed to occupy previews cache atlas slot.");
                return false;
            }

            request.FinishRender(ref icon);
            RemoveRequest(request);
            return true;
        }

        private void StartPreviewsQueue()
        {
            // Ensure to have valid atlas
            GetValidAtlas();

            // Enable task
            _task.Enabled = true;
        }

        #region Requests Management

        private ThumbnailRequest FindRequest(AssetItem item)
        {
            for (int i = 0; i < _requests.Count; i++)
            {
                if (_requests[i].Item == item)
                    return _requests[i];
            }
            return null;
        }

        private void AddRequest(AssetItem item, AssetProxy proxy, Guid cacheVersion, bool forceRegenerate, bool highPriority)
        {
            if (_requests.Count >= MaxQueuedRequests)
            {
                var evicted = _requests[_requests.Count - 1].Item;
                RemoveRequest(_requests[_requests.Count - 1]);
                evicted.NotifyThumbnailRequestCancelled();
            }
            var request = new ThumbnailRequest(item, proxy, cacheVersion, forceRegenerate);
            if (highPriority)
            {
                _requests.Insert(0, request);
                _prepareCursor = 0;
                _renderCursor = 0;
            }
            else
            {
                _requests.Add(request);
            }
            item.AddReference(this, false);
        }

        private void PrioritizeRequest(ThumbnailRequest request)
        {
            var index = _requests.IndexOf(request);
            if (index <= 0)
                return;
            _requests.RemoveAt(index);
            _requests.Insert(0, request);
            _prepareCursor = 0;
            _renderCursor = 0;
        }

        private void RemoveRequest(ThumbnailRequest request)
        {
            var index = _requests.IndexOf(request);
            if (index == -1)
                return;
            try
            {
                request.Dispose();
            }
            finally
            {
                _requests.RemoveAt(index);
                AdjustCursorAfterRemove(ref _prepareCursor, index);
                AdjustCursorAfterRemove(ref _renderCursor, index);
                request.Item.RemoveReference(this);
            }
        }

        private void ClearRequests()
        {
            while (_requests.Count > 0)
                RemoveRequest(_requests[_requests.Count - 1]);
        }

        private void AdjustCursorAfterRemove(ref int cursor, int removedIndex)
        {
            if (removedIndex < cursor)
                cursor--;
            if (cursor >= _requests.Count)
                cursor = 0;
        }

        private void RemoveRequest(AssetItem item)
        {
            var request = FindRequest(item);
            if (request != null)
                RemoveRequest(request);
        }

        private ThumbnailRequest GetReadyRequest(int maxChecks)
        {
            maxChecks = Mathf.Min(maxChecks, _requests.Count);
            for (int checkedCount = 0; checkedCount < maxChecks && _requests.Count != 0; checkedCount++)
            {
                if (_renderCursor >= _requests.Count)
                    _renderCursor = 0;
                var request = _requests[_renderCursor];
                _renderCursor++;
                try
                {
                    if (request.IsReady)
                        return request;
                }
                catch (Exception ex)
                {
                    Editor.LogWarning($"Failed to prepare thumbnail rendering for {request.Item.ShortName}.");
                    Editor.LogWarning(ex);
                    request.Item.NotifyThumbnailRequestFailed(request.ForceRegenerate);
                    RemoveRequest(request);
                }
            }

            return null;
        }

        #endregion

        #region Atlas Management

        private PreviewsCache CreateAtlas()
        {
            // Create atlas path
            var path = StringUtils.CombinePaths(_cacheFolder, string.Format("cache_{0:N}.flax", Guid.NewGuid()));

            // Create atlas
            if (PreviewsCache.Create(path))
            {
                Editor.LogError("Failed to create thumbnails atlas.");
                return null;
            }

            // Load atlas
            var atlas = FlaxEngine.Content.LoadAsync<PreviewsCache>(path);
            if (atlas == null)
            {
                Editor.LogError("Failed to load thumbnails atlas.");
                return null;
            }

            // Register new atlas
            _cache.Add(atlas);

            return atlas;
        }

        private void Flush(bool all = false)
        {
            int flushed = 0;
            int checks = _cache.Count;
            int limit = all ? _cache.Count : MaxFlushesPerInterval;
            while (checks-- > 0 && flushed < limit)
            {
                if (_flushCursor >= _cache.Count)
                    _flushCursor = 0;
                var atlas = _cache[_flushCursor++];
                if (!atlas.IsDirty || atlas.IsFlushing)
                    continue;
                atlas.Flush();
                flushed++;
            }
        }

        private bool HasAllAtlasesLoaded()
        {
            bool allLoaded = true;
            for (int i = _cache.Count - 1; i >= 0; i--)
            {
                var atlas = _cache[i];
                if (atlas.LastLoadFailed)
                {
                    Editor.LogWarning($"Discarding failed thumbnail cache atlas '{atlas.Path}'.");
                    _cache.RemoveAt(i);
                    continue;
                }
                allLoaded &= atlas.IsReady;
            }
            if (_cache.Count == 0)
            {
                var atlas = CreateAtlas();
                return atlas != null && atlas.IsReady;
            }
            return allLoaded;
        }

        private PreviewsCache GetValidAtlas()
        {
            for (int i = 0; i < _cache.Count; i++)
            {
                // A newly created atlas has no slot metadata until it finishes loading.
                if (!_cache[i].IsReady || _cache[i].HasFreeSlot)
                {
                    return _cache[i];
                }
            }

            // Create new atlas
            return CreateAtlas();
        }

        private PreviewsCache GetValidAtlas(Guid assetId)
        {
            // Reuse the existing slot so its sprite stays valid during regeneration.
            for (int i = 0; i < _cache.Count; i++)
            {
                if (_cache[i].FindSlot(assetId).IsValid)
                    return _cache[i];
            }
            return GetValidAtlas();
        }

        #endregion

        /// <inheritdoc />
        public override void OnUpdate()
        {
            _expiredDemandItems.Clear();
            var frame = FlaxEngine.Engine.FrameCount;
            foreach (var item in _demandItems)
            {
                if (item.IsDisposing || !item.ExpireThumbnailDemand(frame))
                    _expiredDemandItems.Add(item);
            }
            for (int i = 0; i < _expiredDemandItems.Count; i++)
                _demandItems.Remove(_expiredDemandItems[i]);

            // Wait some frames before start generating previews (late init feature)
            if (_task == null || Time.TimeSinceStartup < 1.0f || HasAllAtlasesLoaded() == false)
                return;

            lock (_requests)
            {
                var now = DateTime.UtcNow;

                // Prepare a moving window so slow assets cannot block later requests.
                int checks = Mathf.Min(PrepareChecksPerUpdate, _requests.Count);
                for (int checkedCount = 0; checkedCount < checks && _requests.Count != 0; checkedCount++)
                {
                    if (_prepareCursor >= _requests.Count)
                        _prepareCursor = 0;
                    var request = _requests[_prepareCursor];
                    var removed = false;
                    try
                    {
                        request.Update();
                        if (request.State == ThumbnailRequest.States.Created)
                        {
                            request.Prepare();
                        }
                        else if (request.State == ThumbnailRequest.States.Failed)
                        {
                            Editor.LogWarning($"Failed to generate thumbnail for '{request.Item.Path}': {request.FailureMessage ?? "asset could not be loaded"}");
                            request.Item.NotifyThumbnailRequestFailed(request.ForceRegenerate);
                            RemoveRequest(request);
                            removed = true;
                        }
                    }
                    catch (Exception ex)
                    {
                        Editor.LogWarning($"Failed to prepare thumbnail rendering for {request.Item.ShortName}.");
                        Editor.LogWarning(ex);
                        request.Item.NotifyThumbnailRequestFailed(request.ForceRegenerate);
                        RemoveRequest(request);
                        removed = true;
                    }

                    if (!removed)
                        _prepareCursor++;
                }

                if (_requests.Count > 0 && !_task.Enabled)
                    StartPreviewsQueue();

                // Persist completed work even while other thumbnails are still pending.
                if (now - _lastFlushTime >= FlushInterval)
                {
                    _lastFlushTime = now;
                    Flush();
                }
            }
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            Editor.Undo.UndoDone -= OnUndoRedo;
            Editor.Undo.RedoDone -= OnUndoRedo;

            if (_task)
                _task.Enabled = false;

            lock (_requests)
            {
                Flush(true);
                ClearRequests();
                _cache.Clear();
                _pendingRequests.Clear();
                _pendingForcedRequests.Clear();
                _pendingHighPriorityRequests.Clear();
                _demandItems.Clear();
                _expiredDemandItems.Clear();
            }

            _guiRoot.Dispose();
            Object.Destroy(ref _task);
            Object.Destroy(ref _output);
        }

        /// <summary>
        /// Thumbnails GUI root control.
        /// </summary>
        /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
        private class PreviewRoot : ContainerControl
        {
            /// <summary>
            /// The item accent color to draw.
            /// </summary>
            public Color AccentColor;

            /// <inheritdoc />
            public PreviewRoot()
            : base(0, 0, PreviewsCache.AssetIconSize, PreviewsCache.AssetIconSize)
            {
                AutoFocus = false;
                AccentColor = Color.Pink;
                IsLayoutLocked = false;
            }

            /// <inheritdoc />
            public override void Draw()
            {
                base.Draw();

                // Draw accent
                const float accentHeight = 2;
                Render2D.FillRectangle(new Rectangle(0, Height - accentHeight, Width, accentHeight), AccentColor);
            }
        }
    }
}
