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
        /// <param name="guid">The asset file identifier.</param>
        /// <param name="localId">The stable object identifier within the asset file.</param>
        public AssetObjectId(Guid guid, long localId)
        {
            Guid = guid;
            LocalId = localId;
        }

        /// <summary>
        /// Gets whether this identifier points to an asset object.
        /// </summary>
        public bool IsValid => Guid != System.Guid.Empty && LocalId != 0;

        /// <inheritdoc />
        public bool Equals(AssetObjectId other)
        {
            return Guid == other.Guid && LocalId == other.LocalId;
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
                return (Guid.GetHashCode() * 397) ^ LocalId.GetHashCode();
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
            return string.Concat(Guid.ToString("N"), ":", LocalId.ToString(CultureInfo.InvariantCulture));
        }
    }
}
