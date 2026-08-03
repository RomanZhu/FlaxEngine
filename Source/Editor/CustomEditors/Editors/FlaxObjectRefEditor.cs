// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Content;
using FlaxEditor.CustomEditors.Elements;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Drag;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.GUI;
using FlaxEditor.Scripting;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Utilities;
using Object = FlaxEngine.Object;

namespace FlaxEditor.CustomEditors.Editors
{
    /// <summary>
    /// A custom control type used to pick reference to <see cref="FlaxEngine.Object"/>.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.Control" />
    [HideInEditor]
    public class FlaxObjectRefPickerControl : Control, ITooltipPreviewProvider
    {
        private const float FieldHeight = 22.0f;
        private const float ButtonSize = 16.0f;
        private const float IconSize = 12.0f;

        private ScriptType _type;
        private ActorTreeNode _linkedTreeNode;
        private Object _value;
        private string _valueName;
        private string _valueTypeName;
        private bool _supportsPickDropDown;

        private bool _isMouseDown;
        private Float2 _mouseDownPos;
        private Float2 _mousePos;

        private bool _hasValidDragOver;
        private DragActors _dragActors;
        private DragActors _dragActorsWithScript;
        private DragAssets _dragAssets;
        private DragScripts _dragScripts;
        private DragHandlers _dragHandlers;

        /// <summary>
        /// The presenter using this control.
        /// </summary>
        public IPresenterOwner PresenterContext;

        /// <summary>
        /// Gets or sets the allowed objects type (given type and all subclasses). Must be <see cref="Object"/> type of any subclass.
        /// </summary>
        public ScriptType Type
        {
            get => _type;
            set
            {
                if (_type == value)
                    return;
                if (value == ScriptType.Null || (value.Type != typeof(Object) && !value.IsSubclassOf(ScriptType.Object)))
                    throw new ArgumentException(string.Format("Invalid type for FlaxObjectRefEditor. Input type: {0}", value != ScriptType.Null ? value.TypeName : "null"));

                _type = value;
                _supportsPickDropDown = new ScriptType(typeof(Actor)).IsAssignableFrom(value) || 
                                        new ScriptType(typeof(Script)).IsAssignableFrom(value);

                // Deselect value if it's not valid now
                if (!IsValid(_value))
                    Value = null;
            }
        }

        /// <summary>
        /// Gets or sets the selected object value.
        /// </summary>
        public Object Value
        {
            get => _value;
            set
            {
                if (_value == value)
                    return;
                if (!IsValid(value))
                    value = null;

                // Special case for missing objects (eg. referenced actor in script that is deleted in editor)
                if (value != null && (Object.GetUnmanagedPtr(value) == IntPtr.Zero || value.ID == Guid.Empty))
                    value = null;

                _value = value;
                var type = TypeUtils.GetObjectType(_value);
                _valueTypeName = GetTypeDisplayName(type);

                // Get name to display
                if (_value is Script script)
                    _valueName = script.Actor ? script.Actor.Name : type.Name;
                else if (_value != null)
                    _valueName = _value.ToString();
                else
                    _valueName = string.Empty;

                // Update tooltip
                if (_value is SceneObject sceneObject)
                    TooltipText = Utilities.Utils.GetTooltip(sceneObject);
                else
                    TooltipText = string.Empty;

                OnValueChanged();
            }
        }

        /// <summary>
        /// Gets or sets the selected object value by identifier.
        /// </summary>
        public Guid ValueID
        {
            get => _value ? _value.ID : Guid.Empty;
            set => Value = Object.Find<Object>(ref value);
        }

        /// <summary>
        /// Occurs when value gets changed.
        /// </summary>
        public event Action ValueChanged;

        /// <summary>
        /// The custom callback for objects validation. Cane be used to implement a rule for objects to pick.
        /// </summary>
        public Func<Object, ScriptType, bool> CheckValid;

        /// <summary>
        /// Utility flag used to indicate that there are different values assigned to this reference editor and user should be informed about it.
        /// </summary>
        public bool DifferentValues;

        /// <summary>
        /// Custom background color used by the reference field.
        /// </summary>
        public Color ReferenceBackgroundColor = Color.Transparent;

        /// <inheritdoc />
        public SpriteHandle TooltipPreview => GetSelectedAssetItem()?.TooltipPreview ?? SpriteHandle.Invalid;

