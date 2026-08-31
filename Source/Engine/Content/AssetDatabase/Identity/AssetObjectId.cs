// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Globalization;

namespace FlaxEngine
{
    [Serializable]
    partial struct AssetObjectId : IEquatable<AssetObjectId>
    {
        /// <summary>
        /// Initializes a new asset object identifier.
        /// </summary>
        /// <param name="asset">The asset file identifier.</param>
        /// <param name="localId">The stable object identifier within the asset file.</param>
        public AssetObjectId(AssetGuid asset, long localId)
        {
            Asset = asset;
            LocalId = localId;
        }

        /// <summary>Creates an identifier for a source's main imported object.</summary>
        public static AssetObjectId Main(AssetGuid asset) => new AssetObjectId(asset, 1);

        /// <summary>Gets whether this identifier is null.</summary>
        public bool IsNull => !Asset.IsValid || LocalId == 0;

        /// <summary>Parses the canonical guid:fileId representation.</summary>
        public static bool TryParse(string value, out AssetObjectId result)
        {
            result = default;
            if (string.IsNullOrEmpty(value))
                return false;
            int separator = value.LastIndexOf(':');
            if (separator <= 0 || separator == value.Length - 1 ||
                !AssetGuid.TryParse(value.Substring(0, separator), out var asset) ||
                !long.TryParse(value.Substring(separator + 1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var localId) ||
                localId == 0)
                return false;
            result = new AssetObjectId(asset, localId);
            return true;
        }

        /// <summary>
        /// Gets whether this identifier points to an asset object.
        /// </summary>
        public bool IsValid => Asset.IsValid && LocalId != 0;

        /// <summary>Gets whether this is the main imported object.</summary>
        public bool IsMainObject => Asset.IsValid && LocalId == 1;

        /// <inheritdoc />
        public bool Equals(AssetObjectId other)
        {
            return Asset == other.Asset && LocalId == other.LocalId;
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            return obj is AssetObjectId other && Equals(other);
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            unchecked
            {
                return (Asset.GetHashCode() * 397) ^ LocalId.GetHashCode();
            }
        }

        public static bool operator ==(AssetObjectId left, AssetObjectId right)
        {
            return left.Equals(right);
        }

        public static bool operator !=(AssetObjectId left, AssetObjectId right)
        {
            return !left.Equals(right);
        }

        /// <inheritdoc />
        public override string ToString()
        {
            return string.Concat(Asset.ToString(), ":", LocalId.ToString(CultureInfo.InvariantCulture));
        }
    }
}
