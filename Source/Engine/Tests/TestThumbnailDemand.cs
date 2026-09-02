// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.IO;
using FlaxEditor.Content;
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

            item.RemoveReference(visibleOwner);
            Assert.IsFalse(item.HasThumbnailReference);
            Assert.AreEqual(0, item.ReferencesCount);
            item.Dispose();
        }
    }
}
#endif
