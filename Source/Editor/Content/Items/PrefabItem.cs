// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Text;
using FlaxEditor.Scripting;
using FlaxEngine;
using FlaxEngine.Utilities;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Content item that contains <see cref="FlaxEngine.Prefab"/> data.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.JsonAssetItem" />
    public sealed class PrefabItem : JsonAssetItem
    {
        private string _cachedTypeDescription = null;
        private string _cachedInstanceTypeDescription = null;
        private string _cachedSourcePrefabDescription = null;
        private Guid _cachedSourcePrefabId = Guid.Empty;
        private bool _hasCachedSourcePrefabId;

        /// <summary>
        /// Initializes a new instance of the <see cref="PrefabItem"/> class.
        /// </summary>
        /// <param name="path">The asset path.</param>
        /// <param name="id">The asset identifier.</param>
        public PrefabItem(string path, Guid id)
        : base(path, id, PrefabProxy.AssetTypename)
        {
        }

        /// <inheritdoc />
        public override bool OnEditorDrag(object context)
        {
            return true;
        }

        /// <inheritdoc />
        public override Actor OnEditorDrop(object context)
        {
            return PrefabManager.SpawnPrefab(FlaxEngine.Content.LoadAssetAsync<Prefab>(ObjectID), null);
        }

        /// <inheritdoc />
        public override ContentItemType ItemType => ContentItemType.Asset;

        /// <inheritdoc />
        public override ContentItemSearchFilter SearchFilter => ContentItemSearchFilter.Prefab;

        /// <inheritdoc />
        public override SpriteHandle DefaultThumbnail => SpriteHandle.Invalid;

        /// <inheritdoc />
        public override string TypeDescription
        {
            get
            {
                if (_cachedTypeDescription == null)
                {
                    _cachedTypeDescription = "Prefab";
                    var prefab = FlaxEngine.Content.LoadAsset<Prefab>(ObjectID);
                    if (prefab)
                    {
                        Actor root = prefab.GetDefaultInstance();
                        if (root && root.HasPrefabLink)
                            _cachedTypeDescription = "Prefab Variant";
                        else if (root is UIControl or UICanvas)
                            _cachedTypeDescription = "Widget";
                    }
                }
                return _cachedTypeDescription;
            }
        }

        private Guid SourcePrefabID
        {
            get
            {
                if (!_hasCachedSourcePrefabId)
                {
                    _hasCachedSourcePrefabId = true;
                    var prefab = FlaxEngine.Content.LoadAsset<Prefab>(ObjectID);
                    if (prefab)
                    {
                        var root = prefab.GetDefaultInstance();
                        if (root && root.HasPrefabLink)
                            _cachedSourcePrefabId = root.PrefabID;
                    }
                }
                return _cachedSourcePrefabId;
            }
        }

        private string SourcePrefabDescription
        {
            get
            {
                if (_cachedSourcePrefabDescription == null)
                {
                    _cachedSourcePrefabDescription = string.Empty;
                    var sourcePrefabId = SourcePrefabID;
                    if (sourcePrefabId != Guid.Empty)
                    {
                        var item = Editor.Instance.ContentDatabase.FindAsset(sourcePrefabId);
                        if (item != null)
                        {
                            _cachedSourcePrefabDescription = item.ShortName;
                        }
                        else if (FlaxEngine.Content.GetRuntimeAssetInfo(sourcePrefabId, out var info))
                        {
                            _cachedSourcePrefabDescription = System.IO.Path.GetFileNameWithoutExtension(info.Path);
                        }
                    }
                }
                return _cachedSourcePrefabDescription;
            }
        }

        private string InstanceTypeDescription
        {
            get
            {
                if (_cachedInstanceTypeDescription == null)
                {
                    _cachedInstanceTypeDescription = string.Empty;
                    var prefab = FlaxEngine.Content.LoadAsset<Prefab>(ObjectID);
                    if (prefab)
                    {
                        var root = prefab.GetDefaultInstance();
                        if (root)
                        {
                            var type = TypeUtils.GetObjectType(root);
                            _cachedInstanceTypeDescription = type ? type.Name : root.GetType().GetTypeDisplayName();
                        }
                    }
                }
                return _cachedInstanceTypeDescription;
            }
        }

        /// <inheritdoc />
        protected override void OnBuildTooltipText(StringBuilder sb)
        {
            sb.Append("Type: ").Append(TypeDescription).AppendLine();
            if (!string.IsNullOrEmpty(SourcePrefabDescription))
                sb.Append("Source Prefab: ").Append(SourcePrefabDescription).AppendLine();
            if (!string.IsNullOrEmpty(InstanceTypeDescription))
                sb.Append("Prefab Instance Type: ").Append(InstanceTypeDescription).AppendLine();
            if (File.Exists(Path))
                sb.Append("Size: ").Append(FlaxEditor.Utilities.Utils.FormatBytesCount((ulong)new FileInfo(Path).Length)).AppendLine();
            sb.Append("Path: ").Append(FlaxEditor.Utilities.Utils.GetAssetNamePathWithExt(Path)).AppendLine();
        }

        /// <inheritdoc />
        public override bool IsOfType(Type type)
        {
            return type.IsAssignableFrom(typeof(Prefab));
        }
    }
}
