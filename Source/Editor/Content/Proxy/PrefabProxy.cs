// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content.Create;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Content proxy for <see cref="PrefabItem"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.JsonAssetBaseProxy" />
    [ContentContextMenu("New/Prefab")]
    public sealed class PrefabProxy : JsonAssetBaseProxy
    {
        private sealed class PrefabVariantCreateInfo
        {
            public AssetObjectId PrefabObjectId;
        }

        private static bool CanCreatePrefabAssets()
        {
            return Editor.Instance.StateMachine.CurrentState.CanEditContent &&
                   Editor.Instance.Windows.ContentWin?.CurrentViewFolder?.CanHaveAssets == true;
        }

        /// <summary>
        /// The prefab files extension.
        /// </summary>
        public static readonly string Extension = "prefab";

        /// <summary>
        /// The prefab asset data typename.
        /// </summary>
        public static readonly string AssetTypename = typeof(Prefab).FullName;

        /// <inheritdoc />
        public override string Name => "Prefab";

        /// <inheritdoc />
        public override string FileExtension => Extension;

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new PrefabWindow(editor, (AssetItem)item);
        }

        /// <inheritdoc />
        public override bool IsProxyFor(ContentItem item)
        {
            return item is PrefabItem;
        }

        /// <inheritdoc />
        public override bool IsProxyFor<T>()
        {
            return typeof(T) == typeof(Prefab);
        }

        /// <inheritdoc />
        public override Color AccentColor => Style.Current?.BorderSelected ?? Color.FromRGB(0x3D91D9);

        /// <inheritdoc />
        public override string TypeName => AssetTypename;

        /// <inheritdoc />
        public override AssetItem ConstructItem(string path, string typeName, ref Guid id)
        {
            return new PrefabItem(path, id);
        }

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            if (item is not PrefabItem prefabItem)
                return base.CanReimport(item);

            var prefab = FlaxEngine.Content.LoadAsset<Prefab>(prefabItem.ObjectID);
            return prefab.GetDefaultInstance().GetScript<ModelPrefab>() != null;
        }

        /// <inheritdoc />
        public override void OnContentWindowContextMenu(ContextMenu menu, ContentItem item)
        {
            base.OnContentWindowContextMenu(menu, item);

            if (item is not PrefabItem prefabItem)
                return;

            var variantButton = menu.AddButton("Create Prefab Variant", CreatePrefabVariantClicked);
            variantButton.Tag = prefabItem;
            variantButton.Enabled = CanCreatePrefabAssets();
        }

        private static void CreatePrefabVariantClicked(ContextMenuButton button)
        {
            if (button.Tag is PrefabItem prefabItem)
                CreatePrefabVariant(prefabItem);
        }

        private static void CreatePrefabVariant(PrefabItem prefabItem)
        {
            if (!CanCreatePrefabAssets())
                return;

            var proxy = Editor.Instance.ContentDatabase.GetProxy<Prefab>();
            Editor.Instance.Windows.ContentWin.NewItem(proxy, new PrefabVariantCreateInfo { PrefabObjectId = prefabItem.ObjectID }, null, prefabItem.ShortName + " Variant");
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (arg is PrefabVariantCreateInfo variant)
            {
                var prefab = FlaxEngine.Content.LoadAssetAsync<Prefab>(variant.PrefabObjectId);
                var variantActor = PrefabManager.SpawnPrefab(prefab, null);
                if (!variantActor)
                {
                    Editor.LogError("Failed to create prefab variant.");
                    return;
                }

                try
                {
                    variantActor.Name = System.IO.Path.GetFileNameWithoutExtension(outputPath);
                    if (PrefabManager.CreatePrefab(variantActor, outputPath, true))
                        throw new InvalidOperationException("Failed to create the prefab variant source.");
                }
                finally
                {
                    FlaxEngine.Object.Destroy(variantActor, 20.0f);
                }
                if (AuthoredAssetDocumentService.CreateMetadata(outputPath) == Guid.Empty)
                    throw new InvalidOperationException("Failed to register the prefab variant source in the asset database.");
                return;
            }

            bool resetTransform = false;
            var transform = Transform.Identity;
            if (!(arg is Actor actor))
            {
                Editor.Instance.ContentImporting.Create(new PrefabCreateEntry(outputPath));
                return;
            }
            else if (actor.HasScene)
            {
                // Create prefab with identity transform so the actor instance on a level will have it customized
                resetTransform = true;
                transform = actor.LocalTransform;
                actor.LocalTransform = Transform.Identity;
            }

            var createFailed = PrefabManager.CreatePrefab(actor, outputPath, true);
            if (resetTransform)
                actor.LocalTransform = transform;
            if (createFailed)
                throw new InvalidOperationException("Failed to create the prefab source.");
            if (AuthoredAssetDocumentService.CreateMetadata(outputPath) == Guid.Empty)
                throw new InvalidOperationException("Failed to register the prefab source in the asset database.");
        }

    }

    /// <summary>
    /// Content proxy for quick UI Control prefab creation as widget.
    /// </summary>
    [ContentContextMenu("New/Widget")]
    internal sealed class WidgetProxy : AssetProxy
    {
        /// <inheritdoc />
        public override string Name => "UI Widget";

        /// <inheritdoc />
        public override bool IsProxyFor(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override string FileExtension => PrefabProxy.Extension;

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return null;
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.Transparent;

        /// <inheritdoc />
        public override string TypeName => PrefabProxy.AssetTypename;

        /// <inheritdoc />
        public override AssetItem ConstructItem(string path, string typeName, ref Guid id)
        {
            return null;
        }

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            Editor.Instance.ContentImporting.Create(new WidgetCreateEntry(outputPath));
            return;
        }
    }
}