        /// <summary>
        /// Initializes a new instance of the <see cref="FlaxObjectRefPickerControl"/> class.
        /// </summary>
        public FlaxObjectRefPickerControl()
        : base(0, 0, 50, FieldHeight)
        {
            _type = ScriptType.Object;
        }

        /// <summary>
        /// Object validation check routine.
        /// </summary>
        /// <param name="obj">Input object to check.</param>
        /// <returns>True if it can be assigned, otherwise false.</returns>
        protected virtual bool IsValid(Object obj)
        {
            var type = TypeUtils.GetObjectType(obj);
            return obj == null || _type.IsAssignableFrom(type) && (CheckValid == null || CheckValid(obj, type));
        }

        private Rectangle FieldRect => new Rectangle(0, 0, Width, Height);

        private Rectangle PickerButtonRect => new Rectangle(1, (Height - ButtonSize) * 0.5f, ButtonSize, ButtonSize);

        private Rectangle SearchButtonRect => new Rectangle(Width - ButtonSize - 1, (Height - ButtonSize) * 0.5f, ButtonSize, ButtonSize);

        private Rectangle TextRect => new Rectangle(ButtonSize + 3, 0, Mathf.Max(0.0f, Width - ButtonSize * 2 - 6), Height);

        private static string GetTypeDisplayName(ScriptType type)
        {
            return type.Type != null ? type.Type.GetTypeDisplayName() : Utilities.Utils.GetPropertyNameUI(type.ToString());
        }

        private string GetExpectedTypeName()
        {
            return GetTypeDisplayName(_type);
        }

        private string GetDisplayText()
        {
            if (DifferentValues)
                return $"Multiple Values ({GetExpectedTypeName()})";
            if (_value != null)
                return $"{_valueName} ({_valueTypeName})";
            return $"None ({GetExpectedTypeName()})";
        }

        private static void DrawPickerIcon(Rectangle rect, Color color)
        {
            var center = new Float2(rect.X + rect.Width * 0.5f, rect.Y + rect.Height * 0.5f);
            Render2D.DrawLine(new Float2(rect.X + 3, center.Y), new Float2(center.X - 2, center.Y), color);
            Render2D.DrawLine(new Float2(center.X + 2, center.Y), new Float2(rect.Right - 3, center.Y), color);
            Render2D.DrawLine(new Float2(center.X, rect.Y + 3), new Float2(center.X, center.Y - 2), color);
            Render2D.DrawLine(new Float2(center.X, center.Y + 2), new Float2(center.X, rect.Bottom - 3), color);
            Render2D.DrawRectangle(new Rectangle(center.X - 2, center.Y - 2, 4, 4), color);
        }

        private void ShowDropDownMenu()
        {
            Focus();
            if (new ScriptType(typeof(Actor)).IsAssignableFrom(_type))
            {
                ActorSearchPopup.Show(this, new Float2(0, Height), IsValid, actor =>
                {
                    Value = actor;
                    RootWindow.Focus();
                    Focus();
                }, PresenterContext);
            }
            else if (new ScriptType(typeof(Control)).IsAssignableFrom(_type))
            {
                ActorSearchPopup.Show(this, new Float2(0, Height), IsValid, actor =>
                {
                    Value = actor as UIControl;
                    RootWindow.Focus();
                    Focus();
                }, PresenterContext);
            }
            else
            {
                ScriptSearchPopup.Show(this, new Float2(0, Height), IsValid, script =>
                {
                    Value = script;
                    RootWindow.Focus();
                    Focus();
                }, PresenterContext);
            }
        }

