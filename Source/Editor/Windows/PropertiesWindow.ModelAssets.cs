// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Linq;
using FlaxEditor.CustomEditors;
using FlaxEditor.Scripting;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Utilities;

namespace FlaxEditor.Windows
{
    public partial class PropertiesWindow
    {
        private sealed partial class ImportAssetPropertiesEditor
        {
            private ModelUVPreviewControl _modelUVPreview;

            private void BuildModelAssetProperties(LayoutElementsContainer layout, ValueContainer parentValues, ModelImportAssetPropertiesProxy[] proxies)
            {
                var models = proxies.Select(x => x.Asset as ModelBase).ToArray();
                if (models.Any(x => x == null))
                    return;
                if (models.Any(x => !x.IsLoaded))
                {
                    layout.Group("Meshes").Label("Loading model data...", TextAlignment.Center);
                    return;
                }

                BuildMeshesProperties(layout, parentValues, models);
                BuildMaterialsProperties(layout, parentValues, models);
                BuildUVProperties(layout, parentValues, proxies, models);
                BuildRetargetProperties(layout, parentValues, proxies, models);
            }

            private void BuildMeshesProperties(LayoutElementsContainer layout, ValueContainer parentValues, ModelBase[] models)
            {
                var group = layout.Group("Meshes");
                if (models.Length > 1)
                    group.Label($"Editing common mesh structure across {models.Length} models.");

                group.Property("Min Screen Size", CreateModelValue(parentValues, typeof(float),
                    proxy => ((ModelBase)proxy.Asset).MinScreenSize,
                    (proxy, value) => ((ModelBase)proxy.Asset).MinScreenSize = (float)value,
                    new LimitAttribute(0.0f, 1.0f, 0.005f)), null,
                    "The minimum screen size at which the model is rendered. Set to zero to disable small-model culling.");

                int minLodCount = models.Min(x => x.LODsCount);
                int maxLodCount = models.Max(x => x.LODsCount);
                if (minLodCount != maxLodCount)
                    group.Label($"LOD counts differ ({minLodCount}-{maxLodCount}); only common LOD indices are editable.");

                for (int lodIndex = 0; lodIndex < minLodCount; lodIndex++)
                {
                    int capturedLodIndex = lodIndex;
                    var lodGroup = group.Group($"LOD {lodIndex}");
                    var firstLod = models[0].GetLOD(lodIndex);
                    lodGroup.Property("Screen Size", CreateModelValue(parentValues, typeof(float),
                        proxy => ((ModelBase)proxy.Asset).GetLOD(capturedLodIndex).ScreenSize,
                        (proxy, value) => ((ModelBase)proxy.Asset).GetLOD(capturedLodIndex).ScreenSize = (float)value,
                        new LimitAttribute(0.0f, 10.0f, 0.005f)), null,
                        "The lower screen-size boundary at which this LOD is selected.");

                    bool isLoaded = models.All(x => capturedLodIndex >= x.LODsCount - x.LoadedLODs);
                    if (!isLoaded)
                    {
                        lodGroup.Label("Loading LOD mesh data...");
                        continue;
                    }

                    var allMeshes = new MeshBase[models.Length][];
                    for (int modelIndex = 0; modelIndex < models.Length; modelIndex++)
                        models[modelIndex].GetMeshes(out allMeshes[modelIndex], capturedLodIndex);
                    int minMeshCount = allMeshes.Min(x => x.Length);
                    int maxMeshCount = allMeshes.Max(x => x.Length);
                    int triangleCount = allMeshes[0].Sum(x => x.TriangleCount);
                    int vertexCount = allMeshes[0].Sum(x => x.VertexCount);
                    lodGroup.Label(models.Length == 1
                        ? $"Meshes: {minMeshCount:N0}   Triangles: {triangleCount:N0}   Vertices: {vertexCount:N0}"
                        : $"Common meshes: {minMeshCount:N0}" + (minMeshCount != maxMeshCount ? $" (counts vary up to {maxMeshCount:N0})" : string.Empty));
                    lodGroup.Label("Size: " + firstLod.Box.Size).AddCopyContextMenu();

                    for (int meshIndex = 0; meshIndex < minMeshCount; meshIndex++)
                    {
                        int capturedMeshIndex = meshIndex;
                        var firstMesh = allMeshes[0][meshIndex];
                        var meshGroup = lodGroup.Group($"Mesh {meshIndex}");
                        meshGroup.Label($"Triangles: {firstMesh.TriangleCount:N0}   Vertices: {firstMesh.VertexCount:N0}");
                        int maximumSlot = Math.Max(0, models.Min(x => x.MaterialSlots.Length) - 1);
                        meshGroup.Property("Material Slot", CreateModelValue(parentValues, typeof(int),
                            proxy => GetMesh((ModelBase)proxy.Asset, capturedLodIndex, capturedMeshIndex)?.MaterialSlotIndex ?? 0,
                            (proxy, value) =>
                            {
                                var model = (ModelBase)proxy.Asset;
                                var mesh = GetMesh(model, capturedLodIndex, capturedMeshIndex);
                                if (mesh != null)
                                    mesh.MaterialSlotIndex = Mathf.Clamp((int)value, 0, Math.Max(0, model.MaterialSlots.Length - 1));
                            },
                            new LimitAttribute(0, maximumSlot, 0.01f)), null,
                            "Material slot index used by this mesh. Bulk edits apply to the same LOD and mesh index in every selected model.");
                    }
                }
            }

