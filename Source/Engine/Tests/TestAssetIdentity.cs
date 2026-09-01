// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestAssetIdentity
    {
        [Test]
        public void PersistentObjectGuidUsesCanonicalFlaxGuidOrder()
        {
            const string guidText = "74a68a984824b4510d12589f199ad68f";
            var objectId = Json.JsonSerializer.ParseID(guidText);

            Assert.AreEqual(guidText, Json.JsonSerializer.GetStringID(objectId));
            Assert.IsTrue(Guid.TryParse(guidText, out var parsed));
            Assert.AreEqual(objectId, parsed);
        }
    }
}
#endif
