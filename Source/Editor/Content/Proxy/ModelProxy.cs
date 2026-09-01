// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="Model"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    public class ModelProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Model";

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new ModelWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0xe67e22);

        /// <inheritdoc />
        public override Type AssetType => typeof(Model);

        /// <inheritdoc />
        public override void OnContentWindowContextMenu(ContextMenu menu, ContentItem item)
        {
            base.OnContentWindowContextMenu(menu, item);

            menu.AddButton("Create collision data", () =>
            {
                var collisionDataProxy = (CollisionDataProxy)Editor.Instance.ContentDatabase.GetProxy<CollisionData>();
                var selection = Editor.Instance.Windows.ContentWin.View.Selection;
                if (selection.Count > 1)
                {
                    // Batch action
                    var items = selection.ToArray(); // Clone to prevent issue when iterating over and content window changes the selection
                    foreach (var contentItem in items)
                    {
                        if (contentItem is ModelItem modelItem)
                            collisionDataProxy.CreateCollisionDataFromModel(FlaxEngine.Content.LoadAssetAsync<Model>(modelItem.ObjectID), null, false);
                    }
                }
                else
                {
                    var model = FlaxEngine.Content.LoadAssetAsync<Model>(((ModelItem)item).ObjectID);
                    collisionDataProxy.CreateCollisionDataFromModel(model);
                }
            });
        }

    }
}
