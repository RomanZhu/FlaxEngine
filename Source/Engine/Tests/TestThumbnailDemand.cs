// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.IO;
using FlaxEditor;
using FlaxEditor.Content;
using FlaxEditor.Content.Thumbnails;
using FlaxEditor.GUI.Tree;
using FlaxEngine.GUI;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestThumbnailDemand
    {
        private sealed class Owner : IContentItemOwner
        {
            public void OnItemDeleted(ContentItem item)
            {
            }

            public void OnItemRenamed(ContentItem item)
            {
            }

            public void OnItemReimported(ContentItem item)
            {
            }

            public void OnItemDispose(ContentItem item)
            {
            }
        }

        [Test]
        public void OrdinaryOwnershipDoesNotCreateThumbnailDemand()
        {
            var item = new ContentFolder(ContentFolderType.Content, Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")), null);
            var ordinaryOwner = new Owner();
            var visibleOwner = new Owner();

            item.AddReference(ordinaryOwner);
            Assert.IsFalse(item.HasThumbnailReference);

            item.AddReference(visibleOwner, true);
            Assert.IsTrue(item.HasThumbnailReference);

            item.RemoveReference(ordinaryOwner);
            Assert.IsTrue(item.HasThumbnailReference);

            Assert.IsFalse(item.ExpireThumbnailDemand(Engine.FrameCount + 2));
            Assert.IsFalse(item.HasThumbnailReference);
            Assert.AreEqual(1, item.ReferencesCount, "Visibility expiry must not remove ordinary ownership.");

            item.RemoveReference(visibleOwner);
            Assert.IsFalse(item.HasThumbnailReference);
            Assert.AreEqual(0, item.ReferencesCount);
            item.Dispose();
        }

        [Test]
        public void LastGoodThumbnailSurvivesForcedFailureUntilExactReplacement()
        {
            var item = new ContentFolder(ContentFolderType.Content, Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")), null);
            var lastGood = Editor.Instance.Icons.Document128;
            var replacement = Editor.Instance.Icons.Folder128;
            Assert.IsTrue(lastGood.IsValid);
            Assert.IsTrue(replacement.IsValid);
            Assert.AreNotEqual(lastGood, replacement);

            item.Thumbnail = lastGood;
            item.SetStaleThumbnail(lastGood);
            item.NotifyThumbnailRequestQueued(true);
            item.NotifyThumbnailRequestFailed(true);

            Assert.AreEqual(lastGood, item.Thumbnail, "A failed replacement must retain the last-good pixels.");
            Assert.IsTrue(item.IsThumbnailStaleForTesting);
            Assert.IsFalse(item.IsThumbnailRequestQueuedForTesting);
            Assert.IsTrue(item.IsThumbnailRequestFailedForTesting);
            Assert.IsTrue(item.IsThumbnailForceRetryForTesting);

            item.NotifyThumbnailRequestQueued(true);
            item.Thumbnail = replacement;
            Assert.AreEqual(replacement, item.Thumbnail);
            Assert.IsFalse(item.IsThumbnailStaleForTesting);
            Assert.IsFalse(item.IsThumbnailRequestQueuedForTesting);
            Assert.IsFalse(item.IsThumbnailRequestFailedForTesting);
            Assert.IsFalse(item.IsThumbnailForceRetryForTesting);
            item.Dispose();
        }

        [Test]
        public void ForcedUndoReplacementCannotPublishLaterPixels()
        {
            var item = new ContentFolder(ContentFolderType.Content, Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")), null);
            var beforeEdit = Editor.Instance.Icons.Document128;
            var afterEdit = Editor.Instance.Icons.Folder128;

            item.Thumbnail = afterEdit;
            item.SetStaleThumbnail(afterEdit);
            item.NotifyThumbnailRequestQueued(true);
            Assert.AreEqual(afterEdit, item.Thumbnail, "Undo regeneration must retain marked later pixels while queued.");
            Assert.IsTrue(item.IsThumbnailStaleForTesting);

            item.Thumbnail = beforeEdit;
            Assert.AreEqual(beforeEdit, item.Thumbnail, "The matching undo replacement must win.");
            Assert.IsFalse(item.IsThumbnailStaleForTesting);
            Assert.IsFalse(item.IsThumbnailForceRetryForTesting);
            item.Dispose();
        }

        [Test]
        public void ExactArtifactVersionIsImmutableAndRejectsSupersession()
        {
            var requested = Guid.NewGuid();
            var newer = Guid.NewGuid();
            var cacheVersionField = typeof(ThumbnailRequest).GetField(nameof(ThumbnailRequest.CacheVersion));
            Assert.IsNotNull(cacheVersionField);
            Assert.IsTrue(cacheVersionField.IsInitOnly, "A queued publication must not be relabeled to a newer artifact.");
            Assert.IsFalse(ThumbnailRequest.IsArtifactVersionSuperseded(requested, requested));
            Assert.IsTrue(ThumbnailRequest.IsArtifactVersionSuperseded(requested, newer));
            Assert.IsFalse(ThumbnailRequest.IsArtifactVersionSuperseded(Guid.Empty, newer));
            Assert.IsFalse(ThumbnailRequest.IsArtifactVersionSuperseded(requested, Guid.Empty));
        }

        [Test]
        public void NewlyDuplicatedAssetRestartsWithItsPublishedExactVersion()
        {
            var sourceVersion = Guid.NewGuid();
            var duplicateVersion = Guid.NewGuid();

            Assert.IsFalse(ThumbnailRequest.ShouldRestartWithExactVersion(Guid.Empty, Guid.Empty));
            Assert.IsTrue(ThumbnailRequest.ShouldRestartWithExactVersion(Guid.Empty, duplicateVersion));
            Assert.IsFalse(ThumbnailRequest.ShouldRestartWithExactVersion(sourceVersion, sourceVersion));
            Assert.IsFalse(ThumbnailRequest.ShouldRestartWithExactVersion(sourceVersion, duplicateVersion));
            Assert.IsTrue(ThumbnailRequest.IsArtifactVersionSuperseded(sourceVersion, duplicateVersion),
                "The duplicate must not reuse the source asset's cache identity.");
        }

        [Test]
        public void LoadedMaterialWaitsForRenderingShaderReadiness()
        {
            Assert.IsFalse(ThumbnailsModule.HasMaterialRenderingQuality(false, false));
            Assert.IsFalse(ThumbnailsModule.HasMaterialRenderingQuality(false, true));
            Assert.IsFalse(ThumbnailsModule.HasMaterialRenderingQuality(true, false), "A loaded material with a compiling shader must not be drawn.");
            Assert.IsTrue(ThumbnailsModule.HasMaterialRenderingQuality(true, true));
        }

        [Test]
        public void TreeRowsPreferGeneratedThumbnails()
        {
            var item = new FileItem(Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")));
            var generated = Editor.Instance.Icons.Folder128;
            item.Thumbnail = generated;

            Assert.AreEqual(generated, ContentItemTreeNode.GetIconForTesting(item));
            item.Dispose();
        }

        [Test]
        public void RestoredVisibleTreeRowsRenewDemandWithoutDraw()
        {
            var panel = new Panel
            {
                Bounds = new Rectangle(0, 0, 200, 100),
            };
            var tree = new Tree(false)
            {
                AutoSize = false,
                Bounds = new Rectangle(0, 0, 200, 200),
                Parent = panel,
            };
            var restoredRoot = new TreeNode
            {
                Bounds = new Rectangle(0, 0, 200, 200),
                Parent = tree,
            };
            restoredRoot.Expand(true);
            var item = new FileItem(Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")));
            var generated = Editor.Instance.Icons.Folder128;
            item.Thumbnail = generated;
            var node = new ContentItemTreeNode(item)
            {
                Bounds = new Rectangle(0, 10, 180, 18),
                Parent = restoredRoot,
            };

            Assert.IsFalse(item.HasThumbnailReference);
            node.Update(0.0f);
            Assert.IsTrue(item.HasThumbnailReference, "An initially visible attached row must demand its thumbnail without waiting for Draw.");
            Assert.AreEqual(generated, item.Thumbnail, "Renewal must preserve the stable cached thumbnail identity.");

            Assert.IsFalse(item.ExpireThumbnailDemand(Engine.FrameCount + 2), "The visibility lease should expire when it is not renewed.");
            item.Thumbnail = generated;
            node.Update(0.0f);
            Assert.IsTrue(item.HasThumbnailReference, "A stable visible row must renew expired startup demand without scroll or unfold input.");
            Assert.AreEqual(generated, item.Thumbnail);

            Assert.IsFalse(ContentItemTreeNode.IsHeaderInViewport(new Rectangle(0, 110, 180, 18), panel.GetClientArea(), Float2.Zero));
            Assert.IsTrue(ContentItemTreeNode.IsHeaderInViewport(new Rectangle(0, 110, 180, 18), panel.GetClientArea(), new Float2(0, -40)),
                "Scrolling the same row into view must use the same visibility-demand path.");

            panel.Dispose();
            item.Dispose();
        }

        [Test]
        public void TreeRowsDetachBeforeDeletedOrDisposedItemsCanDrawAgain()
        {
            var deletedItem = new FileItem(Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")));
            var deletedNode = new ContentItemTreeNode(deletedItem);
            Assert.AreEqual(1, deletedItem.ReferencesCount);
            ((IContentItemOwner)deletedNode).OnItemDeleted(deletedItem);
            Assert.IsTrue(deletedNode.IsDisposing);
            Assert.AreEqual(0, deletedItem.ReferencesCount);
            deletedItem.Dispose();

            var disposedItem = new FileItem(Path.Combine(Globals.ProjectContentFolder, Guid.NewGuid().ToString("N")));
            var disposedNode = new ContentItemTreeNode(disposedItem);
            Assert.AreEqual(1, disposedItem.ReferencesCount);
            ((IContentItemOwner)disposedNode).OnItemDispose(disposedItem);
            Assert.IsTrue(disposedNode.IsDisposing);
            Assert.AreEqual(0, disposedItem.ReferencesCount);
            disposedItem.Dispose();
        }

        [Test]
        public void InitialTreeDemandWaitsForThumbnailCacheReadiness()
        {
            Assert.IsTrue(ThumbnailsModule.ShouldDeferRequest(false, false));
            Assert.IsTrue(ThumbnailsModule.ShouldDeferRequest(false, true));
            Assert.IsTrue(ThumbnailsModule.ShouldDeferRequest(true, false),
                "Initial visible rows must not miss cached slots while atlases are loading.");
            Assert.IsFalse(ThumbnailsModule.ShouldDeferRequest(true, true),
                "Initial demand must resume automatically once rendering and cache lookup are ready.");
        }

        public static int RunOrdinaryOwnershipDoesNotCreateThumbnailDemand()
        {
            var tests = new TestThumbnailDemand();
            tests.OrdinaryOwnershipDoesNotCreateThumbnailDemand();
            tests.LastGoodThumbnailSurvivesForcedFailureUntilExactReplacement();
            tests.ForcedUndoReplacementCannotPublishLaterPixels();
            tests.ExactArtifactVersionIsImmutableAndRejectsSupersession();
            tests.NewlyDuplicatedAssetRestartsWithItsPublishedExactVersion();
            tests.LoadedMaterialWaitsForRenderingShaderReadiness();
            tests.TreeRowsPreferGeneratedThumbnails();
            tests.RestoredVisibleTreeRowsRenewDemandWithoutDraw();
            tests.TreeRowsDetachBeforeDeletedOrDisposedItemsCanDrawAgain();
            tests.InitialTreeDemandWaitsForThumbnailCacheReadiness();
            return 0;
        }
    }
}
#endif