            private void BuildMaterialsProperties(LayoutElementsContainer layout, ValueContainer parentValues, ModelBase[] models)
            {
                var group = layout.Group("Materials");
                int minSlotCount = models.Min(x => x.MaterialSlots.Length);
                int maxSlotCount = models.Max(x => x.MaterialSlots.Length);
                var member = new ScriptType(typeof(ModelImportAssetPropertiesProxy)).GetField(nameof(ModelImportAssetPropertiesProxy.Materials));
                if (models.Length == 1)
                {
                    group.Object(new ValueContainer(member, parentValues));
                    return;
                }

                if (minSlotCount != maxSlotCount)
                {
                    group.Label($"Material slot counts differ ({minSlotCount}-{maxSlotCount}). Normalize the layouts before editing them as one collection.");
                    var normalize = group.Button("Use First Model's Material Layout");
                    normalize.Button.Clicked += () =>
                    {
                        var first = (ModelImportAssetPropertiesProxy)parentValues[0];
                        using (new UndoMultiBlock(Presenter.Undo, Presenter.GetUndoObjects(Presenter), "Normalize model material slots"))
                        {
                            for (int i = 1; i < parentValues.Count; i++)
                            {
                                var proxy = (ModelImportAssetPropertiesProxy)parentValues[i];
                                proxy.Materials = CloneMaterials(first.Materials);
                                proxy.SyncEditorProperties();
                            }
                        }
                        Presenter.OnModified();
                        Presenter.BuildLayoutOnUpdate();
                    };
                    return;
                }

                group.Label("Bulk edits apply each material slot by index. Structural add/remove/reorder is available for a single-model selection.");
                for (int slotIndex = 0; slotIndex < minSlotCount; slotIndex++)
                {
                    int capturedSlotIndex = slotIndex;
                    var first = ((ModelImportAssetPropertiesProxy)parentValues[0]).Materials[slotIndex];
                    var slotGroup = group.Group($"[{slotIndex}] {first.Name}");
                    slotGroup.Property("Name", CreateModelValue(parentValues, typeof(string),
                        proxy => proxy.Materials[capturedSlotIndex].Name,
                        (proxy, value) => proxy.Materials[capturedSlotIndex].Name = (string)value));
                    slotGroup.Property("Material", CreateModelValue(parentValues, typeof(MaterialBase),
                        proxy => proxy.Materials[capturedSlotIndex].Material,
                        (proxy, value) => proxy.Materials[capturedSlotIndex].Material = (MaterialBase)value,
                        new AssetReferenceAttribute(true)));
                    slotGroup.Property("Shadows Mode", CreateModelValue(parentValues, typeof(ShadowsCastingMode),
                        proxy => proxy.Materials[capturedSlotIndex].ShadowsMode,
                        (proxy, value) => proxy.Materials[capturedSlotIndex].ShadowsMode = (ShadowsCastingMode)value));
                }
            }

