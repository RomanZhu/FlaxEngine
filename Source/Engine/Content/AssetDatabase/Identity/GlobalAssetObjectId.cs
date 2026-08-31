// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Globalization;

namespace FlaxEngine
{
    [Serializable]
    partial struct GlobalAssetObjectId : IEquatable<GlobalAssetObjectId>
    {
        /// <summary>Gets whether this identifier names a persistent object.</summary>
        public bool IsValid => SourceAsset.IsValid && LocalFileId != 0;

        /// <inheritdoc />
        public override string ToString()
        {
            return string.Concat(((int)Kind).ToString(CultureInfo.InvariantCulture), ":", SourceAsset.ToString(), ":",
                LocalFileId.ToString(CultureInfo.InvariantCulture), ":", PrefabInstanceFileId.ToString(CultureInfo.InvariantCulture));
        }

        /// <inheritdoc />
        public bool Equals(GlobalAssetObjectId other)
        {
            return Kind == other.Kind && SourceAsset == other.SourceAsset && LocalFileId == other.LocalFileId &&
                   PrefabInstanceFileId == other.PrefabInstanceFileId;
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            return obj is GlobalAssetObjectId other && Equals(other);
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            unchecked
            {
                var hash = (int)Kind;
                hash = (hash * 397) ^ SourceAsset.GetHashCode();
                hash = (hash * 397) ^ LocalFileId.GetHashCode();
                return (hash * 397) ^ PrefabInstanceFileId.GetHashCode();
            }
        }

        public static bool operator ==(GlobalAssetObjectId left, GlobalAssetObjectId right) => left.Equals(right);
        public static bool operator !=(GlobalAssetObjectId left, GlobalAssetObjectId right) => !left.Equals(right);
    }
}
