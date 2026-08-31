// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    partial struct SceneReference : IComparable, IComparable<AssetObjectId>, IComparable<SceneReference>
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="SceneReference"/> class.
        /// </summary>
        /// <param name="id">The persistent identifier of the scene asset object.</param>
        public SceneReference(AssetObjectId id)
        {
            ID = id;
        }

        /// <summary>Initializes a reference to a scene source's main object.</summary>
        public SceneReference(AssetGuid asset)
        {
            ID = AssetObjectId.Main(asset);
        }

        /// <summary>
        /// Compares two values and returns true if are equal or false if unequal.
        /// </summary>
        /// <param name="left">The left value.</param>
        /// <param name="right">The right value.</param>
        /// <returns>True if values are equal, otherwise false.</returns>
        public static bool operator ==(SceneReference left, SceneReference right)
        {
            return left.ID == right.ID;
        }

        /// <summary>
        /// Compares two values and returns false if are equal or true if unequal.
        /// </summary>
        /// <param name="left">The left value.</param>
        /// <param name="right">The right value.</param>
        /// <returns>True if values are not equal, otherwise false.</returns>
        public static bool operator !=(SceneReference left, SceneReference right)
        {
            return left.ID != right.ID;
        }

        /// <summary>
        /// Compares two values and returns true if are equal or false if unequal.
        /// </summary>
        /// <param name="left">The left value.</param>
        /// <param name="right">The right value.</param>
        /// <returns>True if values are equal, otherwise false.</returns>
        public static bool operator ==(SceneReference left, AssetObjectId right)
        {
            return left.ID == right;
        }

        /// <summary>
        /// Compares two values and returns false if are equal or true if unequal.
        /// </summary>
        /// <param name="left">The left value.</param>
        /// <param name="right">The right value.</param>
        /// <returns>True if values are not equal, otherwise false.</returns>
        public static bool operator !=(SceneReference left, AssetObjectId right)
        {
            return left.ID != right;
        }

        /// <inheritdoc />
        public int CompareTo(object obj)
        {
            if (obj is AssetObjectId id)
                return CompareTo(id);
            if (obj is SceneReference other)
                return CompareTo(other);
            return 0;
        }

        /// <inheritdoc />
        public int CompareTo(AssetObjectId other)
        {
            int source = ID.Asset.Value.CompareTo(other.Asset.Value);
            return source != 0 ? source : ID.LocalId.CompareTo(other.LocalId);
        }

        /// <inheritdoc />
        public int CompareTo(SceneReference other)
        {
            return CompareTo(other.ID);
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            if (obj is AssetObjectId id)
                return ID == id;
            if (obj is SceneReference other)
                return ID == other.ID;
            return false;
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            return ID.GetHashCode();
        }

        /// <inheritdoc />
        public override string ToString()
        {
            return ID.ToString();
        }
    }
}
