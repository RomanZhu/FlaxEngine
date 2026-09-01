// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Base class for all asset proxy objects used to manage <see cref="AssetItem"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.ContentProxy" />
    [HideInEditor]
    public abstract class AssetProxy : ContentProxy
    {
        /// <inheritdoc />
        public override bool IsAsset => true;

        /// <summary>
        /// Gets the full name of the asset type (stored data format).
        /// </summary>
        public abstract string TypeName { get; }

        /// <summary>
        /// Gets a value indicating whether this instance is virtual Proxy not linked to any asset.
        /// </summary>
        protected virtual bool IsVirtual { get; }

        /// <summary>
        /// Determines whether [is virtual proxy].
        /// </summary>
        /// <returns><c>true</c> if [is virtual proxy]; otherwise, <c>false</c>.</returns>
        public bool IsVirtualProxy()
        {
            return IsVirtual && CanExport == false;
        }

        /// <summary>
        /// Checks if this proxy supports the given asset type id at the given path.
        /// </summary>
        /// <param name="typeName">The asset type identifier.</param>
        /// <param name="path">The asset path.</param>
        /// <returns>True if proxy supports assets of the given type id and path.</returns>
        public virtual bool AcceptsAsset(string typeName, string path)
        {
            return typeName == TypeName && path.EndsWith(FileExtension, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        /// Constructs the item for the asset.
        /// </summary>
        /// <param name="path">The asset path.</param>
        /// <param name="typeName">The asset type name identifier.</param>
        /// <param name="id">The asset identifier.</param>
        /// <returns>Created item.</returns>
        public abstract AssetItem ConstructItem(string path, string typeName, ref Guid id);

    }
}
