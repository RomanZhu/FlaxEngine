// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Content;
using FlaxEditor.Content.Import;
using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Dedicated;
using FlaxEditor.CustomEditors.Editors;
using FlaxEditor.Scripting;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Tools;

namespace FlaxEditor.Windows
{
    public partial class PropertiesWindow
    {
        private abstract class ImportAssetPropertiesProxy
        {
            [HideInEditor]
            public Asset Asset { get; }

            [HideInEditor]
            public BinaryAssetItem Item { get; }

            [HideInEditor]
            public object InspectedObject { get; }

            [HideInEditor]
            public abstract object SelectionKey { get; }

            protected ImportAssetPropertiesProxy(BinaryAssetItem item, Asset asset, object inspectedObject = null)
            {
                Item = item;
                Asset = asset;
                InspectedObject = inspectedObject ?? asset;
            }

            public abstract void Reimport();

            public virtual IEnumerable<object> GetUndoObjects()
            {
                yield return this;
                yield return InspectedObject;
            }
        }

        [CustomEditor(typeof(ImportAssetPropertiesEditor))]
        private sealed class ModelImportAssetPropertiesProxy : ImportAssetPropertiesProxy
        {
            public ModelImportSettings ImportSettings = new ModelImportSettings();

            [HideInEditor, Collection(CanReorderItems = true, NotNullItems = true, Spacing = 10)]
            public RetargetSetupProperties[] RetargetSetups;

            [HideInEditor, Collection(CanReorderItems = true, NotNullItems = true, Spacing = 10)]
            public ModelMaterialProperties[] Materials;

            [HideInEditor, NoSerialize, NoUndo]
            public ModelUVChannel UVChannel;

            [HideInEditor, NoSerialize, NoUndo]
            public int UVLOD;

            [HideInEditor, NoSerialize, NoUndo]
            public int UVMesh = -1;

            public override object SelectionKey => ImportSettings.Settings.Type;

            public ModelImportAssetPropertiesProxy(BinaryAssetItem item, Asset asset, ModelImportSettings importSettings, object inspectedObject = null)
            : base(item, asset, inspectedObject)
            {
                ImportSettings = importSettings;
                if (asset is ModelBase model && model.IsLoaded)
                    Materials = GetMaterials(model);
                if (asset is SkinnedModel skinnedModel && skinnedModel.IsLoaded)
                    RetargetSetups = GetRetargetSetups(skinnedModel);
            }

            public override void Reimport()
            {
                FlaxEditor.Editor.Instance.ContentImporting.Reimport(Item, ImportSettings, true);
            }

            public override IEnumerable<object> GetUndoObjects()
            {
                foreach (var value in base.GetUndoObjects())
                    yield return value;

                if (Asset is not ModelBase model || !model.IsLoaded)
                    yield break;

                for (int lodIndex = 0; lodIndex < model.LODsCount; lodIndex++)
                {
                    var lod = model.GetLOD(lodIndex);
                    if (lod != null)
                        yield return lod;
                    model.GetMeshes(out var meshes, lodIndex);
                    for (int meshIndex = 0; meshIndex < meshes.Length; meshIndex++)
                        yield return meshes[meshIndex];
                }
            }

            public void SyncEditorProperties()
            {
                if (Asset is ModelBase model && Materials != null)
                {
                    if (model.MaterialSlots.Length != Materials.Length)
                        model.SetupMaterialSlots(Materials.Length);
                    var materialSlots = model.MaterialSlots;
                    for (int i = 0; i < Materials.Length; i++)
                    {
                        materialSlots[i].Name = Materials[i]?.Name ?? $"Material {i}";
                        materialSlots[i].Material = Materials[i]?.Material;
                        materialSlots[i].ShadowsMode = Materials[i]?.ShadowsMode ?? ShadowsCastingMode.All;
                    }
                }

                if (Asset is not SkinnedModel skinnedModel || RetargetSetups == null)
                    return;

                var setups = new SkinnedModel.SkeletonRetarget[RetargetSetups.Length];
                for (int i = 0; i < setups.Length; i++)
                {
                    var source = RetargetSetups[i];
                    setups[i] = new SkinnedModel.SkeletonRetarget
                    {
                        SourceAsset = source?.SourceAsset?.ID ?? Guid.Empty,
                        SkeletonAsset = source?.Skeleton?.ID ?? Guid.Empty,
                        NodesMapping = source?.NodesMapping != null
                            ? new Dictionary<string, string>(source.NodesMapping)
                            : new Dictionary<string, string>(),
                    };
                }
                skinnedModel.SkeletonRetargets = setups;
            }

