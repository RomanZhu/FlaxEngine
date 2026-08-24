// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Threading.Tasks;
using System.Xml;
using FlaxEditor.Content;
using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Editors;
using FlaxEditor.CustomEditors.Elements;
using FlaxEditor.GUI;
using FlaxEditor.History;
using FlaxEditor.Viewport.Cameras;
using FlaxEditor.Viewport.Previews;
using FlaxEngine;
using FlaxEngine.GUI;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Windows.Assets
{
    /// <summary>
    /// Editor window to view/modify <see cref="CollisionData"/> asset.
    /// </summary>
    /// <seealso cref="CollisionData" />
    /// <seealso cref="FlaxEditor.Windows.Assets.AssetEditorWindow" />
    public sealed class CollisionDataWindow : AssetEditorWindowBase<CollisionData>, IUndoLinkedActionProvider
    {
        [Flags]
        private enum MaterialSlotsMask : uint
        {
            // @formatter:off
            Slot0=1u<<0,Slot1=1u<<1,Slot2=1u<<2,Slot3=1u<<3,Slot4=1u<<4,Slot5=1u<<5,Slot6=1u<<6,Slot7=1u<<7,Slot8=1u<<8,Slot9=1u<<9,Slot10=1u<<10,Slot11=1u<<11,Slot12=1u<<12,Slot13=1u<<13,Slot14=1u<<14,Slot15=1u<<15,Slot16=1u<<16,Slot17=1u<<17,Slot18=1u<<18,Slot19=1u<<19,Slot20=1u<<20,Slot21=1u<<21,Slot22=1u<<22,Slot23=1u<<23,Slot24=1u<<24,Slot25=1u<<25,Slot26=1u<<26,Slot27=1u<<27,Slot28=1u<<28,Slot29=1u<<29,Slot30=1u<<30,Slot31=1u<<31,
            // @formatter:on
            All = uint.MaxValue,
        }

        private struct CollisionDataOptionsState
        {
            public const long SizeInBytes = 36;

            public CollisionDataType Type;
            public Guid ModelId;
            public int ModelLodIndex;
            public uint MaterialSlotsMaskValue;
            public ConvexMeshGenerationFlags ConvexFlags;
            public int ConvexVertexLimit;

            public bool HasSameValues(CollisionDataOptionsState other)
            {
                return Type == other.Type &&
                       ModelId == other.ModelId &&
                       ModelLodIndex == other.ModelLodIndex &&
                       MaterialSlotsMaskValue == other.MaterialSlotsMaskValue &&
                       ConvexFlags == other.ConvexFlags &&
                       ConvexVertexLimit == other.ConvexVertexLimit;
            }
        }

        private sealed class CollisionDataOptionsUndoAction : IUndoAction, IUndoActionMetadata
        {
            private readonly FlaxEditor.Editor _editor;
            private readonly Guid _assetId;
            private readonly string _assetPath;
            private readonly string _assetName;
            private readonly string _ownerTypeName;
            private readonly string _editorTypeName;
            private readonly string _actionString;
            private readonly CollisionDataOptionsState _oldState;
            private readonly CollisionDataOptionsState _newState;

            public CollisionDataOptionsUndoAction(CollisionDataWindow window, string actionString, CollisionDataOptionsState oldState, CollisionDataOptionsState newState)
            {
                _editor = window.Editor;
                _assetId = window.Item.ID;
                _assetPath = window.Item.Path;
                _assetName = window.Item.ShortName;
                _ownerTypeName = window.Item.GetType().FullName;
                _editorTypeName = window.GetType().FullName;
                _actionString = string.IsNullOrEmpty(actionString) ? "Edit collision data options" : actionString;
                _oldState = oldState;
                _newState = newState;
            }

            /// <inheritdoc />
            public string ActionString => _actionString;

            /// <inheritdoc />
            public UndoActionInfo ActionInfo => new UndoActionInfo
            {
                Operation = ActionString,
                TargetType = UndoActionTargetType.Asset,
                TargetName = _assetName,
                TargetPath = _assetPath,
                TargetId = _assetId,
                OwnerTypeName = _ownerTypeName,
                OwnerPath = _assetPath,
                OwnerId = _assetId,
                DisplayEditorTypeName = _editorTypeName,
                Flags = UndoActionFlags.RequiresReopen,
                ReplayPolicy = UndoActionReplayPolicy.Reopen,
                SizeInBytes = CollisionDataOptionsState.SizeInBytes * 2,
            };

            /// <inheritdoc />
            public void Do()
            {
                Apply(_newState);
            }

            /// <inheritdoc />
            public void Undo()
            {
                Apply(_oldState);
            }

            private void Apply(CollisionDataOptionsState state)
            {
                var window = ResolveWindow();
                if (window == null)
                    return;

                window.ApplyCollisionDataOptionsState(state);
            }

            private CollisionDataWindow ResolveWindow()
            {
                if (_editor == null)
                    return null;

                ContentItem item = null;
                if (_assetId != Guid.Empty)
                    item = _editor.ContentDatabase.FindAsset(_assetId);
                if (item == null && !string.IsNullOrEmpty(_assetPath))
                    item = _editor.ContentDatabase.Find(_assetPath);
                if (item == null)
                {
                    FlaxEditor.Editor.LogWarning("Cannot restore collision data undo. Missing item: " + _assetPath);
                    return null;
                }

                var window = _editor.ContentEditing.Open(item) as CollisionDataWindow;
                if (window == null)
                {
                    FlaxEditor.Editor.LogWarning("Cannot restore collision data undo. Missing collision data editor for item: " + item.Path);
                    return null;
                }
                return window;
            }

            /// <inheritdoc />
            public void Dispose()
            {
            }
        }

        /// <summary>
        /// The asset properties proxy object.
        /// </summary>
        [CustomEditor(typeof(Editor))]
        private sealed class PropertiesProxy
        {
            private CollisionDataWindow Window;
            private CollisionData Asset;
            private bool _isCooking;

            public bool CanCook => Window != null && Asset != null && !_isCooking && Type != CollisionDataType.None;

            [EditorOrder(0), EditorDisplay("General"), Tooltip("Type of the collision data to use")]
            public CollisionDataType Type;

            [EditorOrder(10), EditorDisplay("General"), Tooltip("Source model asset to use for collision data generation")]
            public ModelBase Model;

            [EditorOrder(20), Limit(0, 5), EditorDisplay("General", "Model LOD Index"), Tooltip("Source model LOD index to use for collision data generation (will be clamped to the actual model LODs collection size)")]
            public int ModelLodIndex;

            [EditorOrder(30), EditorDisplay("General"), Tooltip("The source model material slots mask. One bit per-slot. Can be used to exclude particular material slots from collision cooking.")]
            public MaterialSlotsMask MaterialSlotsMask = MaterialSlotsMask.All;

            [EditorOrder(100), EditorDisplay("Convex Mesh", "Convex Flags"), Tooltip("Convex mesh generation flags")]
            public ConvexMeshGenerationFlags ConvexFlags;

            [EditorOrder(110), Limit(8, 255), EditorDisplay("Convex Mesh", "Vertex Limit"), Tooltip("Convex mesh vertex count limit")]
            public int ConvexVertexLimit;

            public class Editor : GenericEditor
            {
                private ButtonElement _cookButton;

                /// <inheritdoc />
                public override void Initialize(LayoutElementsContainer layout)
                {
                    base.Initialize(layout);

                    layout.Space(10);
                    _cookButton = layout.Button("Cook");
                    _cookButton.Button.Clicked += OnCookButtonClicked;
                }

                /// <inheritdoc />
                public override void Refresh()
                {
                    if (_cookButton != null && Values.Count == 1)
                    {
                        var p = (PropertiesProxy)Values[0];
                        if (p._isCooking)
                        {
                            _cookButton.Button.Enabled = false;
                            _cookButton.Button.Text = "Cooking...";
                        }
                        else
                        {
                            _cookButton.Button.Enabled = p.CanCook;
                            _cookButton.Button.Text = "Cook";
                        }
                    }

                    base.Refresh();
                }

                private void OnCookButtonClicked()
                {
                    ((PropertiesProxy)Values[0]).Cook();
                }
            }

            private bool TryGetCookData(out CollisionDataType type, out ModelBase model, out int modelLodIndex, out uint materialSlotsMask, out ConvexMeshGenerationFlags convexFlags, out int convexVertexLimit)
            {
                type = Type;
                model = null;
                modelLodIndex = ModelLodIndex;
                materialSlotsMask = (uint)MaterialSlotsMask;
                convexFlags = ConvexFlags;
                convexVertexLimit = ConvexVertexLimit;

                if (Window == null || Asset == null)
                {
                    FlaxEditor.Editor.LogWarning("Cannot cook collision data. Asset is not loaded.");
                    return false;
                }
                if (_isCooking)
                {
                    FlaxEditor.Editor.LogWarning("Cannot cook collision data. Cooking is already in progress.");
                    return false;
                }
                if (Type == CollisionDataType.None)
                {
                    FlaxEditor.Editor.LogWarning("Cannot cook collision data. Missing collision type.");
                    return false;
                }

                var modelId = (object)Model != null ? Model.ID : Guid.Empty;
                if (modelId == Guid.Empty)
                {
                    FlaxEditor.Editor.LogWarning("Cannot cook collision data. Missing source model.");
                    return false;
                }

                model = FlaxEngine.Content.LoadAsync<ModelBase>(modelId);
                if (!model)
                {
                    FlaxEditor.Editor.LogWarning("Cannot cook collision data. Failed to load source model.");
                    return false;
                }

                return true;
            }

            public bool Cook()
            {
                var window = Window;
                if (!TryGetCookData(out var type, out var model, out var modelLodIndex, out var materialSlotsMask, out var convexFlags, out var convexVertexLimit))
                    return false;

                _isCooking = true;
                window._propertiesPresenter.BuildLayout();
                window.UpdateToolstrip();

                var sourcePath = window.Item.Path;
                Task.Run(() =>
                {
                    bool failed = true;
                    try
                    {
                        failed = window.Editor.ContentDatabase.SaveAsset(sourcePath, () => FlaxEditor.Editor.CookMeshCollision(sourcePath, type, model, modelLodIndex, materialSlotsMask, convexFlags, convexVertexLimit));
                    }
                    catch (Exception ex)
                    {
                        FlaxEditor.Editor.LogError("Cannot cook collision data asset.");
                        Debug.LogException(ex);
                    }
                    finally
                    {
                        FlaxEngine.Scripting.InvokeOnUpdate(() =>
                        {
                            _isCooking = false;
                            if (Window != null)
                                window.OnCookFinished(failed, model.ID);
                        });
                    }
                });
                return true;
            }

            public bool Save()
            {
                var window = Window;
                if (!TryGetCookData(out var type, out var model, out var modelLodIndex, out var materialSlotsMask, out var convexFlags, out var convexVertexLimit))
                    return false;

                _isCooking = true;
                window._propertiesPresenter.BuildLayout();
                window.UpdateToolstrip();

                bool failed = true;
                try
                {
                    var sourcePath = window.Item.Path;
                    failed = Task.Run(() => window.Editor.ContentDatabase.SaveAsset(sourcePath, () => FlaxEditor.Editor.CookMeshCollision(sourcePath, type, model, modelLodIndex, materialSlotsMask, convexFlags, convexVertexLimit))).GetAwaiter().GetResult();
                }
                catch (Exception ex)
                {
                    FlaxEditor.Editor.LogError("Cannot save collision data asset.");
                    Debug.LogException(ex);
                }
                finally
                {
                    _isCooking = false;
                    if (window != null)
                        window.OnSaveFinished(failed, model.ID);
                }

                return !failed;
            }

            public void OnLoad(CollisionDataWindow window)
            {
                // Link
                Window = window;
                Asset = window.Asset;

                // Setup cooking parameters
                if (FlaxEditor.Editor.GetCollisionDataOptions(window.Item.Path, out var type, out var model, out var modelLodIndex, out var materialSlotsMask, out var convexFlags, out var convexVertexLimit))
                {
                    Type = type;
                    Model = FlaxEngine.Content.LoadAsync<ModelBase>(model);
                    ModelLodIndex = modelLodIndex;
                    MaterialSlotsMask = (MaterialSlotsMask)materialSlotsMask;
                    ConvexFlags = convexFlags;
                    ConvexVertexLimit = convexVertexLimit;
                }
                else if (Asset != null && Asset.IsLoaded)
                {
                    var options = Asset.Options;
                    Type = options.Type;
                    Model = FlaxEngine.Content.LoadAsync<ModelBase>(options.Model);
                    ModelLodIndex = options.ModelLodIndex;
                    MaterialSlotsMask = (MaterialSlotsMask)options.MaterialSlotsMask;
                    ConvexFlags = options.ConvexFlags;
                    ConvexVertexLimit = options.ConvexVertexLimit;
                }
                else
                {
                    Type = CollisionDataType.ConvexMesh;
                    Model = null;
                    ModelLodIndex = 0;
                    MaterialSlotsMask = MaterialSlotsMask.All;
                    ConvexFlags = ConvexMeshGenerationFlags.None;
                    ConvexVertexLimit = 255;
                }
                if (Type == CollisionDataType.None)
                    Type = CollisionDataType.ConvexMesh;
            }

            public CollisionDataOptionsState CaptureState()
            {
                var modelId = Guid.Empty;
                if ((object)Model != null)
                    modelId = Model.ID;
                return new CollisionDataOptionsState
                {
                    Type = Type,
                    ModelId = modelId,
                    ModelLodIndex = ModelLodIndex,
                    MaterialSlotsMaskValue = (uint)MaterialSlotsMask,
                    ConvexFlags = ConvexFlags,
                    ConvexVertexLimit = ConvexVertexLimit,
                };
            }

            public void ApplyState(CollisionDataOptionsState state)
            {
                Type = state.Type;
                Model = state.ModelId != Guid.Empty ? FlaxEngine.Content.LoadAsync<ModelBase>(state.ModelId) : null;
                ModelLodIndex = state.ModelLodIndex;
                MaterialSlotsMask = (CollisionDataWindow.MaterialSlotsMask)state.MaterialSlotsMaskValue;
                ConvexFlags = state.ConvexFlags;
                ConvexVertexLimit = state.ConvexVertexLimit;
            }

            public void OnClean()
            {
                // Unlink
                Window = null;
                Asset = null;
                _isCooking = false;
                Type = CollisionDataType.None;
                Model = null;
                ModelLodIndex = 0;
                MaterialSlotsMask = MaterialSlotsMask.All;
                ConvexFlags = ConvexMeshGenerationFlags.None;
                ConvexVertexLimit = 0;
            }
        }

        private readonly SplitPanel _split;
        private readonly CollisionDataPreview _preview;
        private readonly CustomEditorPresenter _propertiesPresenter;
        private readonly PropertiesProxy _properties;
        private readonly ToolStripButton _saveButton;
        private readonly ToolStripButton _undoButton;
        private readonly ToolStripButton _redoButton;
        private readonly ToolStripButton _cookButton;
        private readonly Undo _undo;
        private Model _collisionWiresModel;
        private StaticModel _collisionWiresShowActor;
        private bool _updateWireMesh;
        private bool _resetPreviewCamera;
        private Guid _pendingCookModel;

        private class CollisionDataPreview : ModelBasePreview
        {
            public bool ShowInfo;
            public string Info;

            /// <inheritdoc />
            public CollisionDataPreview(bool useWidgets)
            : base(useWidgets)
            {
                ViewportCamera = new FPSCamera();
                Task.ViewFlags &= ~ViewFlags.Sky & ~ViewFlags.Bloom & ~ViewFlags.EyeAdaptation;
            }

            /// <inheritdoc />
            public override void Draw()
            {
                base.Draw();

                if (ShowInfo)
                {
                    var style = Style.Current;
                    var font = style.FontMedium;
                    var pos = new Float2(10, 50);
                    if (!style.HasTextShadow)
                        Render2D.DrawText(font, Info, new Rectangle(pos + Float2.One, Size), Color.Black);
                    Render2D.DrawText(font, Info, new Rectangle(pos, Size), Color.White);
                }
            }
        }

        /// <inheritdoc />
        public CollisionDataWindow(Editor editor, AssetItem item)
        : base(editor, item)
        {
            var inputOptions = Editor.Options.Options.Input;

            // Undo
            _undo = new Undo(Editor.Undo, this);
            _undo.UndoDone += OnUndoRedo;
            _undo.RedoDone += OnUndoRedo;
            _undo.ActionDone += OnUndoRedo;

            // Toolstrip
            _saveButton = _toolstrip.AddButton(editor.Icons.Save64, Save).LinkTooltip("Cook and save", ref inputOptions.Save);
            _cookButton = (ToolStripButton)_toolstrip.AddButton(editor.Icons.Build64, () => _properties.Cook()).LinkTooltip("Cook collision data");
            _toolstrip.AddSeparator();
            _undoButton = _toolstrip.AddButton(Editor.Icons.Undo64, _undo.PerformUndo).LinkTooltip("Undo", ref inputOptions.Undo);
            _redoButton = _toolstrip.AddButton(Editor.Icons.Redo64, _undo.PerformRedo).LinkTooltip("Redo", ref inputOptions.Redo);
            _toolstrip.AddSeparator();
            _toolstrip.AddButton(editor.Icons.CenterView64, () => _preview.ResetCamera()).LinkTooltip("Show whole collision");
            var infoButton = (ToolStripButton)_toolstrip.AddButton(editor.Icons.Info64).LinkTooltip("Show Collision Data info");
            infoButton.Clicked += () =>
            {
                _preview.ShowInfo = !_preview.ShowInfo;
                infoButton.Checked = _preview.ShowInfo;
            };
            _toolstrip.AddButton(editor.Icons.Docs64, () => Platform.OpenUrl(Utilities.Constants.DocsUrl + "manual/physics/colliders/collision-data.html")).LinkTooltip("See documentation to learn more");

            // Split Panel
            _split = new SplitPanel(Orientation.Horizontal, ScrollBars.None, ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, _toolstrip.Bottom, 0),
                SplitterValue = 0.7f,
                Parent = this
            };

            // Model preview
            _preview = new CollisionDataPreview(true)
            {
                Parent = _split.Panel1
            };

            // Asset properties
            _propertiesPresenter = new CustomEditorPresenter(_undo);
            _propertiesPresenter.Panel.Parent = _split.Panel2;
            _propertiesPresenter.Modified += OnPropertiesModified;
            _properties = new PropertiesProxy();
            _propertiesPresenter.Select(_properties);

            // Setup input actions
            InputActions.Add(options => options.Undo, _undo.PerformUndo);
            InputActions.Add(options => options.Redo, _undo.PerformRedo);
        }

        IUndoAction IUndoLinkedActionProvider.CreateLinkedUndoAction(Undo sourceUndo, IUndoAction sourceAction)
        {
            if (!ReferenceEquals(sourceUndo, _undo) || UndoActionMetadata.DoesNotModifyData(sourceAction))
                return null;

            return CreateCollisionDataOptionsUndoAction(sourceAction);
        }

        private IUndoAction CreateCollisionDataOptionsUndoAction(IUndoAction sourceAction)
        {
            if (sourceAction == null || _properties == null)
                return null;

            var afterState = _properties.CaptureState();
            var beforeState = afterState;
            if (!ApplyPreviousValues(sourceAction, ref beforeState))
                return null;

            if (beforeState.HasSameValues(afterState))
                return null;
            return new CollisionDataOptionsUndoAction(this, sourceAction.ActionString, beforeState, afterState);
        }

        private bool ApplyPreviousValues(IUndoAction action, ref CollisionDataOptionsState state)
        {
            if (action is MultiUndoAction multi)
            {
                var changed = false;
                for (var i = multi.Actions.Length - 1; i >= 0; i--)
                    changed |= ApplyPreviousValues(multi.Actions[i], ref state);
                return changed;
            }
            if (!(action is UndoActionObject objectAction))
                return false;
            var data = objectAction.PrepareData();
            if (!ReferenceEquals(data.TargetInstance, _properties))
                return false;
            var changedValue = false;
            foreach (var diff in data.Diff)
                changedValue |= ApplyPreviousValue(diff.MemberPath.Path, diff.Value1, ref state);
            return changedValue;
        }

        private static bool ApplyPreviousValue(string path, object value, ref CollisionDataOptionsState state)
        {
            switch (path)
            {
            case nameof(PropertiesProxy.Type):
                state.Type = (CollisionDataType)Convert.ToInt32(value);
                return true;
            case nameof(PropertiesProxy.Model):
                var model = value as ModelBase;
                state.ModelId = model ? model.ID : Guid.Empty;
                return true;
            case nameof(PropertiesProxy.ModelLodIndex):
                state.ModelLodIndex = Convert.ToInt32(value);
                return true;
            case nameof(PropertiesProxy.MaterialSlotsMask):
                state.MaterialSlotsMaskValue = Convert.ToUInt32(value);
                return true;
            case nameof(PropertiesProxy.ConvexFlags):
                state.ConvexFlags = (ConvexMeshGenerationFlags)Convert.ToInt32(value);
                return true;
            case nameof(PropertiesProxy.ConvexVertexLimit):
                state.ConvexVertexLimit = Convert.ToInt32(value);
                return true;
            default:
                return false;
            }
        }

        private void OnPropertiesModified()
        {
            if (_collisionWiresShowActor)
                _collisionWiresShowActor.IsActive = false;
            UpdatePreviewModel();
            MarkAsEdited();
            MarkAutoSaveEdit();
        }

        private void OnUndoRedo(IUndoAction action)
        {
            if (!UndoActionMetadata.DoesNotModifyData(action))
            {
                MarkAsEdited();
                MarkAutoSaveEdit();
                _propertiesPresenter.BuildLayoutOnUpdate();
            }
            UpdateToolstrip();
        }

        private void UpdatePreviewModel()
        {
            var model = _properties?.Model;
            if (_preview.Asset == model)
                return;

            _preview.Asset = model;
            _resetPreviewCamera = model != null;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            if (_resetPreviewCamera && _preview.Asset && _preview.Asset.IsLoaded)
            {
                _resetPreviewCamera = false;
                _preview.ResetCamera();
            }

            // Sync helper actor size with actual preview model (preview scales model for better usage experience)
            if (_collisionWiresShowActor && _collisionWiresShowActor.IsActive)
            {
                _collisionWiresShowActor.Transform = _preview.StaticModel.Transform;
            }

            base.Update(deltaTime);
        }

        /// <inheritdoc />
        protected override void UpdateToolstrip()
        {
            if (_saveButton != null)
                _saveButton.Enabled = _properties != null && _properties.CanCook;
            if (_undoButton != null)
                _undoButton.Enabled = _undo.CanUndo;
            if (_redoButton != null)
                _redoButton.Enabled = _undo.CanRedo;
            if (_cookButton != null)
                _cookButton.Enabled = _properties != null && _properties.CanCook;

            base.UpdateToolstrip();
        }

        /// <inheritdoc />
        public override bool CanRunAutoSave => false;

        /// <inheritdoc />
        public override void Save()
        {
            _properties.Save();
        }

        private void ApplyCollisionDataOptionsState(CollisionDataOptionsState state)
        {
            _properties.ApplyState(state);
            _propertiesPresenter.BuildLayout();
            MarkAsEdited();
            MarkAutoSaveEdit();
            UpdateToolstrip();
        }

        private void OnCookFinished(bool failed, Guid modelId)
        {
            _propertiesPresenter.BuildLayout();
            UpdateToolstrip();
            if (!failed)
            {
                ClearEditedFlag();
                _pendingCookModel = modelId;
            }
        }

        private void OnSaveFinished(bool failed, Guid modelId)
        {
            _propertiesPresenter.BuildLayout();
            UpdateToolstrip();
            if (!failed)
            {
                ClearEditedFlag();
                _pendingCookModel = modelId;
            }
        }

        /// <summary>
        /// Updates the collision data debug model.
        /// </summary>
        private void UpdateWiresModel()
        {
            // Don't update on a importer/worker thread
            if (Platform.CurrentThreadID != Globals.MainThreadID)
            {
                _updateWireMesh = true;
                return;
            }

            if (_collisionWiresModel == null)
            {
                _collisionWiresModel = FlaxEngine.Content.CreateVirtualAsset<Model>();
                _collisionWiresModel.SetupLODs(new[] { 1 });
            }
            Editor.Internal_GetCollisionWires(FlaxEngine.Object.GetUnmanagedPtr(Asset), out var triangles, out var indices, out var triangleCount, out var indicesCount);
            var hasWires = triangles != null && indices != null;
            if (hasWires)
                _collisionWiresModel.LODs[0].Meshes[0].UpdateMesh(triangles, indices);
            else
                Editor.LogWarning("Failed to get collision wires for " + Asset);
            if (_collisionWiresShowActor == null)
            {
                _collisionWiresShowActor = new StaticModel();
                _preview.Task.AddCustomActor(_collisionWiresShowActor);
            }
            _collisionWiresShowActor.Model = _collisionWiresModel;
            _collisionWiresShowActor.IsActive = hasWires;
            _collisionWiresShowActor.SetMaterial(0, FlaxEngine.Content.LoadAsyncInternal<MaterialBase>(EditorAssets.WiresDebugMaterial));
            _preview.Info = string.Format("\nTriangles: {0:N0}\nVertices: {1:N0}\nMemory Size: {2}", triangleCount, indicesCount / 3, Utilities.Utils.FormatBytesCount(Asset.MemoryUsage));
            UpdatePreviewModel();
        }

        /// <inheritdoc />
        protected override void UnlinkItem()
        {
            _properties.OnClean();
            _preview.Asset = null;

            base.UnlinkItem();
        }

        /// <inheritdoc />
        protected override void OnAssetLinked()
        {
            _preview.Asset = null;
            UpdateToolstrip();

            base.OnAssetLinked();
        }

        /// <inheritdoc />
        protected override void OnAssetLoaded()
        {
            _properties.OnLoad(this);
            _propertiesPresenter.BuildLayout();
            _undo.Clear();
            ClearEditedFlag();
            UpdateToolstrip();
            UpdateWiresModel();

            base.OnAssetLoaded();
        }

        /// <inheritdoc />
        protected override void OnAssetLoadFailed()
        {
            _properties.OnLoad(this);
            _propertiesPresenter.BuildLayout();
            _undo.Clear();
            ClearEditedFlag();
            UpdateToolstrip();

            base.OnAssetLoadFailed();
        }

        /// <inheritdoc />
        public override void OnItemReimported(ContentItem item)
        {
            // Refresh the properties (will get new data in OnAssetLoaded)
            _properties.OnClean();
            _propertiesPresenter.BuildLayout();
            _undo.Clear();
            ClearEditedFlag();
            UpdateToolstrip();

            base.OnItemReimported(item);
        }

        /// <inheritdoc />
        protected override void OnClose()
        {
            _undo.Clear();

            base.OnClose();
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            if (_pendingCookModel != Guid.Empty && Asset && Asset.IsLoaded && Asset.Options.Model == _pendingCookModel)
            {
                _pendingCookModel = Guid.Empty;
                UpdateWiresModel();
            }
            if (_updateWireMesh)
            {
                _updateWireMesh = false;
                UpdateWiresModel();
            }

            base.OnUpdate();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (IsDisposing)
                return;
            base.OnDestroy();

            Object.Destroy(ref _collisionWiresShowActor);
            Object.Destroy(ref _collisionWiresModel);
        }

        /// <inheritdoc />
        public override bool UseLayoutData => true;

        /// <inheritdoc />
        public override void OnLayoutSerialize(XmlWriter writer)
        {
            LayoutSerializeSplitter(writer, "Split", _split);
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize(XmlElement node)
        {
            LayoutDeserializeSplitter(node, "Split", _split);
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize()
        {
            _split.SplitterValue = 0.7f;
        }
    }
}