        /// <summary>
        /// Called when value gets changed.
        /// </summary>
        protected virtual void OnValueChanged()
        {
            ValueChanged?.Invoke();
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            var style = Style.Current;
            var isSelected = _value != null;
            var isEnabled = VisuallyEnabledInHierarchy;
            var fieldRect = FieldRect;
            var pickerRect = PickerButtonRect;
            var searchRect = SearchButtonRect;
            var textRect = TextRect;
            var borderColor = isEnabled && (IsMouseOver || IsFocused || IsNavFocused) ? style.BorderHighlighted : style.BorderNormal;
            var textColor = isEnabled && isSelected && !DifferentValues ? style.Foreground : style.ForegroundGrey;
            var iconColor = isEnabled ? style.ForegroundGrey : style.ForegroundDisabled;
            var backgroundColor = ReferenceBackgroundColor.A > 0.0f ? ReferenceBackgroundColor : style.TextBoxBackground;

            StyleRendering.DrawRoundedRectangle(fieldRect, backgroundColor, borderColor, 1.0f, style.CornerRadius);

            if (isEnabled)
            {
                var pickerIconRect = new Rectangle(pickerRect.X + (pickerRect.Width - IconSize) * 0.5f, pickerRect.Y + (pickerRect.Height - IconSize) * 0.5f, IconSize, IconSize);
                DrawPickerIcon(pickerIconRect, pickerRect.Contains(_mousePos) ? style.Foreground : iconColor);
                if (_supportsPickDropDown)
                    Render2D.DrawSprite(style.Search, searchRect, searchRect.Contains(_mousePos) ? style.Foreground : iconColor);
            }

            Render2D.PushClip(textRect);
            Render2D.DrawText(style.FontMedium, GetDisplayText(), textRect, textColor, TextAlignment.Near, TextAlignment.Center);
            Render2D.PopClip();

            // Check if drag is over
            if (IsDragOver && _hasValidDragOver)
            {
                StyleRendering.DrawRoundedRectangle(fieldRect, style.Selection, style.SelectionBorder, 1.0f, style.CornerRadius);
            }
        }

        /// <inheritdoc />
        public override void OnMouseEnter(Float2 location)
        {
            _mousePos = location;
            _mouseDownPos = Float2.Minimum;

            base.OnMouseEnter(location);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            _mousePos = Float2.Minimum;

            // Check if start drag drop
            if (_isMouseDown)
            {
                // Do the drag
                DoDrag();

                // Clear flag
                _isMouseDown = false;
            }

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            _mousePos = location;

            // Check if start drag drop
            var dragRect = TextRect;
            if (_isMouseDown && Float2.Distance(location, _mouseDownPos) > 10.0f && dragRect.Contains(_mouseDownPos))
            {
                // Do the drag
                DoDrag();

                // Clear flag
                _isMouseDown = false;
            }

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            _isMouseDown = false;

            if (button == MouseButton.Right)
            {
                Focus();
                ShowContextMenu(location);
                return true;
            }

            if (button == MouseButton.Left)
            {
                Focus();
                if (VisuallyEnabledInHierarchy && PickerButtonRect.Contains(ref location))
                {
                    if (!TryAssignCurrentSelection() && _supportsPickDropDown)
                        ShowDropDownMenu();
                    return true;
                }
                if (VisuallyEnabledInHierarchy && _supportsPickDropDown && SearchButtonRect.Contains(ref location))
                {
                    ShowDropDownMenu();
                    return true;
                }
                if (VisuallyEnabledInHierarchy && TextRect.Contains(ref location))
                {
                    FocusSource();
                    return true;
                }

                // Reset valid drag over if still true at this point
                if (_hasValidDragOver)
                    _hasValidDragOver = false;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left)
            {
                // Set flag
                _isMouseDown = true;
                _mouseDownPos = location;
            }

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            Focus();
            return true;
        }

        /// <inheritdoc />
        public override void OnSubmit()
        {
            base.OnSubmit();

            // Picker dropdown menu
            if (_supportsPickDropDown)
                ShowDropDownMenu();
        }

        private Actor GetReferenceActor()
        {
            if (_value is Actor actor)
                return actor;
            if (_value is Script script)
                return script.Actor;
            return null;
        }

        private ContentItem GetSelectedAssetItem()
        {
            return _value is Asset asset ? Editor.Instance.ContentDatabase.FindAsset(asset.ID) : null;
        }

        /// <inheritdoc />
        public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
        {
            var item = GetSelectedAssetItem();
            if (item != null)
            {
                item.UpdateTooltipText();
                TooltipText = item.TooltipText;
            }

            return base.OnShowTooltip(out text, out location, out area);
        }

        private bool FindReference()
        {
            var actor = GetReferenceActor();
            if (actor == null)
                return false;

            if (_linkedTreeNode != null && _linkedTreeNode.Actor == actor)
            {
                HighlightLinkedTreeNode();
                return true;
            }

            if (PresenterContext is PropertiesWindow || PresenterContext == null)
                _linkedTreeNode = Editor.Instance.Scene.GetActorNode(actor)?.TreeNode;
            else if (PresenterContext is PrefabWindow prefabWindow)
                _linkedTreeNode = prefabWindow.Graph.Root.Find(actor)?.TreeNode;
            if (_linkedTreeNode == null)
                return false;

            HighlightLinkedTreeNode();
            return true;
        }

