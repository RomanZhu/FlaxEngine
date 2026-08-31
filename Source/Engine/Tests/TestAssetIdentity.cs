// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestAssetIdentity
    {
        [Test]
        public void CompactObjectIdUsesCanonicalFlaxGuidOrder()
        {
            const string guidText = "74a68a984824b4510d12589f199ad68f";
            const long localId = 18274497224001756L;
            var objectId = new AssetObjectId(new AssetGuid(Json.JsonSerializer.ParseID(guidText)), localId);
            var text = guidText + ":" + localId.ToString(System.Globalization.CultureInfo.InvariantCulture);

            Assert.AreEqual(text, objectId.ToString());
            Assert.IsTrue(AssetObjectId.TryParse(text, out var parsed));
            Assert.AreEqual(objectId, parsed);
            Assert.AreEqual(guidText, parsed.Asset.ToString());
        }
    }
}
#endif
