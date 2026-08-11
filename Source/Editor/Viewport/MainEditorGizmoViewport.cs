// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using Object = FlaxEngine.Object;
using FlaxEditor.Content;
using FlaxEditor.Gizmo;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.Options;
using FlaxEditor.SceneGraph;
using FlaxEditor.Scripting;
using FlaxEditor.Tools.CSG;
using FlaxEditor.Viewport.Cameras;
using FlaxEditor.Viewport.Modes;
using FlaxEditor.Viewport.Widgets;
using FlaxEditor.Windows;
using FlaxEngine;
using FlaxEngine.Gizmo;
using FlaxEngine.GUI;

namespace FlaxEditor.Viewport
{
    /// <summary>
    /// Main editor gizmo viewport used by the <see cref="EditGameWindow"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.Viewport.EditorGizmoViewport" />
    public class MainEditorGizmoViewport : EditorGizmoViewport, IEditorPrimitivesOwner
    {
        private readonly Editor _editor;
        private readonly ContextMenuButton _showGridButton;
        private readonly ContextMenuButton _showNavigationButton;
        private readonly ContextMenuButton _toggleGameViewButton;
        private readonly ContextMenuButton _toggleCharacterControllerModeButton;
        private readonly ContextMenuButton _showDirectionGizmoButton;
        private ToolStripButton _overlayGridButton;
        private ToolStripButton _overlayNavigationButton;
        private ToolStripButton _overlayGameViewButton;
        private ToolStripButton _overlayCharacterControllerModeButton;
        private ToolStripButton _overlayModeButton;
        private ToolStripButton _overlaySelectModeButton;
        private ToolStripButton _overlayTranslateModeButton;
        private ToolStripButton _overlayRotateModeButton;
        private ToolStripButton _overlayScaleModeButton;
        private ToolStripButton _overlayBoundsModeButton;
        private ToolStripButton _overlayTransformSpaceButton;
        private ToolStripButton _overlayPivotButton;
        private ToolStripButton _overlayAbsoluteSnapButton;
        private readonly List<SceneGraphNode> _sceneTreeHoverSelection = new List<SceneGraphNode>(1);
        private readonly List<SceneGraphNode> _viewportPrimaryHoverSelection = new List<SceneGraphNode>(1);
        private ActorNode _sceneTreeHoveredActor;
        private ActorNode _editorViewportHoveredActor;
        private bool _editorViewportHoverIsLeafTarget;
        private ToolStripButton _overlayTranslateSnapButton;
        private ToolStripButton _overlayRotateSnapButton;
        private ToolStripButton _overlayScaleSnapButton;
        private ToolStripButton _overlayTranslateSnapValueButton;
        private ToolStripButton _overlayRotateSnapValueButton;
        private ToolStripButton _overlayScaleSnapValueButton;
        private ToolStripButton _overlayCSGSelectPlaceButton;
        private ToolStripButton _overlayCSGDrawButton;
        private ToolStripButton _overlayCSGEditButton;
        private ToolStripButton _overlayCSGSurfaceButton;
        private ToolStripButton _overlayCSGClipButton;
        private ToolStripButton _overlayCSGOperationButton;
        private ToolStripButton _overlayCSGPlaneButton;
        private ToolStripButton _overlayCSGSnapButton;
        private ToolStripButton _overlayCSGSnapValueButton;
        private ToolStripButton _overlayCSGVisibilityButton;
        private SelectionOutline _customSelectionOutline;
        private bool _middleMouseRecenterCandidate;
        private bool _suppressNextSelectionPick;

        /// <summary>
        /// The editor sprites rendering effect.
        /// </summary>
        /// <seealso cref="FlaxEngine.PostProcessEffect" />
        [HideInEditor]
        public class EditorSpritesRenderer : PostProcessEffect
        {
            /// <summary>
            /// The rendering task.
            /// </summary>
            public SceneRenderTask Task;

            /// <inheritdoc />
            public EditorSpritesRenderer()
            {
                Order = -100000 + 1; // Draw after grid
                UseSingleTarget = true;
            }

            /// <inheritdoc />
            public override bool CanRender()
            {
                return (Task.View.Flags & ViewFlags.EditorSprites) == ViewFlags.EditorSprites && Level.ScenesCount != 0 && base.CanRender();
            }

            /// <inheritdoc />
            public override void Render(GPUContext context, ref RenderContext renderContext, GPUTexture input, GPUTexture output)
            {
                Profiler.BeginEventGPU("Editor Primitives");

                // Prepare
                var renderList = RenderList.GetFromPool();
                var prevList = renderContext.List;
                renderContext.List = renderList;
                renderContext.View.Pass = DrawPass.Forward;

                // Bind output
                float width = input.Width;
                float height = input.Height;
                context.SetViewport(width, height);
                var depthBuffer = renderContext.Buffers.DepthBuffer;
                var depthBufferHandle = depthBuffer.View();
                if ((depthBuffer.Flags & GPUTextureFlags.ReadOnlyDepthView) == GPUTextureFlags.ReadOnlyDepthView)
                    depthBufferHandle = depthBuffer.ViewReadOnlyDepth();
                context.SetRenderTarget(depthBufferHandle, input.View());

                // Collect draw calls
                Draw(ref renderContext);

                // Sort draw calls
                renderList.SortDrawCalls(ref renderContext, true, DrawCallsListType.Forward);

                // Perform the rendering
                renderList.ExecuteDrawCalls(ref renderContext, DrawCallsListType.Forward);

                // Cleanup
                RenderList.ReturnToPool(renderList);
                renderContext.List = prevList;

                Profiler.EndEventGPU();
            }

            /// <summary>
            /// Draws the icons.
            /// </summary>
            protected virtual void Draw(ref RenderContext renderContext)
            {
                for (int i = 0; i < Level.ScenesCount; i++)
                {
                    var scene = Level.GetScene(i);
                    ViewportIconsRenderer.DrawIcons(ref renderContext, scene);
                }
            }
        }

        private bool _lockedFocus;
        private double _lockedFocusOffset;
        private readonly ViewportDebugDrawData _debugDrawData = new ViewportDebugDrawData(32);
        private EditorSpritesRenderer _editorSpritesRenderer;
        private ViewportRubberBandSelector _rubberBandSelector;
        private DirectionGizmo _directionGizmo;

        private bool _gameViewActive;
        private ViewFlags _preGameViewFlags;
        private ViewMode _preGameViewViewMode;
        private bool _gameViewWasGridShown;
        private bool _gameViewWasFpsCounterShown;
        private bool _gameViewWasNavigationShown;
        private InSceneCharacterControllerCamera _characterControllerCamera;
        private ViewportCamera _preCharacterControllerCamera;
        private bool _characterControllerModeActive;
        private bool _characterControllerControlMouseActive;
        private bool _characterControllerWasOrthographic;
        private bool _characterControllerWasTransformGizmoVisible;
        private bool _characterControllerWasSelectionOutlineShown;

        /// <summary>
        /// Drag and drop handlers
        /// </summary>
        public readonly ViewportDragHandlers DragHandlers;

        /// <summary>
        /// The transform gizmo.
        /// </summary>
        public readonly TransformGizmo TransformGizmo;

        /// <summary>
        /// The grid gizmo.
        /// </summary>
        public readonly GridGizmo Grid;

        /// <summary>
        /// The selection outline postFx.
        /// </summary>
        public SelectionOutline SelectionOutline;

        /// <summary>
        /// The scene tree hover outline postFx.
        /// </summary>
        public SelectionOutline SceneTreeHoverOutline;

        /// <summary>
        /// The primary viewport hover outline postFx.
        /// </summary>
        public SelectionOutline ViewportHoverOutline;

        /// <summary>
        /// The editor primitives postFx.
        /// </summary>
        public EditorPrimitives EditorPrimitives;

        /// <summary>
        /// Gets or sets a value indicating whether draw <see cref="DebugDraw"/> shapes.
        /// </summary>
        public bool DrawDebugDraw = true;

        /// <summary>
        /// Gets the debug draw data for the viewport.
        /// </summary>
        public ViewportDebugDrawData DebugDrawData => _debugDrawData;

        /// <summary>
        /// Gets or sets a value indicating whether show navigation mesh.
        /// </summary>
        public bool ShowNavigation
        {
            get => _showNavigationButton.Checked;
            set
            {
                _showNavigationButton.Checked = value;
                if (_overlayNavigationButton != null)
                    _overlayNavigationButton.Checked = value;
            }
        }

        /// <summary>
        /// The sculpt terrain gizmo.
        /// </summary>
        public Tools.Terrain.SculptTerrainGizmoMode SculptTerrainGizmo;

        /// <summary>
        /// The paint terrain gizmo.
        /// </summary>
        public Tools.Terrain.PaintTerrainGizmoMode PaintTerrainGizmo;

        /// <summary>
        /// The edit terrain gizmo.
        /// </summary>
        public Tools.Terrain.EditTerrainGizmoMode EditTerrainGizmo;

        /// <summary>
        /// The paint foliage gizmo.
        /// </summary>
        public Tools.Foliage.PaintFoliageGizmoMode PaintFoliageGizmo;

        /// <summary>
        /// The edit foliage gizmo.
        /// </summary>
        public Tools.Foliage.EditFoliageGizmoMode EditFoliageGizmo;

        /// <summary>
        /// The CSG authoring gizmo mode.
        /// </summary>
        public CSGAuthoringGizmoMode CSGAuthoringMode;