            private void BuildUVProperties(LayoutElementsContainer layout, ValueContainer parentValues, ModelImportAssetPropertiesProxy[] proxies, ModelBase[] models)
            {
                var group = layout.Group("UVs");
                if (models.Length != 1)
                {
                    group.Label("UV layout preview is available when a single model is selected. UV import settings remain bulk-editable below.", TextAlignment.Center);
                    return;
                }

                var proxy = proxies[0];
                group.Property("Preview UV Channel", CreateModelValue(parentValues, typeof(ModelUVChannel),
                    value => value.UVChannel,
                    (value, channel) => value.UVChannel = (ModelUVChannel)channel));
                group.Property("LOD", CreateModelValue(parentValues, typeof(int),
                    value => value.UVLOD,
                    (value, lod) => value.UVLOD = Mathf.Clamp((int)lod, 0, Math.Max(0, models[0].LODsCount - 1)),
                    new LimitAttribute(0, Math.Max(0, models[0].LODsCount - 1), 0.01f)));
                group.Property("Mesh", CreateModelValue(parentValues, typeof(int),
                    value => value.UVMesh,
                    (value, mesh) => value.UVMesh = (int)mesh,
                    new LimitAttribute(-1, 1000000, 0.01f)), null,
                    "Mesh index to preview. Use -1 to display all meshes.");

                _modelUVPreview = group.Custom<ModelUVPreviewControl>().CustomControl;
                _modelUVPreview.Model = models[0];
                UpdateModelUVPreview(proxy);
            }

            private void BuildRetargetProperties(LayoutElementsContainer layout, ValueContainer parentValues, ModelImportAssetPropertiesProxy[] proxies, ModelBase[] models)
            {
                if (models[0] is not SkinnedModel)
                    return;

                var group = layout.Group("Retarget");
                bool compatible = models.Cast<SkinnedModel>().Skip(1).All(x => HaveCompatibleSkeletons((SkinnedModel)models[0], x));
                if (!compatible)
                {
                    group.Label("The selected skinned models have different skeleton structures. Retarget setups cannot be edited as one bulk value.", TextAlignment.Center);
                    return;
                }

                group.Label("Source Asset accepts a Skinned Model or Animation. Skeleton is used when the source is an Animation.");
                group.Property("Setup Count", CreateModelValue(parentValues, typeof(int),
                    proxy => proxy.RetargetSetups?.Length ?? 0,
                    (proxy, value) =>
                    {
                        proxy.RetargetSetups = ResizeRetargetSetups(proxy.RetargetSetups, Mathf.Clamp((int)value, 0, 128));
                        Presenter.BuildLayoutOnUpdate();
                    },
                    new LimitAttribute(0, 128, 0.01f)));

                int setupCount = proxies.Min(x => x.RetargetSetups?.Length ?? 0);
                int maximumSetupCount = proxies.Max(x => x.RetargetSetups?.Length ?? 0);
                if (setupCount != maximumSetupCount)
                    group.Label($"Setup counts differ ({setupCount}-{maximumSetupCount}); only common setup indices are shown until a shared count is entered.");

                var targetNodes = ((SkinnedModel)models[0]).Nodes;
                for (int setupIndex = 0; setupIndex < setupCount; setupIndex++)
                {
                    int capturedSetupIndex = setupIndex;
                    var setupGroup = group.Group($"Setup {setupIndex}");
                    setupGroup.Property("Source Asset", CreateModelValue(parentValues, typeof(Asset),
                        proxy => proxy.RetargetSetups[capturedSetupIndex].SourceAsset,
                        (proxy, value) => proxy.RetargetSetups[capturedSetupIndex].SourceAsset = (Asset)value,
                        new AssetReferenceAttribute(true)));
                    setupGroup.Property("Skeleton", CreateModelValue(parentValues, typeof(SkinnedModel),
                        proxy => proxy.RetargetSetups[capturedSetupIndex].Skeleton,
                        (proxy, value) => proxy.RetargetSetups[capturedSetupIndex].Skeleton = (SkinnedModel)value,
                        new AssetReferenceAttribute(true)));

                    var mappingGroup = setupGroup.Group("Node Mapping");
                    for (int nodeIndex = 0; nodeIndex < targetNodes.Length; nodeIndex++)
                    {
                        string nodeName = targetNodes[nodeIndex].Name;
                        mappingGroup.Property(nodeName, CreateModelValue(parentValues, typeof(string),
                            proxy => GetRetargetNode(proxy.RetargetSetups[capturedSetupIndex], nodeName),
                            (proxy, value) => proxy.RetargetSetups[capturedSetupIndex].NodesMapping[nodeName] = (string)value));
                    }
                }
            }