            private static ModelMaterialProperties[] GetMaterials(ModelBase model)
            {
                var source = model.MaterialSlots;
                var result = new ModelMaterialProperties[source.Length];
                for (int i = 0; i < result.Length; i++)
                {
                    result[i] = new ModelMaterialProperties
                    {
                        Name = source[i].Name,
                        Material = source[i].Material,
                        ShadowsMode = source[i].ShadowsMode,
                    };
                }
                return result;
            }

            private static RetargetSetupProperties[] GetRetargetSetups(SkinnedModel model)
            {
                var source = model.SkeletonRetargets;
                var result = new RetargetSetupProperties[source.Length];
                for (int i = 0; i < result.Length; i++)
                {
                    result[i] = new RetargetSetupProperties
                    {
                        SourceAsset = FlaxEngine.Content.LoadAsync(source[i].SourceAsset),
                        Skeleton = FlaxEngine.Content.LoadAsync<SkinnedModel>(source[i].SkeletonAsset),
                        NodesMapping = source[i].NodesMapping != null
                            ? new Dictionary<string, string>(source[i].NodesMapping)
                            : new Dictionary<string, string>(),
                    };
                }
                return result;
            }
        }

        private sealed class RetargetSetupProperties
        {
            [EditorOrder(0), EditorDisplay("Retarget", "Source Asset"), AssetReference(true)]
            public Asset SourceAsset;

            [EditorOrder(10), EditorDisplay("Retarget", "Skeleton"), AssetReference(true)]
            public SkinnedModel Skeleton;

            [EditorOrder(20), EditorDisplay("Retarget", "Node Mapping")]
            public Dictionary<string, string> NodesMapping = new Dictionary<string, string>();
        }

        private sealed class ModelMaterialProperties
        {
            [EditorOrder(0), EditorDisplay("Material", "Name")]
            public string Name;

            [EditorOrder(10), EditorDisplay("Material", "Material"), AssetReference(true)]
            public MaterialBase Material;

            [EditorOrder(20), EditorDisplay("Material", "Shadows Mode")]
            public ShadowsCastingMode ShadowsMode = ShadowsCastingMode.All;
        }

        private enum ModelUVChannel
        {
            None,
            TexCoord0,
            TexCoord1,
            TexCoord2,
            TexCoord3,
            LightmapUVs,
        }

        [CustomEditor(typeof(ImportAssetPropertiesEditor))]
        private sealed class TextureImportAssetPropertiesProxy : ImportAssetPropertiesProxy
        {
            public TextureImportSettings ImportSettings = new TextureImportSettings();

            public override object SelectionKey => Asset.GetType();

            public TextureImportAssetPropertiesProxy(BinaryAssetItem item, Asset asset, TextureImportSettings importSettings)
            : base(item, asset)
            {
                ImportSettings = importSettings;
            }

            public override void Reimport()
            {
                if (Asset is SpriteAtlas)
                    ImportSettings.Settings.Sprites = null;
                FlaxEditor.Editor.Instance.ContentImporting.Reimport(Item, ImportSettings, true);
            }
        }

        [CustomEditor(typeof(ImportAssetPropertiesEditor))]
        private sealed class AudioImportAssetPropertiesProxy : ImportAssetPropertiesProxy
        {
            public AudioImportSettings ImportSettings = new AudioImportSettings();

            public override object SelectionKey => typeof(AudioClip);

            public AudioImportAssetPropertiesProxy(BinaryAssetItem item, AudioClip asset, AudioImportSettings importSettings)
            : base(item, asset)
            {
                ImportSettings = importSettings;
            }

            public override void Reimport()
            {
                FlaxEditor.Editor.Instance.ContentImporting.Reimport(Item, ImportSettings, true);
            }
        }

        private sealed partial class ImportAssetPropertiesEditor : CustomEditor
        {
            public override DisplayStyle Style => DisplayStyle.InlineIntoParent;