        private void HighlightLinkedTreeNode()
        {
            _linkedTreeNode.ExpandAllParents();
            if (PresenterContext is PropertiesWindow || PresenterContext == null)
                Editor.Instance.Windows.SceneWin.SceneTreePanel.ScrollViewTo(_linkedTreeNode, true);
            else if (PresenterContext is PrefabWindow prefabWindow)
                (prefabWindow.Tree.Parent as Panel)?.ScrollViewTo(_linkedTreeNode, true);
            _linkedTreeNode.StartHighlight();
        }

        private void FocusSource()
        {
            var actor = GetReferenceActor();
            if (actor != null)
            {
                FindReference();
            }
            else if (_value is Asset asset)
            {
                var item = Editor.Instance.ContentDatabase.FindAsset(asset.ID);
                if (item != null)
                    Editor.Instance.Windows.ContentWin.Highlight(item, true);
            }
        }

        private void ShowContextMenu(Float2 location)
        {
            var menu = new ContextMenu();
            var clear = menu.AddButton("Clear", () => Value = null);
            clear.Enabled = VisuallyEnabledInHierarchy && _value != null;

            if (_value != null)
            {
                menu.AddSeparator();

                var actor = GetReferenceActor();
                if (actor != null)
                {
                    menu.AddButton("Find Reference", () => FindReference());
                    menu.AddButton("Focus on Source", FocusSource);
                }

                if (_value is Asset asset)
                {
                    var item = Editor.Instance.ContentDatabase.FindAsset(asset.ID);
                    if (item != null)
                    {
                        menu.AddButton("Find References", () => Editor.Instance.Windows.Open(new AssetReferencesGraphWindow(Editor.Instance, item)));
                        menu.AddButton("Open", () => Editor.Instance.ContentEditing.Open(item));
                        menu.AddButton("Focus on Source", FocusSource);
                    }
                }
            }

            menu.Show(this, location);
        }

        private Object GetObjectFromActor(Actor actor)
        {
            if (actor == null)
                return null;
            if (IsValid(actor))
                return actor;
            return actor.Scripts.FirstOrDefault(IsValid);
        }

        private Object GetObjectFromSelection(IReadOnlyList<SceneGraphNode> selection)
        {
            if (selection == null || selection.Count != 1)
                return null;

            var node = selection[0];
            if (node.EditableObject is Object obj && IsValid(obj))
                return obj;
            return node is ActorNode actorNode ? GetObjectFromActor(actorNode.Actor) : null;
        }

        private Object GetObjectFromCurrentSelection()
        {
            return PresenterContext is PrefabWindow prefabWindow
                ? GetObjectFromSelection(prefabWindow.Selection)
                : GetObjectFromSelection(Editor.Instance.SceneEditing.Selection);
        }

        private bool TryAssignCurrentSelection()
        {
            var obj = GetObjectFromCurrentSelection();
            if (obj == null || obj == _value)
                return false;

            Value = obj;
            RootWindow.Focus();
            Focus();
            return true;
        }

        private void DoDrag()
        {
            // Do the drag drop operation if has selected element
            var dragRect = TextRect;
            if (_value != null && dragRect.Contains(ref _mouseDownPos))
            {
                if (_value is Actor actor)
                    DoDragDrop(DragActors.GetDragData(actor));
                else if (_value is Asset asset)
                    DoDragDrop(DragAssets.GetDragData(asset));
                else if (_value is Script script)
                    DoDragDrop(DragScripts.GetDragData(script));
            }
        }

        private DragDropEffect DragEffect => _hasValidDragOver ? DragDropEffect.Move : DragDropEffect.None;

        /// <inheritdoc />
        public override DragDropEffect OnDragEnter(ref Float2 location, DragData data)
        {
            base.OnDragEnter(ref location, data);

            // Ensure to have valid drag helpers (uses lazy init)
            if (_dragActors == null)
                _dragActors = new DragActors(ValidateDragActor);
            if (_dragActorsWithScript == null)
                _dragActorsWithScript = new DragActors(ValidateDragActorWithScript);
            if (_dragAssets == null)
                _dragAssets = new DragAssets(ValidateDragAsset);
            if (_dragScripts == null)
                _dragScripts = new DragScripts(ValidateDragScript);
            if (_dragHandlers == null)
            {
                _dragHandlers = new DragHandlers
                {
                    _dragActors,
                    _dragActorsWithScript,
                    _dragAssets,
                    _dragScripts,
                };
            }

            _hasValidDragOver = _dragHandlers.OnDragEnter(data) != DragDropEffect.None;

            // Special case when dragging the actor with script to link script reference
            if (_dragActorsWithScript.HasValidDrag)
            {
                var script = _dragActorsWithScript.Objects[0].Actor.Scripts.First(IsValid);
                _dragActorsWithScript.Objects.Clear();
                _dragScripts.Objects.Add(script);
            }

            return DragEffect;
        }

