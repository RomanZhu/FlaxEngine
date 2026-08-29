// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Content;
using FlaxEditor.Viewport.Previews;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    public partial class PropertiesWindow
    {
        private const float AssetPreviewHeaderHeight = 24.0f;
        private const float AssetPreviewHeight = 260.0f;
        private const float AssetPreviewMaxWindowRatio = 0.5f;

        private Panel _assetPreviewPanel;
        private ContainerControl _assetPreviewHost;
        private Label _assetPreviewTitle;
        private Button _assetPreviewToggle;
        private Control _assetPreviewControl;
        private Asset _assetPreviewAsset;
        private bool _assetPreviewExpanded = true;
        private bool _assetPreviewNeedsCameraReset;

        private void InitializeAssetPreview()
        {
            _assetPreviewPanel = new Panel
            {
                AnchorPreset = AnchorPresets.HorizontalStretchBottom,
                BackgroundColor = Style.Current.Background,
                IsScrollable = false,
                Visible = false,
                Parent = _selectionTab,
            };

            var header = new ContainerControl
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(0.0f, 0.0f, 0.0f, AssetPreviewHeaderHeight),
                BackgroundColor = Style.Current.SecondaryBackground,
                Parent = _assetPreviewPanel,
            };
            _assetPreviewTitle = new Label
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(8.0f, 58.0f, 0.0f, 0.0f),
                Text = "Asset Preview",
                HorizontalAlignment = TextAlignment.Near,
                Parent = header,
            };
            _assetPreviewToggle = new Button
            {
                AnchorPreset = AnchorPresets.TopRight,
                Bounds = new Rectangle(-54.0f, 2.0f, 50.0f, AssetPreviewHeaderHeight - 4.0f),
                Text = "Hide",
                TooltipText = "Hide the selected asset preview.",
                Parent = header,
            };
            _assetPreviewToggle.Clicked += ToggleAssetPreview;

            _assetPreviewHost = new ContainerControl
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0.0f, 0.0f, AssetPreviewHeaderHeight, 0.0f),
                Parent = _assetPreviewPanel,
            };
            UpdateAssetPreviewLayout();
        }

        private void ToggleAssetPreview()
        {
            _assetPreviewExpanded = !_assetPreviewExpanded;
            UpdateAssetPreviewLayout();
            LayoutFilterControls();
        }

        private void UpdateAssetPreviewSelection(IReadOnlyList<ContentItem> selection)
        {
            if (selection == null || selection.Count == 0)
            {
                ClearAssetPreview();
                return;
            }

            AssetItem assetItem = null;
            Asset asset = null;
            int selectedAssetCount = 0;
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is not AssetItem candidateItem)
                    continue;

                selectedAssetCount++;
                if (assetItem != null)
                    continue;

                // Inspector selection is explicit, so ensure the preview artifact is available.
                var candidateAsset = candidateItem.LoadAsync();
                if (SupportsAssetPreview(candidateAsset) && !candidateAsset.LastLoadFailed)
                {
                    assetItem = candidateItem;
                    asset = candidateAsset;
                }
            }

            if (assetItem == null)
            {
                ClearAssetPreview();
                return;
            }

            if (!asset.IsLoaded && !_waitingForContentAssets.Contains(asset))
                _waitingForContentAssets.Add(asset);

            if (_assetPreviewAsset != asset)
            {
                DisposeAssetPreviewControl();
                _assetPreviewAsset = asset;
            }

            var selectionSuffix = selectedAssetCount > 1 ? $" (first of {selectedAssetCount} selected)" : string.Empty;
            _assetPreviewTitle.Text = asset.IsLoaded
                ? $"Asset Preview - {assetItem.ShortName}{selectionSuffix}"
                : $"Asset Preview - {assetItem.ShortName} (Loading...){selectionSuffix}";
            _assetPreviewPanel.Visible = true;

            if (asset.IsLoaded && _assetPreviewControl == null)
            {
                try
                {
                    _assetPreviewControl = CreateAssetPreview(asset);
                    _assetPreviewNeedsCameraReset = _assetPreviewControl is ModelPreview or AnimatedModelPreview or PrefabPreview;
                }
                catch (Exception ex)
                {
                    Editor.LogWarning($"Failed to create the Properties asset preview for '{asset.Path}'.");
                    Editor.LogWarning(ex);
                }
            }
            ResetAssetPreviewCamera();
            UpdateAssetPreviewLayout();
            LayoutFilterControls();
        }

        private static bool SupportsAssetPreview(Asset asset)
        {
            return asset is TextureBase or SpriteAtlas or CubeTexture or Model or SkinnedModel or Prefab or MaterialBase;
        }

        private Control CreateAssetPreview(Asset asset)
        {
            Control preview;
            switch (asset)
            {
            case SpriteAtlas spriteAtlas:
                preview = new SpriteAtlasPreview(true) { Asset = spriteAtlas };
                break;
            case CubeTexture cubeTexture:
                preview = new CubeTexturePreview(true) { CubeTexture = cubeTexture };
                break;
            case TextureBase texture:
                preview = new TexturePreview(true) { Asset = texture };
                break;
            case Model model:
                preview = new ModelPreview(true)
                {
                    ScaleToFit = false,
                    Model = model,
                };
                break;
            case SkinnedModel skinnedModel:
                preview = new AnimatedModelPreview(true)
                {
                    ScaleToFit = false,
                    SkinnedModel = skinnedModel,
                };
                break;
            case Prefab prefab:
                preview = new PrefabPreview(true) { Prefab = prefab };
                break;
            case MaterialBase material:
                preview = new MaterialPreview(true) { Material = material };
                break;
            default:
                return null;
            }

            preview.AnchorPreset = AnchorPresets.StretchAll;
            preview.Offsets = Margin.Zero;
            preview.Parent = _assetPreviewHost;
            return preview;
        }

        private void ResetAssetPreviewCamera()
        {
            if (!_assetPreviewNeedsCameraReset || _assetPreviewAsset == null || !_assetPreviewAsset.IsLoaded)
                return;

            switch (_assetPreviewControl)
            {
            case ModelPreview modelPreview when modelPreview.Model != null:
                var modelBounds = modelPreview.Model.GetBox();
                if (modelBounds != BoundingBox.Empty)
                    modelPreview.ViewportCamera.SetArcBallView(modelBounds);
                break;
            case AnimatedModelPreview animatedModelPreview:
                animatedModelPreview.ViewportCamera.SetArcBallView(animatedModelPreview.GetBounds());
                break;
            case PrefabPreview prefabPreview when prefabPreview.Instance != null && prefabPreview.Instance is not UIControl:
                var bounds = prefabPreview.Instance.EditorBoxChildren;
                if (bounds != BoundingBox.Empty)
                    prefabPreview.ViewportCamera.SetArcBallView(bounds);
                break;
            }
            _assetPreviewNeedsCameraReset = false;
        }

        private float UpdateAssetPreviewLayout()
        {
            if (_assetPreviewPanel == null || !_assetPreviewPanel.Visible)
                return 0.0f;

            var height = AssetPreviewHeaderHeight;
            if (_assetPreviewExpanded)
            {
                var availableHeight = _selectionTab?.Height ?? AssetPreviewHeight;
                height = Mathf.Min(AssetPreviewHeight, Mathf.Max(AssetPreviewHeaderHeight, availableHeight * AssetPreviewMaxWindowRatio));
            }

            // A non-stretched anchored axis stores its position in Top and its size in Bottom.
            _assetPreviewPanel.Offsets = new Margin(0.0f, 0.0f, -height, height);
            _assetPreviewHost.Visible = _assetPreviewExpanded;
            _assetPreviewToggle.Text = _assetPreviewExpanded ? "Hide" : "Show";
            _assetPreviewToggle.TooltipText = _assetPreviewExpanded
                ? "Hide the selected asset preview."
                : "Show the selected asset preview.";
            _assetPreviewPanel.UpdateBounds();
            return height;
        }

        private void ClearAssetPreview()
        {
            if (_assetPreviewPanel == null)
                return;

            DisposeAssetPreviewControl();
            if (_assetPreviewPanel.Visible)
            {
                _assetPreviewPanel.Visible = false;
                LayoutFilterControls();
            }
        }

        private void DisposeAssetPreviewControl()
        {
            _assetPreviewAsset = null;
            _assetPreviewNeedsCameraReset = false;
            if (_assetPreviewControl == null)
                return;

            _assetPreviewControl.Dispose();
            _assetPreviewControl = null;
        }
    }
}