            public override void Refresh()
            {
                base.Refresh();
                if (_modelUVPreview != null && Values.Count == 1 && Values[0] is ModelImportAssetPropertiesProxy proxy)
                    UpdateModelUVPreview(proxy);
            }

            protected override void Deinitialize()
            {
                _modelUVPreview = null;
                base.Deinitialize();
            }

            private void UpdateModelUVPreview(ModelImportAssetPropertiesProxy proxy)
            {
                _modelUVPreview.Channel = proxy.UVChannel switch
                {
                    ModelUVChannel.TexCoord0 => 0,
                    ModelUVChannel.TexCoord1 => 1,
                    ModelUVChannel.TexCoord2 => 2,
                    ModelUVChannel.TexCoord3 => 3,
                    ModelUVChannel.LightmapUVs => GetLightmapUVChannel((ModelBase)proxy.Asset),
                    _ => -1,
                };
                _modelUVPreview.LOD = proxy.UVLOD;
                _modelUVPreview.Mesh = proxy.UVMesh;
            }

            private static int GetLightmapUVChannel(ModelBase model)
            {
                model.GetMeshes(out var meshes);
                for (int i = 0; i < meshes.Length; i++)
                {
                    if (meshes[i] is Mesh mesh && mesh.HasLightmapUVs)
                        return mesh.LightmapUVsIndex;
                }
                return -1;
            }

            private static bool HaveCompatibleSkeletons(SkinnedModel first, SkinnedModel second)
            {
                var firstNodes = first.Nodes;
                var secondNodes = second.Nodes;
                if (firstNodes.Length != secondNodes.Length)
                    return false;
                for (int i = 0; i < firstNodes.Length; i++)
                {
                    if (firstNodes[i].ParentIndex != secondNodes[i].ParentIndex || !string.Equals(firstNodes[i].Name, secondNodes[i].Name, StringComparison.Ordinal))
                        return false;
                }
                return true;
            }

            private static ModelMaterialProperties[] CloneMaterials(ModelMaterialProperties[] source)
            {
                if (source == null)
                    return Array.Empty<ModelMaterialProperties>();
                var result = new ModelMaterialProperties[source.Length];
                for (int i = 0; i < result.Length; i++)
                {
                    result[i] = new ModelMaterialProperties
                    {
                        Name = source[i]?.Name,
                        Material = source[i]?.Material,
                        ShadowsMode = source[i]?.ShadowsMode ?? ShadowsCastingMode.All,
                    };
                }
                return result;
            }

            private static RetargetSetupProperties[] ResizeRetargetSetups(RetargetSetupProperties[] source, int count)
            {
                source ??= Array.Empty<RetargetSetupProperties>();
                if (source.Length == count)
                    return source;
                var result = new RetargetSetupProperties[count];
                int copyCount = Math.Min(source.Length, count);
                Array.Copy(source, result, copyCount);
                for (int i = copyCount; i < count; i++)
                {
                    result[i] = new RetargetSetupProperties
                    {
                        NodesMapping = new System.Collections.Generic.Dictionary<string, string>(),
                    };
                }
                return result;
            }

            private static string GetRetargetNode(RetargetSetupProperties setup, string nodeName)
            {
                setup.NodesMapping ??= new System.Collections.Generic.Dictionary<string, string>();
                return setup.NodesMapping.TryGetValue(nodeName, out var value) ? value : string.Empty;
            }

            private static MeshBase GetMesh(ModelBase model, int lodIndex, int meshIndex)
            {
                model.GetMeshes(out var meshes, lodIndex);
                return meshIndex >= 0 && meshIndex < meshes.Length ? meshes[meshIndex] : null;
            }

            private static CustomValueContainer CreateModelValue(ValueContainer parentValues, Type valueType,
                Func<ModelImportAssetPropertiesProxy, object> getter,
                Action<ModelImportAssetPropertiesProxy, object> setter,
                params object[] attributes)
            {
                var values = new CustomValueContainer(new ScriptType(valueType),
                    (instance, index) => getter((ModelImportAssetPropertiesProxy)instance),
                    (instance, index, value) => setter((ModelImportAssetPropertiesProxy)instance, value),
                    attributes.Length != 0 ? attributes : null);
                for (int i = 0; i < parentValues.Count; i++)
                    values.Add(getter((ModelImportAssetPropertiesProxy)parentValues[i]));
                return values;
            }
        }

