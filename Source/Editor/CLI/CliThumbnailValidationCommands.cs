// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using FlaxEditor.Content;
using FlaxEditor.Content.Documents;
using FlaxEditor.Surface;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor
{
    /// <summary>
    /// Renderer-backed thumbnail lifecycle validation commands.
    /// </summary>
    public static class CliThumbnailValidationCommands
    {
        /// <summary>
        /// Validates graph edit, save publication, thumbnail rendering, and persistent cache reload.
        /// </summary>
        [CliCommand("assets.thumbnails.validate-lifecycle", Description = "Validate rendered graph thumbnails across edit, save publication, and cache reload.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation ValidateLifecycle(CliCommandContext context)
        {
            return new ThumbnailLifecycleOperation(context);
        }

        private sealed class SurfaceOwner : IVisjectSurfaceOwner
        {
            public Asset SurfaceAsset => null;
            public string SurfaceName => "Thumbnail lifecycle validation";
            public Undo Undo => null;
            public byte[] SurfaceData { get; set; }
            public VisjectSurfaceContext ParentContext => null;
            public void OnContextCreated(VisjectSurfaceContext context) { }
            public void OnSurfaceEditedChanged() { }
            public void OnSurfaceGraphEdited() { }
            public void OnSurfaceClose() { }
        }

        private sealed class ThumbnailLifecycleOperation : CliCommandOperation, IContentItemOwner
        {
            private enum Stage
            {
                Create,
                WaitForCreatedArtifacts,
                ConfigureParticle,
                WaitForParticleArtifact,
                WaitForInitialParticleThumbnail,
                EditParticle,
                WaitForEditedParticleThumbnail,
                SaveParticle,
                WaitForSavedParticleThumbnail,
                WaitForParticleCacheFlush,
                WaitForParticleCacheReload,
                RequestMaterialThumbnail,
                WaitForInitialMaterialThumbnail,
                SaveMaterial,
                WaitForSavedMaterialThumbnail,
            }

            private readonly CliCommandContext _context;
            private readonly DateTime _deadline = DateTime.UtcNow.AddMinutes(5);
            private readonly string _root;
            private readonly string _particlePath;
            private readonly string _materialPath;
            private Stage _stage;
            private CliCommandResult _result;
            private Guid _particleId;
            private Guid _materialId;
            private Guid _particleVersion;
            private Guid _materialVersion;
            private AssetItem _particleItem;
            private AssetItem _materialItem;
            private AssetDocumentSession _particleSession;
            private AssetDocumentSession _materialSession;
            private ParticleEmitterProxy _particleProxy;
            private PreviewsCache _particleCache;
            private ulong _particleHash;
            private int _particlePixels;
            private int _materialPixels;
            private int _particleRenderCount;
            private bool _particleReferenced;
            private bool _materialReferenced;

            public ThumbnailLifecycleOperation(CliCommandContext context)
            {
                _context = context;
                _root = Path.Combine(Globals.ProjectContentFolder, "__ThumbnailLifecycle_" + Guid.NewGuid().ToString("N"));
                _particlePath = Path.Combine(_root, "Emitter.particleemitter");
                _materialPath = Path.Combine(_root, "Material.material");
            }

            public override bool IsCompleted => _result != null;
            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (_result != null)
                    return;
                try
                {
                    if (_context.CancellationToken.IsCancellationRequested)
                        throw new OperationCanceledException();
                    if (DateTime.UtcNow >= _deadline)
                        throw new TimeoutException("Thumbnail lifecycle validation exceeded five minutes during " + _stage + ".");
                    RenewDemand();
                    UpdateStage();
                }
                catch (Exception ex)
                {
                    Complete(CliCommandResult.Failure("FLX-ASSET-THUMBNAIL-LIFECYCLE-0006", ex.Message,
                        new { stage = _stage.ToString(), temporaryPath = _root }));
                }
            }

            public override void Cancel()
            {
                Complete(CliCommandResult.Failure("FLX-ASSET-THUMBNAIL-LIFECYCLE-0006", "Thumbnail lifecycle validation was cancelled."));
            }

            private void UpdateStage()
            {
                switch (_stage)
                {
                case Stage.Create:
                    CreateAssets();
                    break;
                case Stage.WaitForCreatedArtifacts:
                    WaitForCreatedArtifacts();
                    break;
                case Stage.ConfigureParticle:
                    ConfigureParticle();
                    break;
                case Stage.WaitForParticleArtifact:
                    WaitForParticleArtifact();
                    break;
                case Stage.WaitForInitialParticleThumbnail:
                    WaitForInitialParticleThumbnail();
                    break;
                case Stage.EditParticle:
                    EditParticle();
                    break;
                case Stage.WaitForEditedParticleThumbnail:
                    WaitForEditedParticleThumbnail();
                    break;
                case Stage.SaveParticle:
                    SaveParticle();
                    break;
                case Stage.WaitForSavedParticleThumbnail:
                    WaitForSavedParticleThumbnail();
                    break;
                case Stage.WaitForParticleCacheFlush:
                    WaitForParticleCacheFlush();
                    break;
                case Stage.WaitForParticleCacheReload:
                    WaitForParticleCacheReload();
                    break;
                case Stage.RequestMaterialThumbnail:
                    RequestMaterialThumbnail();
                    break;
                case Stage.WaitForInitialMaterialThumbnail:
                    WaitForInitialMaterialThumbnail();
                    break;
                case Stage.SaveMaterial:
                    SaveMaterial();
                    break;
                case Stage.WaitForSavedMaterialThumbnail:
                    WaitForSavedMaterialThumbnail();
                    break;
                }
            }

            private void CreateAssets()
            {
                var device = GPUDevice.Instance;
                if (Editor.Instance.IsHeadlessMode || device == null || device.RendererType == RendererType.Null)
                    throw new InvalidOperationException("Run this command in a non-headless FlaxEditor with a real graphics renderer.");
                Directory.CreateDirectory(_root);
                _particleId = AssetDocumentRegistry.CreateGraph(_particlePath, typeof(ParticleEmitter).FullName);
                _materialId = AssetDocumentRegistry.CreateGraph(_materialPath, typeof(Material).FullName);
                if (_particleId == Guid.Empty || _materialId == Guid.Empty)
                    throw new InvalidOperationException("Failed to create temporary graph assets.");
                if (AssetPipelineService.RefreshSources(new[] { _particlePath, _materialPath }))
                    throw new InvalidOperationException("Failed to register temporary graph assets.");
                if (AssetPipelineService.BuildAsset(_particleId) || AssetPipelineService.BuildAsset(_materialId))
                    throw new InvalidOperationException("Failed to request temporary graph artifacts.");
                _context.ReportProgress("Building temporary graph artifacts", 0.05f);
                _stage = Stage.WaitForCreatedArtifacts;
            }

            private void WaitForCreatedArtifacts()
            {
                if (!ArtifactsReady(_particleId, _materialId))
                    return;
                _particleItem = Editor.Instance.ContentDatabase.FindAsset(_particleId)
                                ?? throw new InvalidOperationException("Temporary particle item was not registered.");
                _materialItem = Editor.Instance.ContentDatabase.FindAsset(_materialId)
                                ?? throw new InvalidOperationException("Temporary material item was not registered.");
                _particleProxy = Editor.Instance.ContentDatabase.GetProxy(_particleItem) as ParticleEmitterProxy
                                 ?? throw new InvalidOperationException("Particle thumbnail proxy was not registered.");
                if (AssetDocumentRegistry.OpenGraph<ParticleEmitter>(_particleItem, out _particleSession) == null ||
                    AssetDocumentRegistry.OpenGraph<Material>(_materialItem, out _materialSession) == null)
                    throw new InvalidOperationException("Temporary graph assets failed to open.");
                _stage = Stage.ConfigureParticle;
            }

            private void ConfigureParticle()
            {
                var template = FlaxEngine.Content.LoadAsyncInternal<ParticleEmitter>("Editor/Particles/Constant Burst");
                if (template == null || template.WaitForLoaded())
                    throw new InvalidOperationException("Particle thumbnail template failed to load.");
                _particleSession.SetGraphSurface(template.LoadSurface(false));
                if (_particleSession.SaveGraph(_particleItem))
                    throw new InvalidOperationException("Failed to save the initial particle graph.");
                _particleVersion = RequireCurrentVersion(_particleId);
                _context.ReportProgress("Waiting for initial particle publication", 0.15f);
                _stage = Stage.WaitForParticleArtifact;
            }

            private void WaitForParticleArtifact()
            {
                if (!LoadedExact(_particleId, _particleVersion))
                    return;
                _particleItem.Thumbnail = SpriteHandle.Invalid;
                _particleItem.AddReference(this, true);
                _particleReferenced = true;
                _stage = Stage.WaitForInitialParticleThumbnail;
            }

            private void WaitForInitialParticleThumbnail()
            {
                if (!TryGetExactThumbnail(_particleItem, _particleVersion, out var thumbnail))
                    return;
                AssertParticleThumbnail(thumbnail, "initial particle thumbnail");
                _particleRenderCount = _particleProxy.ThumbnailRenderCount;
                _context.ReportProgress("Rendered initial particle thumbnail", 0.3f);
                _stage = Stage.EditParticle;
            }

            private void EditParticle()
            {
                _particleSession.SetGraphSurface(MoveParticleGraphNode(_particleSession.GetGraphSurface(), new Float2(11, 7)));
                _particleItem.RefreshThumbnail();
                _stage = Stage.WaitForEditedParticleThumbnail;
            }

            private void WaitForEditedParticleThumbnail()
            {
                if (_particleProxy.ThumbnailRenderCount <= _particleRenderCount ||
                    !TryGetExactThumbnail(_particleItem, _particleVersion, out var thumbnail))
                    return;
                AssertParticleThumbnail(thumbnail, "edited particle thumbnail");
                _particleRenderCount = _particleProxy.ThumbnailRenderCount;
                _context.ReportProgress("Rendered edited particle thumbnail", 0.45f);
                _stage = Stage.SaveParticle;
            }

            private void SaveParticle()
            {
                if (_particleSession.SaveGraph(_particleItem))
                    throw new InvalidOperationException("Failed to save the edited particle graph.");
                var publishedVersion = RequireCurrentVersion(_particleId);
                if (publishedVersion == _particleVersion)
                    throw new InvalidOperationException("Particle graph edit did not produce a new artifact digest.");
                if (LoadedExact(_particleId, publishedVersion))
                    throw new InvalidOperationException("Particle save applied its deferred hot-swap before publication ordering could be observed.");
                _particleVersion = publishedVersion;
                _stage = Stage.WaitForSavedParticleThumbnail;
            }

            private void WaitForSavedParticleThumbnail()
            {
                if (!LoadedExact(_particleId, _particleVersion) ||
                    !TryGetExactThumbnail(_particleItem, _particleVersion, out var thumbnail))
                    return;
                AssertParticleThumbnail(thumbnail, "saved particle thumbnail");
                _particlePixels = CountVisiblePixels(thumbnail);
                _particleHash = GetPixelHash(thumbnail);
                _particleCache = (PreviewsCache)thumbnail.Atlas;
                _particleItem.RemoveReference(this);
                _particleReferenced = false;
                _particleCache.Flush();
                _context.ReportProgress("Flushing saved particle thumbnail", 0.65f);
                _stage = Stage.WaitForParticleCacheFlush;
            }

            private void WaitForParticleCacheFlush()
            {
                if (_particleCache.IsDirty || _particleCache.IsFlushing)
                    return;
                _particleCache.Reload();
                _stage = Stage.WaitForParticleCacheReload;
            }

            private void WaitForParticleCacheReload()
            {
                if (_particleCache.LastLoadFailed)
                    throw new InvalidOperationException("Persisted thumbnail cache failed to reload.");
                if (!_particleCache.IsLoaded)
                    return;
                var thumbnail = _particleCache.FindSlotVersioned(_particleId, _particleVersion);
                if (!thumbnail.IsValid)
                    throw new InvalidOperationException("Reloaded cache has no exact saved-particle thumbnail.");
                if (CountVisiblePixels(thumbnail) == 0 || GetPixelHash(thumbnail) != _particleHash)
                    throw new InvalidOperationException("Reloaded cache did not preserve the saved particle pixels.");
                _stage = Stage.RequestMaterialThumbnail;
            }

            private void RequestMaterialThumbnail()
            {
                _materialVersion = RequireCurrentVersion(_materialId);
                _materialItem.Thumbnail = SpriteHandle.Invalid;
                _materialItem.AddReference(this, true);
                _materialReferenced = true;
                _stage = Stage.WaitForInitialMaterialThumbnail;
            }

            private void WaitForInitialMaterialThumbnail()
            {
                if (!TryGetExactThumbnail(_materialItem, _materialVersion, out var thumbnail))
                    return;
                if (CountVisiblePixels(thumbnail) == 0)
                    throw new InvalidOperationException("Initial material thumbnail contains no visible pixels.");
                _context.ReportProgress("Rendered initial material thumbnail", 0.8f);
                _stage = Stage.SaveMaterial;
            }

            private void SaveMaterial()
            {
                _materialSession.SetGraphSurface(MoveMaterialGraphNode(_materialSession.GetGraphSurface(), new Float2(9, 4)));
                if (_materialSession.SaveGraph(_materialItem))
                    throw new InvalidOperationException("Failed to save the edited material graph.");
                var publishedVersion = RequireCurrentVersion(_materialId);
                if (publishedVersion == _materialVersion)
                    throw new InvalidOperationException("Material graph edit did not produce a new artifact digest.");
                _materialVersion = publishedVersion;
                _stage = Stage.WaitForSavedMaterialThumbnail;
            }

            private void WaitForSavedMaterialThumbnail()
            {
                if (!LoadedExact(_materialId, _materialVersion) ||
                    !TryGetExactThumbnail(_materialItem, _materialVersion, out var thumbnail))
                    return;
                _materialPixels = CountVisiblePixels(thumbnail);
                if (_materialPixels == 0)
                    throw new InvalidOperationException("Saved material thumbnail contains no visible pixels.");
                _context.ReportProgress("Validated saved graph thumbnails", 1.0f);
                Complete(CliCommandResult.Success(new
                {
                    particle = new { version = _particleVersion, visiblePixels = _particlePixels, pixelHash = _particleHash.ToString("x16"), cacheReloaded = true },
                    material = new { version = _materialVersion, visiblePixels = _materialPixels },
                }));
            }

            private void RenewDemand()
            {
                if (_particleReferenced)
                    _particleItem.RequestThumbnail(this);
                if (_materialReferenced)
                    _materialItem.RequestThumbnail(this);
            }

            private static bool ArtifactsReady(params Guid[] ids)
            {
                foreach (var id in ids)
                {
                    var status = AssetPipelineService.GetBuildStatus(id);
                    if (status == "Failed" || status == "Cancelled" || status == "NotBuilt")
                        throw new InvalidOperationException("Artifact build ended as " + status + " for " + id + ": " + AssetPipelineService.GetBuildDiagnostic(id).Message);
                    if (!AssetPipelineService.IsArtifactCurrent(id))
                        return false;
                }
                return true;
            }

            private static Guid RequireCurrentVersion(Guid id)
            {
                var version = AssetDatabaseQueryService.GetCurrentRuntimeArtifactCacheID(id);
                if (version == Guid.Empty)
                    throw new InvalidOperationException("Artifact has no exact thumbnail cache identity: " + id);
                return version;
            }

            private static bool LoadedExact(Guid id, Guid version)
            {
                var asset = AssetDatabaseQueryService.LoadAssetPreview(id);
                return AssetDatabaseQueryService.GetLoadedRuntimeArtifactCacheID(asset) == version;
            }

            private static bool TryGetExactThumbnail(AssetItem item, Guid version, out SpriteHandle thumbnail)
            {
                thumbnail = item.Thumbnail;
                if (!thumbnail.IsValid || !(thumbnail.Atlas is PreviewsCache cache))
                    return false;
                thumbnail = cache.FindSlotVersioned(item.ID, version);
                return thumbnail.IsValid;
            }

            private void AssertParticleThumbnail(SpriteHandle thumbnail, string name)
            {
                if (_particleProxy.LastThumbnailParticleCount <= 0)
                    throw new InvalidOperationException(name + " rendered without simulated particles.");
                if (CountVisiblePixels(thumbnail) == 0)
                    throw new InvalidOperationException(name + " contains no visible pixels.");
            }

            private static int CountVisiblePixels(SpriteHandle thumbnail)
            {
                var data = thumbnail.Atlas.Texture.DownloadData();
                if (data == null)
                    throw new InvalidOperationException("Thumbnail atlas GPU download failed.");
                try
                {
                    if (data.GetPixels(out Color32[] pixels))
                        throw new InvalidOperationException("Thumbnail atlas pixel conversion failed.");
                    var location = thumbnail.Location;
                    var size = thumbnail.Size;
                    var visible = 0;
                    for (var y = 0; y < (int)size.Y - 2; y++)
                    {
                        var row = ((int)location.Y + y) * data.Width + (int)location.X;
                        for (var x = 0; x < (int)size.X; x++)
                        {
                            var pixel = pixels[row + x];
                            if (pixel.R + pixel.G + pixel.B > 24)
                                visible++;
                        }
                    }
                    return visible;
                }
                finally
                {
                    FlaxEngine.Object.Destroy(data);
                }
            }

            private static ulong GetPixelHash(SpriteHandle thumbnail)
            {
                var data = thumbnail.Atlas.Texture.DownloadData();
                if (data == null)
                    throw new InvalidOperationException("Thumbnail atlas pixel download failed.");
                try
                {
                    if (data.GetPixels(out Color32[] pixels))
                        throw new InvalidOperationException("Thumbnail atlas pixel conversion failed.");
                    var location = thumbnail.Location;
                    var size = thumbnail.Size;
                    ulong hash = 1469598103934665603UL;
                    for (var y = 0; y < (int)size.Y; y++)
                    {
                        var row = ((int)location.Y + y) * data.Width + (int)location.X;
                        for (var x = 0; x < (int)size.X; x++)
                        {
                            var pixel = pixels[row + x];
                            hash = (hash ^ pixel.R) * 1099511628211UL;
                            hash = (hash ^ pixel.G) * 1099511628211UL;
                            hash = (hash ^ pixel.B) * 1099511628211UL;
                            hash = (hash ^ pixel.A) * 1099511628211UL;
                        }
                    }
                    return hash;
                }
                finally
                {
                    FlaxEngine.Object.Destroy(data);
                }
            }

            private static byte[] MoveParticleGraphNode(byte[] data, Float2 delta)
            {
                var owner = new SurfaceOwner { SurfaceData = (byte[])data.Clone() };
                using var surface = new ParticleEmitterSurface(owner, null, null);
                if (surface.Load())
                    throw new InvalidOperationException("Particle graph failed to load.");
                var node = surface.Nodes.FirstOrDefault(x => x != surface.RootNode)
                           ?? throw new InvalidOperationException("Particle graph has no movable node.");
                node.Location += delta;
                if (surface.Save())
                    throw new InvalidOperationException("Particle graph failed to serialize.");
                return owner.SurfaceData;
            }

            private static byte[] MoveMaterialGraphNode(byte[] data, Float2 delta)
            {
                var owner = new SurfaceOwner { SurfaceData = (byte[])data.Clone() };
                using var surface = new MaterialSurface(owner);
                if (surface.Load())
                    throw new InvalidOperationException("Material graph failed to load.");
                var node = surface.Nodes.FirstOrDefault()
                           ?? throw new InvalidOperationException("Material graph has no movable node.");
                node.Location += delta;
                if (surface.Save())
                    throw new InvalidOperationException("Material graph failed to serialize.");
                return owner.SurfaceData;
            }

            private void Complete(CliCommandResult result)
            {
                try
                {
                    Cleanup();
                }
                catch (Exception ex)
                {
                    result = CliCommandResult.Failure("FLX-ASSET-THUMBNAIL-CLEANUP-0006",
                        "Thumbnail validation cleanup failed: " + ex.Message, new { temporaryPath = _root });
                }
                _result = result;
            }

            private void Cleanup()
            {
                if (_particleReferenced)
                {
                    _particleItem.RemoveReference(this);
                    _particleReferenced = false;
                }
                if (_materialReferenced)
                {
                    _materialItem.RemoveReference(this);
                    _materialReferenced = false;
                }
                if (_particleSession != null)
                    AssetDocumentRegistry.Close(_particleItem, ref _particleSession);
                if (_materialSession != null)
                    AssetDocumentRegistry.Close(_materialItem, ref _materialSession);
                if (Directory.Exists(_root))
                    Directory.Delete(_root, true);
                AssetPipelineService.RefreshSources(new[] { _root });
            }

            public void OnItemDeleted(ContentItem item) { }
            public void OnItemRenamed(ContentItem item) { }
            public void OnItemReimported(ContentItem item) { }
            public void OnItemDispose(ContentItem item) { }
        }
    }
}