            public override void Initialize(LayoutElementsContainer layout)
            {
                if (Values.Count == 0 || Values[0] is not ImportAssetPropertiesProxy first)
                    return;

                var proxies = Values.Cast<ImportAssetPropertiesProxy>().ToArray();

                var inspectedType = first.InspectedObject.GetType();
                var allActors = proxies.All(x => x.InspectedObject is Actor);
                var assetType = allActors
                    ? typeof(Actor)
                    : proxies.All(x => x.InspectedObject.GetType() == inspectedType)
                        ? inspectedType
                        : typeof(object);
                var assetValues = new InspectedObjectValueContainer(Values, new ScriptType(assetType));
                var assetGroup = layout.Group(first.InspectedObject is Actor ? "Prefab Root" : "Asset");
                assetGroup.Object(assetValues, first.InspectedObject is Actor ? new ActorEditor() : new ContentAssetEditor());

                if (first is ModelImportAssetPropertiesProxy)
                    BuildModelAssetProperties(layout, Values, proxies.Cast<ModelImportAssetPropertiesProxy>().ToArray());

                var importSettingsMember = new ScriptType(first.GetType()).GetField(nameof(ModelImportAssetPropertiesProxy.ImportSettings));
                if (importSettingsMember == ScriptMemberInfo.Null)
                {
                    layout.Label("Import settings are unavailable for this selection.", TextAlignment.Center);
                    return;
                }

                var importSettingsValues = new ValueContainer(importSettingsMember, Values);
                var importGroup = layout.Group("Import Settings");
                importGroup.Object(importSettingsValues);

                var pathGroup = layout.Group("Import Path");
                if (proxies.Length == 1)
                {
                    Utilities.Utils.CreateImportPathUI(pathGroup, first.Item);
                }
                else
                {
                    pathGroup.Label($"{proxies.Length} source assets selected.");
                }

                var reimportButton = layout.Button(proxies.Length == 1 ? "Reimport" : $"Reimport {proxies.Length} Assets");
                reimportButton.Button.Clicked += () =>
                {
                    for (int i = 0; i < proxies.Length; i++)
                        proxies[i].Reimport();
                };
            }
        }

        /// <summary>
        /// Maps the nested Asset/Prefab Root editor values back to the selected import proxies.
        /// A plain detached value container cannot be used here because every custom editor is
        /// refreshed against its parent values on each presenter update.
        /// </summary>
        private sealed class InspectedObjectValueContainer : ValueContainer
        {
            public InspectedObjectValueContainer(ValueContainer proxyValues, ScriptType type)
            : base(ScriptMemberInfo.Null)
            {
                SetType(type);
                Refresh(proxyValues);
            }

            public override void Refresh(ValueContainer instanceValues)
            {
                if (instanceValues == null)
                    throw new ArgumentNullException(nameof(instanceValues));

                if (Count != instanceValues.Count)
                {
                    Clear();
                    for (int i = 0; i < instanceValues.Count; i++)
                        Add(((ImportAssetPropertiesProxy)instanceValues[i]).InspectedObject);
                    return;
                }

                for (int i = 0; i < instanceValues.Count; i++)
                    this[i] = ((ImportAssetPropertiesProxy)instanceValues[i]).InspectedObject;
            }

            public override void Set(ValueContainer instanceValues, object value)
            {
                // Asset and Actor instances cannot be replaced through the import proxy. Their
                // nested editors mutate the inspected objects directly, so only refresh the link.
                Refresh(instanceValues);
            }

            public override void Set(ValueContainer instanceValues, ValueContainer values)
            {
                Refresh(instanceValues);
            }

            public override void Set(ValueContainer instanceValues)
            {
                Refresh(instanceValues);
            }
        }

        private sealed class ImportAssetPropertiesState : IDisposable
        {
            private const float ModelSaveDelay = 0.15f;
            public readonly List<ImportAssetPropertiesProxy> Proxies;
            private readonly List<PrefabContentAssetState> _prefabStates;
            private bool _hasPendingModelSave;
            private float _pendingModelSaveDelay;

            public ImportAssetPropertiesState(List<ImportAssetPropertiesProxy> proxies, List<PrefabContentAssetState> prefabStates)
            {
                Proxies = proxies;
                _prefabStates = prefabStates;
            }