        private sealed class ModelUVPreviewControl : RenderToTextureControl
        {
            private readonly MeshDataCache _meshData = new MeshDataCache();
            private ModelBase _model;
            private int _channel = -1;
            private int _lod;
            private int _mesh = -1;

            public ModelBase Model
            {
                set
                {
                    if (_model == value)
                        return;
                    _model = value;
                    Invalidate();
                }
            }

            public int Channel
            {
                set
                {
                    if (_channel == value)
                        return;
                    _channel = value;
                    Visible = value >= 0;
                    Invalidate();
                }
            }

            public int LOD
            {
                set
                {
                    if (_lod == value)
                        return;
                    _lod = value;
                    Invalidate();
                }
            }

            public int Mesh
            {
                set
                {
                    if (_mesh == value)
                        return;
                    _mesh = value;
                    Invalidate();
                }
            }

            public ModelUVPreviewControl()
            {
                Offsets = new Margin(4);
                AutomaticInvalidate = false;
                Visible = false;
            }

            public override void DrawSelf()
            {
                base.DrawSelf();
                var bounds = new Rectangle(Float2.Zero, Size);
                if (_channel < 0 || !_model || bounds.Size.MaxValue < 5.0f)
                    return;
                if (!_meshData.RequestMeshData(_model))
                {
                    Invalidate();
                    Render2D.DrawText(Style.Current.FontMedium, "Loading...", bounds, Color.White, TextAlignment.Center, TextAlignment.Center);
                    return;
                }

                var meshData = _meshData.MeshDatas;
                if (meshData == null || meshData.Length == 0)
                    return;
                int lodIndex = Mathf.Clamp(_lod, 0, meshData.Length - 1);
                var lod = meshData[lodIndex];
                if (lod == null || lod.Length == 0)
                    return;

                Render2D.PushClip(bounds);
                int meshIndex = Mathf.Clamp(_mesh, -1, lod.Length - 1);
                if (meshIndex == -1)
                {
                    for (int i = 0; i < lod.Length; i++)
                        DrawMeshUVs(ref lod[i], ref bounds);
                }
                else
                {
                    DrawMeshUVs(ref lod[meshIndex], ref bounds);
                }
                Render2D.PopClip();
            }

            private void DrawMeshUVs(ref MeshDataCache.MeshData meshData, ref Rectangle bounds)
            {
                if (meshData.IndexBuffer == null || meshData.VertexAccessor == null)
                {
                    Render2D.DrawText(Style.Current.FontMedium, "Missing mesh data", bounds, Color.Red, TextAlignment.Center, TextAlignment.Center);
                    return;
                }
                var texCoordStream = meshData.VertexAccessor.TexCoord(_channel);
                if (!texCoordStream.IsValid)
                {
                    Render2D.DrawText(Style.Current.FontMedium, "Missing texcoords channel", bounds, Color.Yellow, TextAlignment.Center, TextAlignment.Center);
                    return;
                }

                var scale = bounds.Size;
                for (int i = 0; i < meshData.IndexBuffer.Length; i += 3)
                {
                    var uv0 = texCoordStream.GetFloat2((int)meshData.IndexBuffer[i]) * scale;
                    var uv1 = texCoordStream.GetFloat2((int)meshData.IndexBuffer[i + 1]) * scale;
                    var uv2 = texCoordStream.GetFloat2((int)meshData.IndexBuffer[i + 2]) * scale;
                    if (Float2.TriangleArea(ref uv0, ref uv1, ref uv2) <= 10.0f)
                        continue;
                    Render2D.DrawLine(uv0, uv1, Color.White);
                    Render2D.DrawLine(uv1, uv2, Color.White);
                    Render2D.DrawLine(uv2, uv0, Color.White);
                }
            }

            protected override void OnSizeChanged()
            {
                Height = Width;
                base.OnSizeChanged();
            }

            protected override void OnVisibleChanged()
            {
                base.OnVisibleChanged();
                Parent?.PerformLayout();
                Height = Width;
            }

            public override void OnDestroy()
            {
                _meshData.Dispose();
                base.OnDestroy();
            }
        }
    }
}