        /// <summary>
        /// Initializes a new instance of the <see cref="MainEditorGizmoViewport"/> class.
        /// </summary>
        /// <param name="editor">Editor instance.</param>
        public MainEditorGizmoViewport(Editor editor)
        : base(Object.New<SceneRenderTask>(), editor.Undo, editor.Scene.Root)
        {
            _editor = editor;
            var inputOptions = _editor.Options.Options.Input;
            DragHandlers = new ViewportDragHandlers(this, this, ValidateDragItem, ValidateDragActorType, ValidateDragScriptItem);

            // Prepare rendering task
            Task.ActorsSource = ActorsSources.Scenes;
            Task.ViewFlags = ViewFlags.DefaultEditor;
            Task.Begin += OnBegin;
            Task.CollectDrawCalls += OnCollectDrawCalls;
            Task.PostRender += OnPostRender;

            // Render task after the main game task so streaming and render state data will use main game task instead of editor preview
            Task.Order = 1;

            // Create post effects
            SelectionOutline = Object.New<SelectionOutline>();
            SelectionOutline.SelectionGetter = () => TransformGizmo.SelectedParents;
            Task.AddCustomPostFx(SelectionOutline);
            ViewportHoverOutline = Object.New<SelectionOutline>();
            ViewportHoverOutline.UseEditorOptions = false;
            ViewportHoverOutline.ShowSelectionOutline = true;
            ViewportHoverOutline.Order = SelectionOutline.Order + 1;
            ViewportHoverOutline.SelectionOutlineColor0 = new Color(0.12f, 0.55f, 1.0f, 1.0f);
            ViewportHoverOutline.SelectionOutlineColor1 = new Color(0.05f, 0.32f, 0.72f, 1.0f);
            ViewportHoverOutline.SelectionGetter = GetViewportPrimaryHoverSelection;
            Task.AddCustomPostFx(ViewportHoverOutline);
            SceneTreeHoverOutline = Object.New<SelectionOutline>();
            SceneTreeHoverOutline.UseEditorOptions = false;
            SceneTreeHoverOutline.ShowSelectionOutline = true;
            SceneTreeHoverOutline.Order = SelectionOutline.Order + 2;
            SceneTreeHoverOutline.SelectionOutlineColor0 = new Color(0.56f, 0.56f, 0.56f, 1.0f);
            SceneTreeHoverOutline.SelectionOutlineColor1 = new Color(0.36f, 0.36f, 0.36f, 1.0f);
            SceneTreeHoverOutline.SelectionGetter = GetSceneTreeHoverSelection;
            Task.AddCustomPostFx(SceneTreeHoverOutline);
            EditorPrimitives = Object.New<EditorPrimitives>();
            EditorPrimitives.Viewport = this;
            Task.AddCustomPostFx(EditorPrimitives);
            _editorSpritesRenderer = Object.New<EditorSpritesRenderer>();
            _editorSpritesRenderer.Task = Task;
            Task.AddCustomPostFx(_editorSpritesRenderer);

            // Add transformation gizmo
            TransformGizmo = new TransformGizmo(this);
            TransformGizmo.ApplyTransformation += ApplyTransform;
            TransformGizmo.Duplicate += _editor.SceneEditing.Duplicate;
            Gizmos.Active = TransformGizmo;

            // Add rubber band selector
            _rubberBandSelector = new ViewportRubberBandSelector(this);

            // Add direction gizmo
            _directionGizmo = new DirectionGizmo(this)
            {
                AnchorPreset = AnchorPresets.TopRight,
                Parent = this,
            };

            // Add grid
            Grid = new GridGizmo(this);
            Grid.EnabledChanged += gizmo =>
            {
                _showGridButton.Icon = gizmo.Enabled ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
                if (_overlayGridButton != null)
                    _overlayGridButton.Checked = gizmo.Enabled;
            };

            editor.SceneEditing.SelectionChanged += OnSelectionChanged;
            // Gizmo widgets
            AddGizmoViewportWidgets(this, TransformGizmo, true, true);

            // Show grid widget
            _showGridButton = ViewWidgetShowMenu.AddButton("Grid", () => Grid.Enabled = !Grid.Enabled);
            _showGridButton.Icon = Style.Current.CheckBoxTick;
            _showGridButton.CloseMenuOnClick = false;

            // Show navigation widget
            _showNavigationButton = ViewWidgetShowMenu.AddButton("Navigation", inputOptions.ToggleNavMeshVisibility, () => ShowNavigation = !ShowNavigation);
            _showNavigationButton.CloseMenuOnClick = false;

            // Hover highlights
            ContextMenuButton objectHoverHighlights = null;
            objectHoverHighlights = ViewWidgetShowMenu.AddButton("Highlight Scene Items from Editor", () =>
            {
                _editor.Options.Options.Interface.HighlightViewportObjectHover = !_editor.Options.Options.Interface.HighlightViewportObjectHover;
                if (!_editor.Options.Options.Interface.HighlightViewportObjectHover)
                    ClearSceneTreeHoverFromEditorViewport();
                objectHoverHighlights.Checked = _editor.Options.Options.Interface.HighlightViewportObjectHover;
                _editor.Options.SaveOptions();
            });
            objectHoverHighlights.CloseMenuOnClick = false;
            objectHoverHighlights.TooltipText = "When hovered Editor items will highlight items in Scene.";
            objectHoverHighlights.Checked = _editor.Options.Options.Interface.HighlightViewportObjectHover;

            // Direction gizmo
            _showDirectionGizmoButton = ViewWidgetButtonMenu.AddButton("Direction Gizmo", () => _directionGizmo.Visible = !_directionGizmo.Visible);
            _showDirectionGizmoButton.AutoCheck = true;
            _showDirectionGizmoButton.CloseMenuOnClick = false;
            
            // Game View
            ViewWidgetButtonMenu.AddSeparator();
            _toggleGameViewButton = ViewWidgetButtonMenu.AddButton("Game View", inputOptions.ToggleGameView, ToggleGameView);
            _toggleGameViewButton.CloseMenuOnClick = false;
            _toggleCharacterControllerModeButton = ViewWidgetButtonMenu.AddButton("Character Controller", inputOptions.ToggleCharacterControllerMode, ToggleCharacterControllerMode);
            _toggleCharacterControllerModeButton.CloseMenuOnClick = false;
            _toggleCharacterControllerModeButton.LinkTooltip("Toggle in-scene character controller camera mode.");

            // Create camera widget
            ViewWidgetButtonMenu.AddSeparator();
            ViewWidgetButtonMenu.AddButton("Create camera here", CreateCameraAtView);

            // Init gizmo modes
            {
                // Add default modes used by the editor
                Gizmos.AddMode(new TransformGizmoMode());
                Gizmos.AddMode(CSGAuthoringMode = new CSGAuthoringGizmoMode());
                Gizmos.AddMode(new NoGizmoMode());
                Gizmos.AddMode(SculptTerrainGizmo = new Tools.Terrain.SculptTerrainGizmoMode());
                Gizmos.AddMode(PaintTerrainGizmo = new Tools.Terrain.PaintTerrainGizmoMode());
                Gizmos.AddMode(EditTerrainGizmo = new Tools.Terrain.EditTerrainGizmoMode());
                Gizmos.AddMode(PaintFoliageGizmo = new Tools.Foliage.PaintFoliageGizmoMode());
                Gizmos.AddMode(EditFoliageGizmo = new Tools.Foliage.EditFoliageGizmoMode());

                // Restore the explicit Object/CSG authoring context without inferring it from selection.
                if (_editor.ProjectCache.TryGetCustomData(CSGAuthoringGizmoMode.ContextCacheKey, out string context) &&
                    string.Equals(context, CSGAuthoringGizmoMode.ContextCacheValue, StringComparison.Ordinal))
                    Gizmos.SetActiveMode<CSGAuthoringGizmoMode>();
                else
                    Gizmos.SetActiveMode<TransformGizmoMode>();
            }
            Gizmos.ActiveModeChanged += OnActiveGizmoModeChanged;
            CSGAuthoringMode.Controller.Changed += UpdateViewportToolStrip;
            TransformGizmo.ModeChanged += UpdateViewportToolStrip;
            TransformGizmo.TransformSpaceChanged += UpdateViewportToolStrip;
            TransformGizmo.PivotChanged += UpdateViewportToolStrip;
            AddMainViewportToolStripButtons();

            // Setup input actions
            InputActions.Add(options => options.LockFocusSelection, LockFocusSelection);
            InputActions.Add(options => options.FocusSelection, FocusSelection);
            InputActions.Add(options => options.RotateSelection, RotateSelection);
            InputActions.Add(options => options.Delete, _editor.SceneEditing.Delete);
            InputActions.Add(options => options.ToggleNavMeshVisibility, () => ShowNavigation = !ShowNavigation);

            // View modes
            InputActions.Add(options => options.ToggleCharacterControllerMode, ToggleCharacterControllerMode);
            InputActions.Add(options => options.ToggleGameView, ToggleGameView);

            editor.Options.OptionsChanged += OnEditorOptionsChanged;
            editor.PlayModeBeginning += OnPlayModeBeginning;
            Level.SceneLoaded += OnSceneContextChanged;
            Level.SceneUnloading += OnSceneContextChanged;
            OnEditorOptionsChanged(editor.Options.Options);
        }

        private void OnActiveGizmoModeChanged(EditorGizmoMode mode)
        {
            if (mode is CSGAuthoringGizmoMode)
                _editor.ProjectCache.SetCustomData(CSGAuthoringGizmoMode.ContextCacheKey, CSGAuthoringGizmoMode.ContextCacheValue);
            else if (mode is TransformGizmoMode)
                _editor.ProjectCache.SetCustomData(CSGAuthoringGizmoMode.ContextCacheKey, "Object");
            UpdateViewportToolStrip();
        }

        private void OnSceneContextChanged(Scene scene, Guid sceneId)
        {
            Gizmos.ActiveMode?.TryCancel(EditorGizmoModeCancelReason.SceneChanged);
        }

        private void OnEditorOptionsChanged(EditorOptions options)
        {
            _directionGizmo.Visible = options.Viewport.ShowDirectionGizmo;
            _showDirectionGizmoButton.Checked = _directionGizmo.Visible;
            _directionGizmo.Size = new Float2(DirectionGizmo.DefaultGizmoSize * options.Viewport.DirectionGizmoScale);
            _directionGizmo.LocalX = -_directionGizmo.Size.X * 0.5f;
            _directionGizmo.LocalY = _directionGizmo.Size.Y * 0.5f + ViewportWidgetsContainer.WidgetsHeight;
            if (!options.Interface.HighlightViewportObjectHover)
                ClearSceneTreeHoverFromEditorViewport();
            if (!options.Interface.HighlightSceneTreeHoverInViewport)
                SetSceneTreeHoveredActor(null);
        }