            public void ApplyPrefabChanges(Editor editor)
            {
                for (int i = 0; i < _prefabStates.Count; i++)
                    _prefabStates[i].Apply(editor);

                SyncModelsAndRequestSave();
            }

            public void ApplyModelUndoRedo()
            {
                SyncModelsAndRequestSave();
            }

            private void SyncModelsAndRequestSave()
            {
                bool containsModel = false;
                for (int i = 0; i < Proxies.Count; i++)
                {
                    if (Proxies[i] is ModelImportAssetPropertiesProxy modelProxy && modelProxy.Asset is ModelBase)
                    {
                        modelProxy.SyncEditorProperties();
                        containsModel = true;
                    }
                }
                if (containsModel)
                {
                    _hasPendingModelSave = true;
                    _pendingModelSaveDelay = ModelSaveDelay;
                }
            }

            public void UpdateDeferredSave(float deltaTime, bool isDragging)
            {
                if (!_hasPendingModelSave)
                    return;
                if (isDragging)
                {
                    _pendingModelSaveDelay = ModelSaveDelay;
                    return;
                }

                _pendingModelSaveDelay -= deltaTime;
                if (_pendingModelSaveDelay <= 0.0f)
                    SavePendingChanges();
            }

            public void SavePendingChanges()
            {
                if (!_hasPendingModelSave)
                    return;
                _hasPendingModelSave = false;
                for (int i = 0; i < Proxies.Count; i++)
                {
                    if (Proxies[i].Asset is ModelBase model && model.IsLoaded && model.Save())
                        Editor.LogError($"Failed to save model asset '{model.Path}'.");
                }
            }

            public void DiscardPendingChanges()
            {
                _hasPendingModelSave = false;
            }

            public bool TryGetPrefab(Actor instance, out Prefab prefab)
            {
                for (int i = 0; i < _prefabStates.Count; i++)
                {
                    var state = _prefabStates[i];
                    if (state.Instance == instance)
                    {
                        prefab = state.Asset;
                        return prefab != null;
                    }
                }

                prefab = null;
                return false;
            }

            public void Dispose()
            {
                SavePendingChanges();
                for (int i = 0; i < _prefabStates.Count; i++)
                    _prefabStates[i].Dispose();
                _prefabStates.Clear();
                Proxies.Clear();
            }
        }

        private sealed class PrefabContentAssetsState : IDisposable
        {
            private readonly List<PrefabContentAssetState> _states;
            public readonly List<Actor> Instances;

            public PrefabContentAssetsState(List<PrefabContentAssetState> states)
            {
                _states = states;
                Instances = states.Select(x => x.Instance).ToList();
            }

            public void Apply(Editor editor)
            {
                for (int i = 0; i < _states.Count; i++)
                    _states[i].Apply(editor);
            }

            public bool TryGetPrefab(Actor instance, out Prefab prefab)
            {
                for (int i = 0; i < _states.Count; i++)
                {
                    var state = _states[i];
                    if (state.Instance == instance)
                    {
                        prefab = state.Asset;
                        return prefab != null;
                    }
                }

                prefab = null;
                return false;
            }

            public void Dispose()
            {
                for (int i = 0; i < _states.Count; i++)
                    _states[i].Dispose();
                _states.Clear();
                Instances.Clear();
            }
        }

        private bool TryCreateImportAssetPropertiesSelection(IReadOnlyList<ContentItem> selection, out ImportAssetPropertiesState state)
        {
            state = null;
            var proxies = new List<ImportAssetPropertiesProxy>();
            var waitingAssets = new List<Asset>();
            var prefabStates = new List<PrefabContentAssetState>();
            object selectionKey = null;
            Type proxyType = null;

            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is not AssetItem assetItem)
                {
                    if (selection[i] is ScriptItem or ShaderSourceItem || selection[i] is FileItem and not VideoItem)
                    {
                        DisposePrefabStates(prefabStates);
                        return false;
                    }
                    continue;
                }
                if (assetItem is not BinaryAssetItem binaryAssetItem)
                {
                    DisposePrefabStates(prefabStates);
                    return false;
                }