        private bool ValidateDragActor(ActorNode a)
        {
            if (!IsValid(a.Actor))
                return false;
            
            if (PresenterContext is PrefabWindow prefabWindow)
            {
                if (prefabWindow.Tree == a.TreeNode.ParentTree)
                    return true;
            }
            else if (PresenterContext is PropertiesWindow || PresenterContext == null)
            {
                if (a.ParentScene != null)
                    return true;
            }
            return false;
        }

        private bool ValidateDragScript(Script script)
        {
            if (!IsValid(script))
                return false;
            
            if (PresenterContext is PrefabWindow prefabWindow)
            {
                var actorNode = prefabWindow.Graph.Root.Find(script.Actor);
                if (actorNode != null)
                    return true;
            }
            else if (PresenterContext is PropertiesWindow || PresenterContext == null)
            {
                if (script.Actor.HasScene)
                    return true;
            }
            return false;
        }

        private bool ValidateDragAsset(AssetItem assetItem)
        {
            // Check if can accept assets
            if (!new ScriptType(typeof(Asset)).IsAssignableFrom(_type))
                return false;

            // Load or get asset
            var id = assetItem.ID;
            var obj = Object.Find<Asset>(ref id);
            if (obj == null)
                return false;

            // Check it
            return IsValid(obj);
        }

        private bool ValidateDragActorWithScript(ActorNode node)
        {
            bool isCorrectContext = false;
            if (PresenterContext is PrefabWindow prefabWindow)
            {
                if (prefabWindow.Tree == node.TreeNode.ParentTree)
                    isCorrectContext =  true;
            }
            else if (PresenterContext is PropertiesWindow || PresenterContext == null)
            {
                if (node.ParentScene != null)
                    isCorrectContext =  true;
            }
            return node.Actor.Scripts.Any(IsValid) && isCorrectContext;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragMove(ref Float2 location, DragData data)
        {
            base.OnDragMove(ref location, data);

            return DragEffect;
        }

        /// <inheritdoc />
        public override void OnDragLeave()
        {
            _hasValidDragOver = false;
            _dragHandlers.OnDragLeave();

            base.OnDragLeave();
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragDrop(ref Float2 location, DragData data)
        {
            var result = DragEffect;

            base.OnDragDrop(ref location, data);

            if (_dragActors.HasValidDrag)
            {
                Value = _dragActors.Objects[0].Actor;
            }
            else if (_dragAssets.HasValidDrag)
            {
                ValueID = _dragAssets.Objects[0].ID;
            }
            else if (_dragScripts.HasValidDrag)
            {
                ValueID = _dragScripts.Objects[0].ID;
            }

            return result;
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _value = null;
            _type = ScriptType.Null;
            _valueName = null;
            _valueTypeName = null;
            _linkedTreeNode = null;

            base.OnDestroy();
        }
    }

    /// <summary>
    /// Default implementation of the inspector used to edit reference to the <see cref="FlaxEngine.Object"/>.
    /// </summary>
    [CustomEditor(typeof(Object)), DefaultEditor]
    public sealed class FlaxObjectRefEditor : CustomEditor
    {
        private CustomElement<FlaxObjectRefPickerControl> _element;

        /// <inheritdoc />
        public override DisplayStyle Style => DisplayStyle.Inline;

        /// <inheritdoc />
        public override void Initialize(LayoutElementsContainer layout)
        {
            if (!HasDifferentTypes)
            {
                _element = layout.Custom<FlaxObjectRefPickerControl>();
                _element.CustomControl.PresenterContext = Presenter.Owner;
                _element.CustomControl.Type = Values.Type.Type != typeof(object) || Values[0] == null ? Values.Type : TypeUtils.GetObjectType(Values[0]);
                _element.CustomControl.ValueChanged += () => SetValue(_element.CustomControl.Value);
            }
        }

        /// <inheritdoc />
        public override void Refresh()
        {
            base.Refresh();

            var differentValues = HasDifferentValues;
            _element.CustomControl.DifferentValues = differentValues;
            if (!differentValues)
            {
                _element.CustomControl.Value = Values[0] as Object;
            }
        }
    }
}