        private void AddMainViewportToolStripButtons()
        {
            _overlayModeButton = AddViewportToolStripMenuButton(GetGizmoModeLabel(), SpriteHandle.Invalid, CreateGizmoModeMenu(), ToolStripAnchor.Left, "Flax.Scene.Gizmo.Mode");
            _viewportToolStrip.SetItemPlacement(_overlayModeButton, ToolStripAnchor.Left, 0, "Flax.Scene.Gizmo.Mode");
            _overlayModeButton.DrawMenuChevron = true;
            _overlayModeButton.PerformLayout();
            _overlayModeButton.LinkTooltip("Scene editing mode.");

            var inputOptions = _editor.Options.Options.Input;
            _overlaySelectModeButton = AddViewportToolStripButton("Select", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.Transform.Select.Left", () => TransformGizmo.ActiveMode = TransformGizmoBase.Mode.Select);
            _overlaySelectModeButton.LinkTooltip("Select mode.", ref inputOptions.SelectMode);
            _overlayTranslateModeButton = AddViewportToolStripButton(string.Empty, _editor.Icons.Translate32, ToolStripAnchor.Left, "Flax.Scene.Transform.Translate.Left", () => TransformGizmo.ActiveMode = TransformGizmoBase.Mode.Translate);
            _overlayTranslateModeButton.LinkTooltip("Translate gizmo mode.", ref inputOptions.TranslateMode);
            _overlayRotateModeButton = AddViewportToolStripButton(string.Empty, _editor.Icons.Rotate32, ToolStripAnchor.Left, "Flax.Scene.Transform.Rotate.Left", () => TransformGizmo.ActiveMode = TransformGizmoBase.Mode.Rotate);
            _overlayRotateModeButton.LinkTooltip("Rotate gizmo mode.", ref inputOptions.RotateMode);
            _overlayScaleModeButton = AddViewportToolStripButton(string.Empty, _editor.Icons.Scale32, ToolStripAnchor.Left, "Flax.Scene.Transform.Scale.Left", () => TransformGizmo.ActiveMode = TransformGizmoBase.Mode.Scale);
            _overlayScaleModeButton.LinkTooltip("Scale gizmo mode.", ref inputOptions.ScaleMode);
            _overlayBoundsModeButton = AddViewportToolStripButton(string.Empty, _editor.Icons.VisjectBoxClosed32, ToolStripAnchor.Left, "Flax.Scene.Transform.Bounds.Left", () => TransformGizmo.ActiveMode = TransformGizmoBase.Mode.Bounds);
            _overlayBoundsModeButton.LinkTooltip("Resize the selection bounds by dragging a face.", ref inputOptions.BoundsMode);
            _overlayTransformSpaceButton = AddViewportToolStripButton("World", _editor.Icons.Globe32, ToolStripAnchor.Left, "Flax.Scene.Transform.Space.Left", () =>
            {
                TransformGizmo.ToggleTransformSpace();
                _editor.ProjectCache.SetCustomData("TransformSpaceState", TransformGizmo.ActiveTransformSpace.ToString());
            });
            _overlayTransformSpaceButton.LinkTooltip("Toggle gizmo transform space.", ref inputOptions.ToggleTransformSpace);
            _overlayPivotButton = AddViewportToolStripButton("Center", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.Transform.Pivot.Left", () => TransformGizmo.TogglePivot());
            _overlayPivotButton.CustomizationLabel = "Pivot / Center";
            _overlayPivotButton.LinkTooltip("Toggle gizmo pivot between selection center and object pivot.", ref inputOptions.TogglePivot);
            _overlayAbsoluteSnapButton = AddViewportToolStripButton("Abs", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.Transform.AbsoluteSnap.Left", () =>
            {
                TransformGizmo.AbsoluteSnapEnabled = !TransformGizmo.AbsoluteSnapEnabled;
                _editor.ProjectCache.SetCustomData("AbsoluteSnapState", TransformGizmo.AbsoluteSnapEnabled);
            });
            _overlayAbsoluteSnapButton.LinkTooltip("Toggle absolute world-space snapping.");
            _overlayTranslateSnapButton = AddViewportToolStripButton(string.Empty, _editor.Icons.Grid32, ToolStripAnchor.Left, "Flax.Scene.Transform.TranslateSnap.Left", () =>
            {
                TransformGizmo.TranslationSnapEnable = !TransformGizmo.TranslationSnapEnable;
                _editor.ProjectCache.SetCustomData("TranslateSnapState", TransformGizmo.TranslationSnapEnable);
            });
            _overlayTranslateSnapButton.LinkTooltip("Toggle position snapping.");
            _overlayTranslateSnapValueButton = AddViewportToolStripMenuButton(GetTranslateSnapLabel(), SpriteHandle.Invalid, CreateTranslateSnapMenu(), ToolStripAnchor.Left, "Flax.Scene.Transform.TranslateSnapValue.Left");
            _overlayTranslateSnapValueButton.LinkTooltip("Position snapping values.");
            _overlayRotateSnapButton = AddViewportToolStripButton(string.Empty, _editor.Icons.RotateSnap32, ToolStripAnchor.Left, "Flax.Scene.Transform.RotateSnap.Left", () =>
            {
                TransformGizmo.RotationSnapEnabled = !TransformGizmo.RotationSnapEnabled;
                _editor.ProjectCache.SetCustomData("RotationSnapState", TransformGizmo.RotationSnapEnabled);
            });
            _overlayRotateSnapButton.LinkTooltip("Toggle rotation snapping.");
            _overlayRotateSnapValueButton = AddViewportToolStripMenuButton(GetRotateSnapLabel(), SpriteHandle.Invalid, CreateSnapValueMenu(RotateSnapValues, () => TransformGizmo.RotationSnapValue, value =>
            {
                TransformGizmo.RotationSnapValue = value;
                _editor.ProjectCache.SetCustomData("RotationSnapValue", TransformGizmo.RotationSnapValue);
            }), ToolStripAnchor.Left, "Flax.Scene.Transform.RotateSnapValue.Left");
            _overlayRotateSnapValueButton.LinkTooltip("Rotation snapping values.");
            _overlayScaleSnapButton = AddViewportToolStripButton(string.Empty, _editor.Icons.ScaleSnap32, ToolStripAnchor.Left, "Flax.Scene.Transform.ScaleSnap.Left", () =>
            {
                TransformGizmo.ScaleSnapEnabled = !TransformGizmo.ScaleSnapEnabled;
                _editor.ProjectCache.SetCustomData("ScaleSnapState", TransformGizmo.ScaleSnapEnabled);
            });
            _overlayScaleSnapButton.LinkTooltip("Toggle scale snapping.");
            _overlayScaleSnapValueButton = AddViewportToolStripMenuButton(GetScaleSnapLabel(), SpriteHandle.Invalid, CreateSnapValueMenu(ScaleSnapValues, () => TransformGizmo.ScaleSnapValue, value =>
            {
                TransformGizmo.ScaleSnapValue = value;
                _editor.ProjectCache.SetCustomData("ScaleSnapValue", TransformGizmo.ScaleSnapValue);
            }), ToolStripAnchor.Left, "Flax.Scene.Transform.ScaleSnapValue.Left");
            _overlayScaleSnapValueButton.LinkTooltip("Scale snapping values.");

            _overlayCSGSelectPlaceButton = AddViewportToolStripButton("Select", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.CSG.SelectPlace.Left", () => CSGAuthoringMode.Controller.SetTool(CSGTool.SelectPlace));
            _overlayCSGSelectPlaceButton.LinkTooltip("Select brushes or place the chosen primitive.", ref inputOptions.CSGSelectPlaceTool);
            _overlayCSGDrawButton = AddViewportToolStripButton("Draw", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.CSG.Draw.Left", () => CSGAuthoringMode.Controller.SetTool(CSGTool.Draw));
            _overlayCSGDrawButton.LinkTooltip("Draw a brush footprint and extrusion.", ref inputOptions.CSGDrawTool);
            _overlayCSGEditButton = AddViewportToolStripButton("Edit", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.CSG.Edit.Left", () => CSGAuthoringMode.Controller.SetTool(CSGTool.Edit));
            _overlayCSGEditButton.LinkTooltip("Edit brush topology.", ref inputOptions.CSGEditTool);
            _overlayCSGSurfaceButton = AddViewportToolStripButton("Surface", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.CSG.Surface.Left", () => CSGAuthoringMode.Controller.SetTool(CSGTool.Surface));
            _overlayCSGSurfaceButton.LinkTooltip("Edit brush surface properties.", ref inputOptions.CSGSurfaceTool);
            _overlayCSGClipButton = AddViewportToolStripButton("Clip", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Scene.CSG.Clip.Left", null);
            _overlayCSGClipButton.Enabled = false;
            _overlayCSGClipButton.LinkTooltip("Clip is planned for a later milestone.");
            _overlayCSGOperationButton = AddViewportToolStripMenuButton(GetCSGOperationLabel(), SpriteHandle.Invalid, CreateCSGOperationMenu(), ToolStripAnchor.Left, "Flax.Scene.CSG.Operation.Left");
            _overlayCSGOperationButton.DrawMenuChevron = true;
            _overlayCSGOperationButton.LinkTooltip("Operation used by newly authored brushes.");
            _overlayCSGPlaneButton = AddViewportToolStripMenuButton("Plane", SpriteHandle.Invalid, CreateCSGPlaneMenu(), ToolStripAnchor.Left, "Flax.Scene.CSG.Plane.Left");
            _overlayCSGPlaneButton.DrawMenuChevron = true;
            _overlayCSGPlaneButton.LinkTooltip("Pick, lock, or reset the CSG working plane.");
            _overlayCSGSnapButton = AddViewportToolStripButton("Snap", _editor.Icons.Grid32, ToolStripAnchor.Left, "Flax.Scene.CSG.Snap.Left", () => CSGAuthoringMode.Controller.SetSnappingEnabled(!CSGAuthoringMode.Controller.SnappingEnabled));
            _overlayCSGSnapButton.LinkTooltip("Toggle CSG grid snapping.");
            _overlayCSGSnapValueButton = AddViewportToolStripMenuButton(GetCSGSnapLabel(), SpriteHandle.Invalid, CreateCSGSnapMenu(), ToolStripAnchor.Left, "Flax.Scene.CSG.SnapValue.Left");
            _overlayCSGSnapValueButton.LinkTooltip("CSG linear snap increment.");
            _overlayCSGVisibilityButton = AddViewportToolStripMenuButton("View", SpriteHandle.Invalid, CreateCSGVisibilityMenu(), ToolStripAnchor.Left, "Flax.Scene.CSG.Visibility.Left");
            _overlayCSGVisibilityButton.DrawMenuChevron = true;
            _overlayCSGVisibilityButton.LinkTooltip("CSG viewport visibility.");

            _overlayGridButton = AddViewportToolStripButton("Grid", Editor.Instance.Icons.Grid32, ToolStripAnchor.Right, "Flax.Scene.Grid", () => Grid.Enabled = !Grid.Enabled);
            _overlayGridButton.LinkTooltip("Toggle grid.");
            _overlayNavigationButton = AddViewportToolStripButton("Nav", SpriteHandle.Invalid, ToolStripAnchor.Right, "Flax.Scene.Navigation", () => ShowNavigation = !ShowNavigation);
            _overlayNavigationButton.LinkTooltip("Toggle navigation mesh.");
            _overlayGameViewButton = AddViewportToolStripGlyphButton(ToolStripGlyph.Eye, ToolStripAnchor.Right, "Flax.Scene.GameView", ToggleGameView);
            _overlayGameViewButton.LinkTooltip("Toggle game view preview.", ref inputOptions.ToggleGameView);
            _overlayCharacterControllerModeButton = AddViewportToolStripButton("Walk", SpriteHandle.Invalid, ToolStripAnchor.Right, "Flax.Scene.CharacterController", ToggleCharacterControllerMode);
            _overlayCharacterControllerModeButton.LinkTooltip("Toggle in-scene character controller camera mode.", ref inputOptions.ToggleCharacterControllerMode);
            AddViewportToolStripButton("Cam+", Editor.Instance.Icons.CameraFill32, ToolStripAnchor.Right, "Flax.Scene.CreateCamera", CreateCameraAtView).LinkTooltip("Create camera here.");
            UpdateViewportToolStrip();
        }

        private ContextMenu CreateGizmoModeMenu()
        {
            var menu = new ContextMenu();
            var objectMode = menu.AddButton("Object Mode", () => Gizmos.SetActiveMode<TransformGizmoMode>());
            objectMode.Icon = _editor.Icons.Toolbox96;
            var csgMode = menu.AddButton("CSG Authoring", () => Gizmos.SetActiveMode<CSGAuthoringGizmoMode>());
            csgMode.Icon = _editor.Icons.VisjectBoxClosed32;
            var noGizmoMode = menu.AddButton("No Gizmo", () => Gizmos.SetActiveMode<NoGizmoMode>());
            noGizmoMode.Icon = _editor.Icons.Cross12;
            menu.AddSeparator();
            var sculptTerrainMode = menu.AddButton("Sculpt Terrain", () => Gizmos.SetActiveMode<Tools.Terrain.SculptTerrainGizmoMode>());
            sculptTerrainMode.Icon = _editor.Icons.Terrain96;
            var paintTerrainMode = menu.AddButton("Paint Terrain", () => Gizmos.SetActiveMode<Tools.Terrain.PaintTerrainGizmoMode>());
            paintTerrainMode.Icon = _editor.Icons.Paint96;
            var editTerrainMode = menu.AddButton("Edit Terrain", () => Gizmos.SetActiveMode<Tools.Terrain.EditTerrainGizmoMode>());
            editTerrainMode.Icon = _editor.Icons.Terrain96;
            menu.AddSeparator();
            var paintFoliageMode = menu.AddButton("Paint Foliage", () => Gizmos.SetActiveMode<Tools.Foliage.PaintFoliageGizmoMode>());
            paintFoliageMode.Icon = _editor.Icons.Foliage96;
            var editFoliageMode = menu.AddButton("Edit Foliage", () => Gizmos.SetActiveMode<Tools.Foliage.EditFoliageGizmoMode>());
            editFoliageMode.Icon = _editor.Icons.Foliage96;
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                objectMode.Checked = Gizmos.ActiveMode is TransformGizmoMode;
                csgMode.Checked = Gizmos.ActiveMode is CSGAuthoringGizmoMode;
                noGizmoMode.Checked = Gizmos.ActiveMode is NoGizmoMode;
                sculptTerrainMode.Checked = Gizmos.ActiveMode is Tools.Terrain.SculptTerrainGizmoMode;
                paintTerrainMode.Checked = Gizmos.ActiveMode is Tools.Terrain.PaintTerrainGizmoMode;
                editTerrainMode.Checked = Gizmos.ActiveMode is Tools.Terrain.EditTerrainGizmoMode;
                paintFoliageMode.Checked = Gizmos.ActiveMode is Tools.Foliage.PaintFoliageGizmoMode;
                editFoliageMode.Checked = Gizmos.ActiveMode is Tools.Foliage.EditFoliageGizmoMode;
            };
            return menu;
        }

        private ContextMenu CreateCSGOperationMenu()
        {
            var menu = new ContextMenu();
            var additive = menu.AddButton("Additive", () => CSGAuthoringMode.Controller.SetOperation(CSGOperation.Additive));
            var subtractive = menu.AddButton("Subtractive", () => CSGAuthoringMode.Controller.SetOperation(CSGOperation.Subtractive));
            var intersecting = menu.AddButton("Intersecting");
            intersecting.Enabled = false;
            intersecting.LinkTooltip("Intersecting operations are planned for a later milestone.");
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                additive.Checked = CSGAuthoringMode.Controller.Operation == CSGOperation.Additive;
                subtractive.Checked = CSGAuthoringMode.Controller.Operation == CSGOperation.Subtractive;
            };
            return menu;
        }

        private ContextMenu CreateCSGPlaneMenu()
        {
            var menu = new ContextMenu();
            var pick = menu.AddButton("Pick from Surface", () => CSGAuthoringMode.Controller.RequestPickWorkingPlane());
            pick.LinkTooltip("Working-plane picking is routed to the CSG tool and implemented in a later milestone.");
            var locked = menu.AddButton("Lock Plane", () => CSGAuthoringMode.Controller.SetWorkingPlaneLocked(!CSGAuthoringMode.Controller.WorkingPlaneLocked));
            locked.CloseMenuOnClick = false;
            menu.AddButton("Reset Plane", CSGAuthoringMode.Controller.ResetWorkingPlane);
            menu.VisibleChanged += control =>
            {
                if (control.Visible)
                    locked.Checked = CSGAuthoringMode.Controller.WorkingPlaneLocked;
            };
            return menu;
        }

        private ContextMenu CreateCSGSnapMenu()
        {
            var menu = new ContextMenu();
            var buttons = new List<ContextMenuButton>(CSGToolController.SnapIncrements.Length);
            foreach (float increment in CSGToolController.SnapIncrements)
            {
                var value = increment;
                var button = menu.AddButton(value.ToString(), () => CSGAuthoringMode.Controller.SetSnapIncrement(value));
                button.Tag = value;
                buttons.Add(button);
            }
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                foreach (var button in buttons)
                    button.Checked = Mathf.NearEqual((float)button.Tag, CSGAuthoringMode.Controller.SnapIncrement);
            };
            return menu;
        }

        private ContextMenu CreateCSGVisibilityMenu()
        {
            var menu = new ContextMenu();
            var source = menu.AddButton("Source Brushes", () => CSGAuthoringMode.Controller.ToggleVisibility(CSGVisibility.SourceBrushes));
            var built = menu.AddButton("Built Geometry", () => CSGAuthoringMode.Controller.ToggleVisibility(CSGVisibility.BuiltGeometry));
            var hidden = menu.AddButton("Hidden Brushes", () => CSGAuthoringMode.Controller.ToggleVisibility(CSGVisibility.HiddenBrushes));
            source.CloseMenuOnClick = false;
            built.CloseMenuOnClick = false;
            hidden.CloseMenuOnClick = false;
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                var visibility = CSGAuthoringMode.Controller.Visibility;
                source.Checked = (visibility & CSGVisibility.SourceBrushes) != 0;
                built.Checked = (visibility & CSGVisibility.BuiltGeometry) != 0;
                hidden.Checked = (visibility & CSGVisibility.HiddenBrushes) != 0;
            };
            return menu;
        }

        private ContextMenu CreateTranslateSnapMenu()
        {
            var menu = CreateSnapValueMenu(TranslateSnapValues, () => TransformGizmo.TranslationSnapValue, value =>
            {
                TransformGizmo.TranslationSnapValue = value;
                _editor.ProjectCache.SetCustomData("TranslateSnapValue", TransformGizmo.TranslationSnapValue);
            });
            var buttonBB = menu.AddButton("Bounding Box");
            buttonBB.LinkTooltip("Snaps the selection based on its bounding volume.");
            buttonBB.Tag = -1.0f;
            var buttonCustom = menu.AddButton("Custom");
            buttonCustom.LinkTooltip("Custom grid size.");
            const float defaultCustomTranslateSnappingValue = 250.0f;
            float customTranslateSnappingValue = TransformGizmo.TranslationSnapValue;
            if (customTranslateSnappingValue < 0.0f)
                customTranslateSnappingValue = defaultCustomTranslateSnappingValue;
            foreach (var v in TranslateSnapValues)
            {
                if (Mathf.Abs(TransformGizmo.TranslationSnapValue - v) < 0.001f)
                {
                    customTranslateSnappingValue = defaultCustomTranslateSnappingValue;
                    break;
                }
            }
            buttonCustom.Tag = customTranslateSnappingValue;
            var customValue = new FloatValueBox(customTranslateSnappingValue, Style.Current.FontMedium.MeasureText(buttonCustom.Text).X + 5, 2, 70.0f, 0.001f, float.MaxValue, 0.1f)
            {
                Parent = buttonCustom
            };
            customValue.ValueChanged += () =>
            {
                buttonCustom.Tag = customValue.Value;
                buttonCustom.Click();
            };
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                customValue.Value = (float)buttonCustom.Tag;
            };
            return menu;
        }

        private ContextMenu CreateSnapValueMenu(float[] values, Func<float> getter, Action<float> setter)
        {
            var menu = new ContextMenu();
            for (int i = 0; i < values.Length; i++)
            {
                var v = values[i];
                var button = menu.AddButton(v.ToString());
                button.Tag = v;
            }
            menu.ButtonClicked += button =>
            {
                setter((float)button.Tag);
                UpdateViewportToolStrip();
            };
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                foreach (var e in menu.Items)
                {
                    if (e is ContextMenuButton b && b.Tag is float value)
                        b.Icon = Mathf.Abs(getter() - value) < 0.001f ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
                }
            };
            return menu;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            base.Update(deltaTime);
            UpdateViewportToolStrip();

            if (_characterControllerModeActive)
            {
                if (_characterControllerCamera == null || !_characterControllerCamera.IsActive)
                {
                    StopCharacterControllerMode();
                    return;
                }

                return;
            }

            var selection = TransformGizmo.SelectedParents;
            var requestUnlockFocus = FlaxEngine.Input.Mouse.GetButtonDown(MouseButton.Right) || FlaxEngine.Input.Mouse.GetButtonDown(MouseButton.Left);
            if (TransformGizmo.SelectedParents.Count == 0 || (requestUnlockFocus && ContainsFocus))
            {
                UnlockFocusSelection();
            }
            else if (_lockedFocus)
            {
                var selectionBounds = BoundingSphere.Empty;
                for (int i = 0; i < selection.Count; i++)
                {
                    selection[i].GetEditorSphere(out var sphere);
                    BoundingSphere.Merge(ref selectionBounds, ref sphere, out selectionBounds);
                }

                if (ContainsFocus)
                {
                    var viewportFocusDistance = Vector3.Distance(ViewPosition, selectionBounds.Center) / 10f;
                    _lockedFocusOffset -= FlaxEngine.Input.Mouse.ScrollDelta * viewportFocusDistance;
                }

                var focusDistance = Mathf.Max(selectionBounds.Radius * 2d, 100d);
                ViewPosition = selectionBounds.Center + (-ViewDirection * (focusDistance + _lockedFocusOffset));
            }
        }

        /// <summary>
        /// Overrides the selection outline effect or restored the default one.
        /// </summary>
        /// <param name="customSelectionOutline">The custom selection outline or null if use default one.</param>
        public void OverrideSelectionOutline(SelectionOutline customSelectionOutline)
        {
            if (_customSelectionOutline != null)
            {
                Task.RemoveCustomPostFx(_customSelectionOutline);
                Object.Destroy(ref _customSelectionOutline);
                Task.AddCustomPostFx(customSelectionOutline ? customSelectionOutline : SelectionOutline);
            }
            else if (customSelectionOutline != null)
            {
                Task.RemoveCustomPostFx(SelectionOutline);
                Task.AddCustomPostFx(customSelectionOutline);
            }

            _customSelectionOutline = customSelectionOutline;
        }

        private void OnPlayModeBeginning()
        {
            Gizmos.ActiveMode?.TryCancel(EditorGizmoModeCancelReason.PlayModeBeginning);
            StopCharacterControllerMode();
        }

        private void ToggleCharacterControllerMode()
        {
            if (_characterControllerModeActive)
                StopCharacterControllerMode();
            else
                StartCharacterControllerMode();
        }

        private void StartCharacterControllerMode()
        {
            if (_characterControllerModeActive || !Level.IsAnySceneLoaded)
                return;

            if (_gameViewActive)
                ToggleGameView();

            _characterControllerCamera ??= new InSceneCharacterControllerCamera();
            _preCharacterControllerCamera = ViewportCamera;
            _characterControllerWasOrthographic = UseOrthographicProjection;
            _characterControllerWasTransformGizmoVisible = TransformGizmo.Visible;
            _characterControllerWasSelectionOutlineShown = SelectionOutline.ShowSelectionOutline;

            UseOrthographicProjection = false;
            ViewportCamera = _characterControllerCamera;
            if (!_characterControllerCamera.Enter())
            {
                ViewportCamera = _preCharacterControllerCamera;
                _preCharacterControllerCamera = null;
                UseOrthographicProjection = _characterControllerWasOrthographic;
                return;
            }

            _characterControllerModeActive = true;
            Focus();
            UpdateCharacterControllerModeButtons();
            UpdateViewportToolStrip();
        }

        private void StopCharacterControllerMode()
        {
            if (!_characterControllerModeActive)
                return;

            if (_characterControllerControlMouseActive && RootWindow?.Window != null)
            {
                base.OnControlMouseEnd(RootWindow.Window);
                _characterControllerControlMouseActive = false;
            }
            _characterControllerCamera?.Exit();

            if (ViewportCamera == _characterControllerCamera)
                ViewportCamera = _preCharacterControllerCamera ?? new FPSCamera();
            _preCharacterControllerCamera = null;

            UseOrthographicProjection = _characterControllerWasOrthographic;
            TransformGizmo.Visible = _characterControllerWasTransformGizmoVisible && !_gameViewActive;
            SelectionOutline.ShowSelectionOutline = _characterControllerWasSelectionOutlineShown && !_gameViewActive;
            _characterControllerModeActive = false;
            UpdateCharacterControllerModeButtons();
            UpdateViewportToolStrip();
        }

        /// <inheritdoc />
        public override bool OnMouseWheel(Float2 location, float delta)
        {
            if (_characterControllerModeActive)
            {
                base.OnMouseWheel(location, delta);
                return true;
            }

            return base.OnMouseWheel(location, delta);
        }

        private void UpdateCharacterControllerModeButtons()
        {
            if (_toggleCharacterControllerModeButton != null)
                _toggleCharacterControllerModeButton.Icon = _characterControllerModeActive ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
            if (_overlayCharacterControllerModeButton != null)
                _overlayCharacterControllerModeButton.Checked = _characterControllerModeActive;
        }

        private bool IsCharacterControllerLookActive()
        {
            if (!_characterControllerModeActive)
                return false;
            return (Root?.GetMouseButton(MouseButton.Right) ?? false) || _input.IsMouseRightDown;
        }

        private void CreateCameraAtView()
        {
            if (!Level.IsAnySceneLoaded)
                return;

            // Create actor
            var parent = Level.GetScene(0);
            var actor = new Camera
            {
                StaticFlags = StaticFlags.None,
                Name = Utilities.Utils.IncrementNameNumber("Camera", x => parent.GetChild(x) == null),
                Transform = ViewTransform,
                NearPlane = NearPlane,
                FarPlane = FarPlane,
                OrthographicScale = OrthographicScale,
                UsePerspective = !UseOrthographicProjection,
                FieldOfView = FieldOfView
            };

            // Spawn
            _editor.SceneEditing.Spawn(actor, parent);
        }

        private void OnBegin(RenderTask task, GPUContext context)
        {
            _debugDrawData.Clear();

            if (task is SceneRenderTask sceneRenderTask)
            {
                // Sync debug view to avoid lag on culling/LODing
                var view = sceneRenderTask.View;
                DebugDraw.SetView(ref view);
            }

            // Collect selected objects debug shapes and visuals
            var selectedParents = TransformGizmo.SelectedParents;
            if (selectedParents.Count > 0)
            {
                for (int i = 0; i < selectedParents.Count; i++)
                {
                    if (selectedParents[i].IsActiveInHierarchy)
                        selectedParents[i].OnDebugDraw(_debugDrawData);
                }
            }
        }

        private void OnCollectDrawCalls(ref RenderContext renderContext)
        {
            if (renderContext.View.Pass == DrawPass.Depth)
                return;
            DragHandlers.CollectDrawCalls(_debugDrawData, ref renderContext);
            if (ShowNavigation)
                Editor.Internal_DrawNavMesh();
            _debugDrawData.OnDraw(ref renderContext);
        }

        /// <inheritdoc />
        public void DrawEditorPrimitives(GPUContext context, ref RenderContext renderContext, GPUTexture target, GPUTexture targetDepth)
        {
            // Draw gizmos
            foreach (var gizmo in Gizmos)
            {
                if (gizmo.Visible)
                {
                    gizmo.Draw(ref renderContext);
                }
            }

            // Draw selected objects debug shapes and visuals
            if (DrawDebugDraw && (renderContext.View.Flags & ViewFlags.DebugDraw) == ViewFlags.DebugDraw)
            {
                _debugDrawData.DrawActors(true);
                DebugDraw.Draw(ref renderContext, target.View(), targetDepth.View(), true);
            }
        }

        private void OnPostRender(GPUContext context, ref RenderContext renderContext)
        {
            bool renderPostFx = true;
            switch (renderContext.View.Mode)
            {
            case ViewMode.Default:
            case ViewMode.PhysicsColliders:
                renderPostFx = false;
                break;
            }
            if (renderPostFx)
            {
                var task = renderContext.Task;

                // Render editor primitives, gizmo and debug shapes in debug view modes
                // Note: can use Output buffer as both input and output because EditorPrimitives is using an intermediate buffer
                if (EditorPrimitives && EditorPrimitives.CanRender())
                {
                    EditorPrimitives.Render(context, ref renderContext, task.Output, task.Output);
                }

                // Render editor sprites
                if (_editorSpritesRenderer && _editorSpritesRenderer.CanRender())
                {
                    _editorSpritesRenderer.Render(context, ref renderContext, task.Output, task.Output);
                }

                // Render selection outline
                var selectionOutline = _customSelectionOutline ?? SelectionOutline;
                RenderSelectionOutline(context, ref renderContext, task, selectionOutline);
                RenderSelectionOutline(context, ref renderContext, task, ViewportHoverOutline);
                RenderSelectionOutline(context, ref renderContext, task, SceneTreeHoverOutline);
            }
        }

        private static void RenderSelectionOutline(GPUContext context, ref RenderContext renderContext, SceneRenderTask task, SelectionOutline selectionOutline)
        {
            if (selectionOutline && selectionOutline.CanRender())
            {
                // Use temporary intermediate buffer
                var desc = task.Output.Description;
                var temp = RenderTargetPool.Get(ref desc);
                selectionOutline.Render(context, ref renderContext, task.Output, temp);

                // Copy the results back to the output
                context.CopyTexture(task.Output, 0, 0, 0, 0, temp, 0);

                RenderTargetPool.Release(temp);
            }
        }

        private void OnSelectionChanged()
        {
            var selection = _editor.SceneEditing.Selection;
            Gizmos.ForEach(x => x.OnSelectionChanged(selection));
        }

        /// <summary>
        /// Press "R" to rotate the selected gizmo objects 45 degrees.
        /// </summary>
        public void RotateSelection()
        {
            var win = (WindowRootControl)Root;
            var selection = _editor.SceneEditing.Selection;
            var isShiftDown = win.GetKey(KeyboardKeys.Shift);

            Quaternion rotationDelta;
            if (isShiftDown)
                rotationDelta = Quaternion.Euler(0.0f, -45.0f, 0.0f);
            else
                rotationDelta = Quaternion.Euler(0.0f, 45.0f, 0.0f);

            bool useObjCenter = TransformGizmo.ActivePivot == TransformGizmoBase.PivotType.ObjectCenter;
            Vector3 gizmoPosition = TransformGizmo.InteractionPivotPosition;

            // Rotate selected objects
            bool isPlayMode = _editor.StateMachine.IsPlayMode;
            TransformGizmo.StartTransforming();
            for (int i = 0; i < selection.Count; i++)
            {
                var obj = selection[i];
                if (isPlayMode && obj.CanTransform == false)
                    continue;
                var trans = obj.Transform;
                var pivotOffset = trans.Translation - gizmoPosition;
                if (useObjCenter || pivotOffset.IsZero)
                {
                    trans.Orientation *= Quaternion.Invert(trans.Orientation) * rotationDelta * trans.Orientation;
                }
                else
                {
                    Matrix.RotationQuaternion(ref trans.Orientation, out var transWorld);
                    Matrix.RotationQuaternion(ref rotationDelta, out var deltaWorld);
                    Matrix world = transWorld * Matrix.Translation(pivotOffset) * deltaWorld * Matrix.Translation(-pivotOffset);
                    trans.SetRotation(ref world);
                    trans.Translation += world.TranslationVector;
                }
                obj.Transform = trans;
            }
            TransformGizmo.EndTransforming();
        }

        /// <summary>
        /// Toggles game view view mode on or off.
        /// </summary>
        public void ToggleGameView()
        {
            if (_characterControllerModeActive)
                StopCharacterControllerMode();

            if (!_gameViewActive)
            {
                // Cache flags & values
                _preGameViewFlags = Task.ViewFlags;
                _preGameViewViewMode = Task.ViewMode;
                _gameViewWasGridShown = Grid.Enabled;
                _gameViewWasFpsCounterShown = ShowFpsCounter;
                _gameViewWasNavigationShown = ShowNavigation;
            }

            // Set flags & values
            Task.ViewFlags = _gameViewActive ? _preGameViewFlags : ViewFlags.DefaultGame;
            Task.ViewMode = _gameViewActive ? _preGameViewViewMode : ViewMode.Default;
            ShowFpsCounter = _gameViewActive ? _gameViewWasFpsCounterShown : false;
            ShowNavigation = _gameViewActive ? _gameViewWasNavigationShown : false;
            Grid.Enabled = _gameViewActive ? _gameViewWasGridShown : false;

            _gameViewActive = !_gameViewActive;

            TransformGizmo.Visible = !_gameViewActive;
            CSGAuthoringMode.Gizmo.Visible = !_gameViewActive;
            SelectionOutline.ShowSelectionOutline = !_gameViewActive;
            if (_gameViewActive)
                ClearSceneTreeHoverFromEditorViewport();

            _toggleGameViewButton.Icon = _gameViewActive ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
            UpdateViewportToolStrip();
        }

        /// <inheritdoc />
        protected override void UpdateViewportToolStrip()
        {
            base.UpdateViewportToolStrip();
            SetViewportToolStripButtonText(_overlayModeButton, GetGizmoModeLabel());
            bool objectMode = Gizmos.ActiveMode is TransformGizmoMode;
            bool csgMode = Gizmos.ActiveMode is CSGAuthoringGizmoMode;
            if (_overlaySelectModeButton != null)
            {
                _overlaySelectModeButton.Visible = objectMode;
                _overlaySelectModeButton.Checked = TransformGizmo.ActiveMode == TransformGizmoBase.Mode.Select;
            }
            if (_overlayTranslateModeButton != null)
            {
                _overlayTranslateModeButton.Visible = objectMode;
                _overlayTranslateModeButton.Checked = TransformGizmo.ActiveMode == TransformGizmoBase.Mode.Translate;
            }
            if (_overlayRotateModeButton != null)
            {
                _overlayRotateModeButton.Visible = objectMode;
                _overlayRotateModeButton.Checked = TransformGizmo.ActiveMode == TransformGizmoBase.Mode.Rotate;
            }
            if (_overlayScaleModeButton != null)
            {
                _overlayScaleModeButton.Visible = objectMode;
                _overlayScaleModeButton.Checked = TransformGizmo.ActiveMode == TransformGizmoBase.Mode.Scale;
            }
            if (_overlayBoundsModeButton != null)
            {
                _overlayBoundsModeButton.Visible = objectMode;
                _overlayBoundsModeButton.Checked = TransformGizmo.ActiveMode == TransformGizmoBase.Mode.Bounds;
            }
            if (_overlayTransformSpaceButton != null)
            {
                _overlayTransformSpaceButton.Visible = objectMode;
                var isWorld = TransformGizmo.ActiveTransformSpace == TransformGizmoBase.TransformSpace.World;
                _overlayTransformSpaceButton.Checked = isWorld;
                SetViewportToolStripButtonText(_overlayTransformSpaceButton, isWorld ? "World" : "Local");
            }
            if (_overlayPivotButton != null)
            {
                _overlayPivotButton.Visible = objectMode;
                var isObjectPivot = TransformGizmo.ActivePivot == TransformGizmoBase.PivotType.ObjectCenter;
                _overlayPivotButton.Checked = isObjectPivot;
                SetViewportToolStripButtonText(_overlayPivotButton, isObjectPivot ? "Pivot" : "Center");
            }
            if (_overlayAbsoluteSnapButton != null)
            {
                _overlayAbsoluteSnapButton.Visible = objectMode && TransformGizmo.ActiveTransformSpace == TransformGizmoBase.TransformSpace.World;
                _overlayAbsoluteSnapButton.Checked = TransformGizmo.AbsoluteSnapEnabled;
            }
            if (_overlayTranslateSnapButton != null)
            {
                _overlayTranslateSnapButton.Visible = objectMode;
                _overlayTranslateSnapButton.Checked = TransformGizmo.TranslationSnapEnable;
            }
            if (_overlayRotateSnapButton != null)
            {
                _overlayRotateSnapButton.Visible = objectMode;
                _overlayRotateSnapButton.Checked = TransformGizmo.RotationSnapEnabled;
            }
            if (_overlayScaleSnapButton != null)
            {
                _overlayScaleSnapButton.Visible = objectMode;
                _overlayScaleSnapButton.Checked = TransformGizmo.ScaleSnapEnabled;
            }
            if (_overlayTranslateSnapValueButton != null)
                _overlayTranslateSnapValueButton.Visible = objectMode;
            if (_overlayRotateSnapValueButton != null)
                _overlayRotateSnapValueButton.Visible = objectMode;
            if (_overlayScaleSnapValueButton != null)
                _overlayScaleSnapValueButton.Visible = objectMode;
            SetViewportToolStripButtonText(_overlayTranslateSnapValueButton, GetTranslateSnapLabel());
            SetViewportToolStripButtonText(_overlayRotateSnapValueButton, GetRotateSnapLabel());
            SetViewportToolStripButtonText(_overlayScaleSnapValueButton, GetScaleSnapLabel());

            if (_overlayCSGSelectPlaceButton != null)
            {
                _overlayCSGSelectPlaceButton.Visible = csgMode;
                _overlayCSGSelectPlaceButton.Checked = CSGAuthoringMode.Controller.Tool == CSGTool.SelectPlace;
            }
            if (_overlayCSGDrawButton != null)
            {
                _overlayCSGDrawButton.Visible = csgMode;
                _overlayCSGDrawButton.Checked = CSGAuthoringMode.Controller.Tool == CSGTool.Draw;
            }
            if (_overlayCSGEditButton != null)
            {
                _overlayCSGEditButton.Visible = csgMode;
                _overlayCSGEditButton.Checked = CSGAuthoringMode.Controller.Tool == CSGTool.Edit;
            }
            if (_overlayCSGSurfaceButton != null)
            {
                _overlayCSGSurfaceButton.Visible = csgMode;
                _overlayCSGSurfaceButton.Checked = CSGAuthoringMode.Controller.Tool == CSGTool.Surface;
            }
            if (_overlayCSGClipButton != null)
                _overlayCSGClipButton.Visible = csgMode;
            if (_overlayCSGOperationButton != null)
            {
                _overlayCSGOperationButton.Visible = csgMode;
                SetViewportToolStripButtonText(_overlayCSGOperationButton, GetCSGOperationLabel());
            }
            if (_overlayCSGPlaneButton != null)
            {
                _overlayCSGPlaneButton.Visible = csgMode;
                _overlayCSGPlaneButton.Checked = CSGAuthoringMode.Controller.WorkingPlaneLocked;
                SetViewportToolStripButtonText(_overlayCSGPlaneButton, CSGAuthoringMode.Controller.WorkingPlaneLocked ? "Plane Lock" : "Plane");
            }
            if (_overlayCSGSnapButton != null)
            {
                _overlayCSGSnapButton.Visible = csgMode;
                _overlayCSGSnapButton.Checked = CSGAuthoringMode.Controller.SnappingEnabled;
            }
            if (_overlayCSGSnapValueButton != null)
            {
                _overlayCSGSnapValueButton.Visible = csgMode;
                SetViewportToolStripButtonText(_overlayCSGSnapValueButton, GetCSGSnapLabel());
            }
            if (_overlayCSGVisibilityButton != null)
                _overlayCSGVisibilityButton.Visible = csgMode;
            if (_overlayGridButton != null)
                _overlayGridButton.Checked = Grid.Enabled;
            if (_overlayNavigationButton != null)
                _overlayNavigationButton.Checked = ShowNavigation;
            if (_overlayGameViewButton != null)
                _overlayGameViewButton.Checked = _gameViewActive;
            UpdateCharacterControllerModeButtons();
        }

        /// <summary>
        /// Sets the actor highlighted in the editor viewport when hovering the scene tree.
        /// </summary>
        /// <param name="actorNode">The hovered actor node or null to clear the highlight.</param>
        public void SetSceneTreeHoveredActor(ActorNode actorNode)
        {
            _sceneTreeHoveredActor = actorNode;
        }

        private List<SceneGraphNode> GetSceneTreeHoverSelection()
        {
            _sceneTreeHoverSelection.Clear();
            if (!SelectionOutline || !SelectionOutline.ShowSelectionOutline)
                return _sceneTreeHoverSelection;

            var actorNode = _sceneTreeHoveredActor;
            if (_editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport && actorNode != null && actorNode.Actor)
                _sceneTreeHoverSelection.Add(actorNode);
            actorNode = _editorViewportHoveredActor;
            if (_editorViewportHoverIsLeafTarget && _editor.Options.Options.Interface.HighlightViewportObjectHover && actorNode != null && actorNode.Actor && !_sceneTreeHoverSelection.Contains(actorNode))
                _sceneTreeHoverSelection.Add(actorNode);

            return _sceneTreeHoverSelection;
        }

        private List<SceneGraphNode> GetViewportPrimaryHoverSelection()
        {
            _viewportPrimaryHoverSelection.Clear();
            if (!SelectionOutline || !SelectionOutline.ShowSelectionOutline || _editorViewportHoverIsLeafTarget)
                return _viewportPrimaryHoverSelection;

            var actorNode = _editorViewportHoveredActor;
            if (_editor.Options.Options.Interface.HighlightViewportObjectHover && actorNode != null && actorNode.Actor)
                _viewportPrimaryHoverSelection.Add(actorNode);
            return _viewportPrimaryHoverSelection;
        }

        private ActorNode GetActorNodeUnderMouse(out bool isLeafTarget)
        {
            isLeafTarget = false;
            if (_gameViewActive || _characterControllerModeActive || IsControllingMouse || IsLeftMouseButtonDown || IsRightMouseButtonDown || IsAltKeyDown || _directionGizmo.IsMouseOver || !ContainsPoint(ref _viewMousePos))
                return null;

            var ray = ConvertMouseToRay(ref _viewMousePos);
            var view = new Ray(ViewPosition, ViewDirection);
            var renderView = Task.View;
            var hit = TransformGizmo.GetHoverTarget(ref ray, ref view, renderView.Flags, renderView.Mode, out isLeafTarget);
            if (hit is ActorChildNode actorChildNode)
                return actorChildNode.ParentNode as ActorNode;
            return hit as ActorNode;
        }

        private void UpdateSceneTreeHoverFromEditorViewport()
        {
            if (!_editor.Options.Options.Interface.HighlightViewportObjectHover)
            {
                ClearSceneTreeHoverFromEditorViewport();
                return;
            }

            var actorNode = GetActorNodeUnderMouse(out var isLeafTarget);
            if (_editorViewportHoveredActor == actorNode && _editorViewportHoverIsLeafTarget == isLeafTarget)
                return;

            _editorViewportHoveredActor = actorNode;
            _editorViewportHoverIsLeafTarget = isLeafTarget;
            _editor.Windows.SceneWin?.SetViewportHoveredActor(actorNode);
        }

        private void ClearSceneTreeHoverFromEditorViewport()
        {
            if (_editorViewportHoveredActor == null && !_editorViewportHoverIsLeafTarget)
                return;

            _editorViewportHoveredActor = null;
            _editorViewportHoverIsLeafTarget = false;
            _editor.Windows.SceneWin?.SetViewportHoveredActor(null);
        }

        private static void SetViewportToolStripButtonText(ToolStripButton button, string text)
        {
            if (button != null && button.Text != text)
                button.Text = text;
        }

        private string GetTranslateSnapLabel()
        {
            return TransformGizmo.TranslationSnapValue < 0.0f ? "Bounds" : TransformGizmo.TranslationSnapValue.ToString();
        }

        private string GetRotateSnapLabel()
        {
            return TransformGizmo.RotationSnapValue.ToString();
        }

        private string GetScaleSnapLabel()
        {
            return TransformGizmo.ScaleSnapValue.ToString();
        }

        private string GetCSGOperationLabel()
        {
            return CSGAuthoringMode?.Controller?.Operation == CSGOperation.Subtractive ? "Subtract" : "Add";
        }

        private string GetCSGSnapLabel()
        {
            return CSGAuthoringMode?.Controller?.SnapIncrement.ToString() ?? "10";
        }

        private string GetGizmoModeLabel()
        {
            if (Gizmos.ActiveMode is TransformGizmoMode)
                return "Object Mode";
            if (Gizmos.ActiveMode is CSGAuthoringGizmoMode)
                return "CSG Mode";
            if (Gizmos.ActiveMode is NoGizmoMode)
                return "No Gizmo";
            if (Gizmos.ActiveMode is Tools.Terrain.SculptTerrainGizmoMode)
                return "Sculpt Terrain";
            if (Gizmos.ActiveMode is Tools.Terrain.PaintTerrainGizmoMode)
                return "Paint Terrain";
            if (Gizmos.ActiveMode is Tools.Terrain.EditTerrainGizmoMode)
                return "Edit Terrain";
            if (Gizmos.ActiveMode is Tools.Foliage.PaintFoliageGizmoMode)
                return "Paint Foliage";
            if (Gizmos.ActiveMode is Tools.Foliage.EditFoliageGizmoMode)
                return "Edit Foliage";
            return "Mode";
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            base.OnLostFocus();

            Gizmos.ActiveMode?.TryCancel(EditorGizmoModeCancelReason.FocusLost);
            ClearSceneTreeHoverFromEditorViewport();
            _rubberBandSelector.StopRubberBand();
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            base.OnMouseLeave();

            ClearSceneTreeHoverFromEditorViewport();
            _rubberBandSelector.StopRubberBand();
        }

        /// <summary>
        /// Focuses the viewport on the current selection of the gizmo.
        /// </summary>
        public void FocusSelection()
        {
            var orientation = ViewOrientation;
            FocusSelection(ref orientation);
        }

        /// <summary>
        /// Lock focus on the current selection gizmo.
        /// </summary>
        public void LockFocusSelection()
        {
            _lockedFocus = true;
        }

        /// <summary>
        /// Unlock focus on the current selection.
        /// </summary>
        public void UnlockFocusSelection()
        {
            _lockedFocus = false;
            _lockedFocusOffset = 0f;
        }

        /// <summary>
        /// Focuses the viewport on the current selection of the gizmo.
        /// </summary>
        /// <param name="orientation">The target view orientation.</param>
        public void FocusSelection(ref Quaternion orientation)
        {
            ViewportCamera.FocusSelection(Gizmos, ref orientation);
        }

        /// <summary>
        /// Applies the transform to the collection of scene graph nodes.
        /// </summary>
        /// <param name="selection">The selection.</param>
        /// <param name="translationDelta">The translation delta.</param>
        /// <param name="rotationDelta">The rotation delta.</param>
        /// <param name="scaleDelta">The scale delta.</param>
        public void ApplyTransform(List<SceneGraphNode> selection, ref Vector3 translationDelta, ref Quaternion rotationDelta, ref Vector3 scaleDelta)
        {
            // TransformGizmoBase restores the transaction start transforms
            // before invoking this delegate. These deltas are therefore
            // origin-relative preview values, never the previous frame's
            // scene state.
            bool applyRotation = !rotationDelta.IsIdentity;
            bool useObjCenter = TransformGizmo.ActivePivot == TransformGizmoBase.PivotType.ObjectCenter;
            Vector3 gizmoPosition = TransformGizmo.InteractionPivotPosition;

            // Transform selected objects
            bool isPlayMode = _editor.StateMachine.IsPlayMode;
            for (int i = 0; i < selection.Count; i++)
            {
                var obj = selection[i];

                // Block transforming static objects in play mode
                if (isPlayMode && obj.CanTransform == false)
                    continue;
                var trans = obj.Transform;

                // Apply rotation
                if (applyRotation)
                {
                    Vector3 pivotOffset = trans.Translation - gizmoPosition;
                    if (useObjCenter || pivotOffset.IsZero)
                    {
                        //trans.Orientation *= rotationDelta;
                        trans.Orientation *= Quaternion.Invert(trans.Orientation) * rotationDelta * trans.Orientation;
                    }
                    else
                    {
                        Matrix.RotationQuaternion(ref trans.Orientation, out var transWorld);
                        Matrix.RotationQuaternion(ref rotationDelta, out var deltaWorld);
                        Matrix world = transWorld * Matrix.Translation(pivotOffset) * deltaWorld * Matrix.Translation(-pivotOffset);
                        trans.SetRotation(ref world);
                        trans.Translation += world.TranslationVector;
                    }
                }

                // Apply scale
                trans.Scale = TransformGizmoBase.ApplyScaleDelta(trans.Scale, scaleDelta);

                // Apply translation
                trans.Translation += translationDelta;

                obj.Transform = trans;
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            // Draw gizmo screen-space overlays
            foreach (var gizmo in Gizmos)
            {
                if (gizmo.Visible)
                {
                    gizmo.Draw();
                }
            }

            // Draw rubber band for rectangle selection
            _rubberBandSelector.Draw();

            if (IsCharacterControllerLookActive())
                DrawCharacterControllerReticle();
        }

        private void DrawCharacterControllerReticle()
        {
            var center = Size * 0.5f;
            var shadow = Color.Black.AlphaMultiplied(0.65f);
            var color = Color.White.AlphaMultiplied(0.85f);
            var left = center + new Float2(-10.0f, 0.0f);
            var right = center + new Float2(10.0f, 0.0f);
            var top = center + new Float2(0.0f, -10.0f);
            var bottom = center + new Float2(0.0f, 10.0f);

            Render2D.DrawLine(left, center + new Float2(-3.0f, 0.0f), shadow, 3.0f);
            Render2D.DrawLine(center + new Float2(3.0f, 0.0f), right, shadow, 3.0f);
            Render2D.DrawLine(top, center + new Float2(0.0f, -3.0f), shadow, 3.0f);
            Render2D.DrawLine(center + new Float2(0.0f, 3.0f), bottom, shadow, 3.0f);
            Render2D.DrawLine(left, center + new Float2(-3.0f, 0.0f), color, 1.0f);
            Render2D.DrawLine(center + new Float2(3.0f, 0.0f), right, color, 1.0f);
            Render2D.DrawLine(top, center + new Float2(0.0f, -3.0f), color, 1.0f);
            Render2D.DrawLine(center + new Float2(0.0f, 3.0f), bottom, color, 1.0f);
        }

        /// <inheritdoc />
        public override void OrientViewport(ref Quaternion orientation)
        {
            if (TransformGizmo.SelectedParents.Count != 0)
                FocusSelection(ref orientation);
            else
                base.OrientViewport(ref orientation);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            base.OnMouseMove(location);

            if (IsCharacterControllerLookActive())
            {
                ClearSceneTreeHoverFromEditorViewport();
                return;
            }

            if (Gizmos.ActiveMode?.OnMouseMove(location) == true)
            {
                ClearSceneTreeHoverFromEditorViewport();
                _rubberBandSelector.StopRubberBand();
                return;
            }
            bool csgMode = Gizmos.ActiveMode is CSGAuthoringGizmoMode;
            if (csgMode)
                ClearSceneTreeHoverFromEditorViewport();
            else
                UpdateSceneTreeHoverFromEditorViewport();

            // Don't allow rubber band selection when gizmo is controlling mouse, vertex painting mode, or cloth painting is enabled
            bool canStart = !(IsControllingMouse || IsRightMouseButtonDown || IsAltKeyDown) &&
                            (Gizmos?.Active is TransformGizmo || Gizmos?.Active is IViewportRubberBandSelection);
            _rubberBandSelector.TryCreateRubberBand(canStart, _viewMousePos);
        }

        /// <inheritdoc />
        protected override void OnControlMouseBegin(Window win)
        {
            if (IsCharacterControllerLookActive())
                _characterControllerControlMouseActive = true;
            else
                _rubberBandSelector.ReleaseRubberBandSelection();

            base.OnControlMouseBegin(win);
        }

        /// <inheritdoc />
        protected override void OnControlMouseEnd(Window win)
        {
            if (_characterControllerControlMouseActive)
                _characterControllerControlMouseActive = false;

            base.OnControlMouseEnd(win);
        }

        /// <inheritdoc />
        protected override void OnLeftMouseButtonDown()
        {
            base.OnLeftMouseButtonDown();
            if (Gizmos.ActiveMode?.OnMouseDown(_viewMousePos, MouseButton.Left) == true)
            {
                _rubberBandSelector.StopRubberBand();
                return;
            }
            if (Gizmos.Active is TransformGizmoBase transformGizmo)
                transformGizmo.ResetSelectionReleaseSuppression();

            if ((Gizmos.ActiveMode is TransformGizmoMode || Gizmos.ActiveMode is CSGAuthoringGizmoMode) && Root.GetMouseButtonDown(MouseButton.Left) && !IsAltKeyDown && !_directionGizmo.IsMouseOver)
            {
                _rubberBandSelector.TryStartingRubberBandSelection(_viewMousePos);
            }
        }

        /// <inheritdoc />
        protected override void OnLeftMouseButtonUp()
        {
            if (Gizmos.ActiveMode?.OnMouseUp(_viewMousePos, MouseButton.Left) == true)
            {
                _rubberBandSelector.ReleaseRubberBandSelection();
                Focus();
                base.OnLeftMouseButtonUp();
                return;
            }
            var rubberBandHandled = _rubberBandSelector.ReleaseRubberBandSelection();
            if (Gizmos.Active is TransformGizmoBase transformGizmo && transformGizmo.ConsumeSelectionRelease())
                return;
            if (_suppressNextSelectionPick)
            {
                _suppressNextSelectionPick = false;
                return;
            }

            // Skip if was controlling mouse or mouse is not over the area
            var containsViewMouse = ContainsPoint(ref _viewMousePos);
            if (_prevInput.IsControllingMouse || !containsViewMouse || _directionGizmo.IsMouseOver)
            {
                if (rubberBandHandled)
                    Focus();
                return;
            }

            // Select rubberbanded rect actor nodes or pick with gizmo
            if (!rubberBandHandled)
            {
                // Try to pick something with the current gizmo
                Gizmos.Active?.Pick();
            }

            // Keep focus
            Focus();

            base.OnLeftMouseButtonUp();
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            if (base.OnMouseDoubleClick(location, button))
                return true;
            if (Gizmos.ActiveMode?.OnMouseDoubleClick(location, button) == true)
                return true;
            if (button != MouseButton.Left || _gameViewActive || _characterControllerModeActive || IsControllingMouse || IsAltKeyDown || _directionGizmo.IsMouseOver || !ContainsPoint(ref location))
                return false;
            if (Gizmos.ActiveMode is not TransformGizmoMode)
                return false;

            var ray = ConvertMouseToRay(ref location);
            var view = new Ray(ViewPosition, ViewDirection);
            var renderView = Task.View;
            if (!TransformGizmo.TryDrillPick(ref ray, ref view, renderView.Flags, renderView.Mode, out var target))
                return false;

            _editor.SceneEditing.Select(target);
            _suppressNextSelectionPick = true;
            Focus();
            return true;
        }

        /// <inheritdoc />
        protected override void OnRightMouseButtonUp()
        {
            if (IsRightMouseButtonClick &&
                ContainsPoint(ref _viewMousePos) &&
                !_directionGizmo.IsMouseOver &&
                !_gameViewActive &&
                !_characterControllerModeActive &&
                !IsCharacterControllerLookActive() &&
                Gizmos.Active is TransformGizmo transformGizmo &&
                transformGizmo.ActiveAxis == TransformGizmoBase.Axis.None)
            {
                var ray = MouseRay;
                var view = new Ray(ViewPosition, ViewDirection);
                var renderView = RenderTask.View;
                var hit = transformGizmo.GetPickTarget(ref ray, ref view, renderView.Flags, renderView.Mode);

                if (hit != null)
                {
                    bool isSelected = _editor.SceneEditing.Selection.Contains(hit);
                    if (!isSelected)
                        _editor.SceneEditing.Select(hit);
                }

                Focus();
                OpenContextMenu();
            }

            base.OnRightMouseButtonUp();
        }

        /// <inheritdoc />
        protected override void OnMiddleMouseButtonDown()
        {
            base.OnMiddleMouseButtonDown();
            _middleMouseRecenterCandidate = !IsAltKeyDown;
        }

        /// <inheritdoc />
        protected override void OnMiddleMouseButtonUp()
        {
            if (_middleMouseRecenterCandidate && IsMiddleMouseButtonClick)
                TryRecenterCameraToMouseHit();
            _middleMouseRecenterCandidate = false;

            base.OnMiddleMouseButtonUp();
        }

        private void TryRecenterCameraToMouseHit()
        {
            if (IsAltKeyDown || _directionGizmo.IsMouseOver || !(ViewportCamera is FPSCamera fpsCamera) || SceneGraphRoot == null)
                return;

            var ray = ConvertMouseToRay(ref _viewMousePos);
            var view = new Ray(ViewPosition, ViewDirection);
            var flags = SceneGraphNode.RayCastData.FlagTypes.SkipColliders |
                        SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives |
                        SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
            var hit = SceneGraphRoot.RayCast(ref ray, ref view, out var distance, flags);
            if (hit == null || distance <= NearPlane || distance >= FarPlane)
                return;

            fpsCamera.RecenterView(ray.GetPoint(distance));
            _mouseDelta = Float2.Zero;
        }

        /// <summary>
        /// Gets the world-space point under the current viewport cursor.
        /// </summary>
        public Vector3 GetWorldPointUnderCursor()
        {
            var ray = MouseRay;
            var view = new Ray(ViewPosition, ViewDirection);
            if (SceneGraphRoot != null)
            {
                var flags = SceneGraphNode.RayCastData.FlagTypes.SkipColliders |
                            SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives |
                            SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
                var hit = SceneGraphRoot.RayCast(ref ray, ref view, out var distance, flags);
                if (hit != null && distance > NearPlane && distance < FarPlane)
                    return ray.GetPoint(distance);
            }

            var gridPlane = new Plane(Vector3.Zero, Vector3.Up);
            if (Grid.Enabled && CollisionsHelper.RayIntersectsPlane(ref ray, ref gridPlane, out Real gridDistance) && gridDistance > NearPlane && gridDistance < 4000.0f)
                return ray.GetPoint(gridDistance);
            return ray.GetPoint(100.0f);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (base.OnMouseUp(location, button))
                return true;

            // Handle mouse going up when using rubber band with mouse capture that click up outside the view
            if (button == MouseButton.Left && !new Rectangle(Float2.Zero, Size).Contains(ref location))
            {
                _rubberBandSelector.ReleaseRubberBandSelection();
                return true;
            }

            return false;
        }

        private bool IsCharacterControllerMovementKey(KeyboardKeys key)
        {
            var input = _editor.Options.Options.Input;
            return key == input.Forward.Key
                || key == input.Backward.Key
                || key == input.Left.Key
                || key == input.Right.Key;
        }

        private bool ProcessCharacterControllerModeShortcut(KeyboardKeys key)
        {
            if (key != KeyboardKeys.G)
                return false;

            var root = Root;
            if ((root?.GetKey(KeyboardKeys.Control) ?? false) || (root?.GetKey(KeyboardKeys.Alt) ?? false))
                return false;

            if (root?.GetKey(KeyboardKeys.Shift) ?? false)
                ToggleGameView();
            else
                ToggleCharacterControllerMode();
            return true;
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            var input = _editor.Options.Options.Input;
            if (Gizmos.Active is TransformGizmoBase transformGizmo &&
                input.Undo.Process(this, key) &&
                transformGizmo.TryCancelPointerInteractionForUndo())
            {
                return true;
            }

            if (_characterControllerModeActive)
            {
                if (key == KeyboardKeys.Escape)
                {
                    StopCharacterControllerMode();
                    return true;
                }

                if (ProcessCharacterControllerModeShortcut(key))
                    return true;

                var root = Root;
                bool shortcutModifier = (root?.GetKey(KeyboardKeys.Control) ?? false) || (root?.GetKey(KeyboardKeys.Alt) ?? false);
                if (IsCharacterControllerLookActive() && !shortcutModifier && (key == KeyboardKeys.Shift || key == KeyboardKeys.Spacebar || IsCharacterControllerMovementKey(key)))
                    return true;
            }

            if (Gizmos.ActiveMode?.OnKeyDown(key) == true)
                return true;

            if (ProcessCharacterControllerModeShortcut(key))
                return true;

            if (key == KeyboardKeys.Escape &&
                Gizmos.Active == TransformGizmo &&
                !TransformGizmo.HasActiveTransaction &&
                TransformGizmo.ActiveAxis == TransformGizmoBase.Axis.None &&
                TransformGizmo.TryExitSelectionScope(out var scope))
            {
                _editor.SceneEditing.Select(scope);
                return true;
            }

            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        public override void OnKeyUp(KeyboardKeys key)
        {
            if (Gizmos.ActiveMode?.OnKeyUp(key) == true)
                return;
            base.OnKeyUp(key);
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragEnter(ref Float2 location, DragData data)
        {
            DragHandlers.ClearDragEffects();
            var result = base.OnDragEnter(ref location, data);
            if (result != DragDropEffect.None)
                return result;
            return DragHandlers.DragEnter(ref location, data);
        }

        private bool ValidateDragItem(ContentItem contentItem)
        {
            if (!Level.IsAnySceneLoaded)
                return false;

            if (contentItem is AssetItem assetItem)
            {
                if (assetItem.OnEditorDrag(this))
                    return true;
                if (assetItem.IsOfType<MaterialBase>())
                    return true;
                if (assetItem.IsOfType<SceneAsset>())
                    return true;
            }

            return false;
        }

        private static bool ValidateDragActorType(ScriptType actorType)
        {
            return Level.IsAnySceneLoaded && Editor.Instance.CodeEditing.Actors.Get().Contains(actorType);
        }

        private static bool ValidateDragScriptItem(ScriptItem script)
        {
            return Level.IsAnySceneLoaded && Editor.Instance.CodeEditing.Actors.Get(script) != ScriptType.Null;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragMove(ref Float2 location, DragData data)
        {
            DragHandlers.ClearDragEffects();
            var result = base.OnDragMove(ref location, data);
            if (result != DragDropEffect.None)
                return result;
            return DragHandlers.DragEnter(ref location, data);
        }

        /// <inheritdoc />
        public override void OnDragLeave()
        {
            DragHandlers.ClearDragEffects();
            DragHandlers.OnDragLeave();
            base.OnDragLeave();
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragDrop(ref Float2 location, DragData data)
        {
            DragHandlers.ClearDragEffects();
            var result = base.OnDragDrop(ref location, data);
            if (result != DragDropEffect.None)
                return result;
            return DragHandlers.DragDrop(ref location, data);
        }

        /// <inheritdoc />
        public override void Select(List<SceneGraphNode> nodes, bool recordUndo = true)
        {
            _editor.SceneEditing.Select(nodes, recordUndo: recordUndo);
        }

        /// <inheritdoc />
        public override bool TryDuplicateForTransform(out List<SceneGraphNode> createdObjects, out IUndoAction undoAction)
        {
            return _editor.SceneEditing.TryDuplicateForTransform(out createdObjects, out undoAction);
        }

        /// <inheritdoc />
        public override void Spawn(Actor actor)
        {
            var parent = actor.Parent ?? Level.GetScene(0);
            actor.Name = Utilities.Utils.IncrementNameNumber(actor.Name, x => parent.GetChild(x) == null);
            _editor.SceneEditing.Spawn(actor);
        }

        /// <inheritdoc />
        public override void OpenContextMenu()
        {
            var mouse = PointFromWindow(Root.MousePosition);
            _editor.Windows.SceneWin.ShowContextMenu(this, mouse);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (IsDisposing)
                return;

            ClearSceneTreeHoverFromEditorViewport();
            StopCharacterControllerMode();
            Gizmos.ActiveModeChanged -= OnActiveGizmoModeChanged;
            if (CSGAuthoringMode?.Controller != null)
                CSGAuthoringMode.Controller.Changed -= UpdateViewportToolStrip;
            _editor.PlayModeBeginning -= OnPlayModeBeginning;
            Level.SceneLoaded -= OnSceneContextChanged;
            Level.SceneUnloading -= OnSceneContextChanged;
            _debugDrawData.Dispose();
            if (_task != null)
            {
                // Release if task is not used to save screenshot for project icon
                Object.Destroy(ref _task);
                ReleaseResources();
            }

            base.OnDestroy();
        }

        private RenderTask _savedTask;
        private GPUTexture _savedBackBuffer;

        internal void SaveProjectIcon()
        {
            TakeScreenshot(StringUtils.CombinePaths(Globals.ProjectCacheFolder, "icon.png"));

            _savedTask = _task;
            _savedBackBuffer = _backBuffer;

            _task = null;
            _backBuffer = null;
        }

        internal void SaveProjectIconEnd()
        {
            if (_savedTask)
            {
                _savedTask.Enabled = false;
                Object.Destroy(_savedTask);
                ReleaseResources();
                _savedTask = null;
            }
            Object.Destroy(ref _savedBackBuffer);
        }

        private void ReleaseResources()
        {
            if (Task)
            {
                Task.RemoveCustomPostFx(SelectionOutline);
                Task.RemoveCustomPostFx(ViewportHoverOutline);
                Task.RemoveCustomPostFx(SceneTreeHoverOutline);
                Task.RemoveCustomPostFx(EditorPrimitives);
                Task.RemoveCustomPostFx(_editorSpritesRenderer);
                Task.RemoveCustomPostFx(_customSelectionOutline);
            }
            Object.Destroy(ref SelectionOutline);
            Object.Destroy(ref ViewportHoverOutline);
            Object.Destroy(ref SceneTreeHoverOutline);
            Object.Destroy(ref EditorPrimitives);
            Object.Destroy(ref _editorSpritesRenderer);
            Object.Destroy(ref _customSelectionOutline);
        }
    }
}