                var asset = binaryAssetItem.LoadAsync();
                if (asset == null || !TryCreateImportAssetPropertiesProxy(binaryAssetItem, asset, out var proxy, out var prefabState))
                {
                    DisposePrefabStates(prefabStates);
                    return false;
                }

                if (!asset.IsLoaded && !asset.LastLoadFailed)
                    waitingAssets.Add(asset);

                proxyType ??= proxy.GetType();
                selectionKey ??= proxy.SelectionKey;
                if (proxy.GetType() != proxyType || !Equals(proxy.SelectionKey, selectionKey))
                {
                    prefabState?.Dispose();
                    DisposePrefabStates(prefabStates);
                    return false;
                }
                proxies.Add(proxy);
                if (prefabState != null)
                    prefabStates.Add(prefabState);
            }

            if (proxies.Count == 0)
            {
                DisposePrefabStates(prefabStates);
                return false;
            }

            _waitingForContentAssets.AddRange(waitingAssets);
            state = new ImportAssetPropertiesState(proxies, prefabStates);
            return true;
        }

        private bool TryCreatePrefabPropertiesSelection(IReadOnlyList<ContentItem> selection, out PrefabContentAssetsState state)
        {
            state = null;
            var prefabStates = new List<PrefabContentAssetState>();

            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is not AssetItem assetItem)
                {
                    if (selection[i] is ScriptItem or ShaderSourceItem || selection[i] is FileItem and not VideoItem)
                    {
                        DisposePrefabStates(prefabStates);
                        return false;
                    }
                    continue;
                }

                var prefab = assetItem.LoadAsync() as Prefab;
                if (prefab == null || !prefab.IsLoaded)
                {
                    DisposePrefabStates(prefabStates);
                    return false;
                }

                var prefabState = new PrefabContentAssetState(prefab);
                if (!prefabState.Instance)
                {
                    prefabState.Dispose();
                    DisposePrefabStates(prefabStates);
                    return false;
                }
                prefabStates.Add(prefabState);
            }

            if (prefabStates.Count == 0)
                return false;

            state = new PrefabContentAssetsState(prefabStates);
            return true;
        }

        private static void DisposePrefabStates(List<PrefabContentAssetState> states)
        {
            for (int i = 0; i < states.Count; i++)
                states[i].Dispose();
            states.Clear();
        }

        private static bool TryCreateImportAssetPropertiesProxy(BinaryAssetItem item, Asset asset, out ImportAssetPropertiesProxy proxy, out PrefabContentAssetState prefabState)
        {
            proxy = null;
            prefabState = null;
            if (asset is Model or SkinnedModel or Animation or Prefab)
            {
                var settings = new ModelImportSettings();
                if (!FlaxEditor.Editor.TryRestoreImportOptions(ref settings.Settings, item.Path))
                    return false;

                var expectedType = asset switch
                {
                    Model => ModelTool.ModelType.Model,
                    SkinnedModel => ModelTool.ModelType.SkinnedModel,
                    Animation => ModelTool.ModelType.Animation,
                    Prefab => ModelTool.ModelType.Prefab,
                    _ => settings.Settings.Type,
                };
                if (settings.Settings.Type != expectedType)
                    return false;

                object inspectedObject = null;
                if (asset is Prefab prefab)
                {
                    prefabState = new PrefabContentAssetState(prefab);
                    if (!prefabState.Instance)
                    {
                        prefabState.Dispose();
                        prefabState = null;
                        return false;
                    }
                    inspectedObject = prefabState.Instance;
                }

                proxy = new ModelImportAssetPropertiesProxy(item, asset, settings, inspectedObject);
                return true;
            }

            if (asset is Texture or CubeTexture or SpriteAtlas)
            {
                var settings = new TextureImportSettings();
                if (!FlaxEditor.Editor.TryRestoreImportOptions(ref settings.Settings, item.Path))
                    return false;
                proxy = new TextureImportAssetPropertiesProxy(item, asset, settings);
                return true;
            }

            if (asset is AudioClip audioClip)
            {
                var settings = new AudioImportSettings();
                if (!FlaxEditor.Editor.TryRestoreImportOptions(ref settings.Settings, item.Path))
                    return false;
                proxy = new AudioImportAssetPropertiesProxy(item, audioClip, settings);
                return true;
            }

            return false;
        }
    }
}
