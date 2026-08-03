// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Drag;
using FlaxEditor.GUI.Tree;
using FlaxEditor.Options;
using FlaxEditor.Scripting;
using FlaxEditor.Utilities;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Utilities;

namespace FlaxEditor.SceneGraph.GUI
{
    /// <summary>
    /// Tree node GUI control used as a proxy object for actors hierarchy.
    /// </summary>
    /// <seealso cref="TreeNode" />
    public class ActorTreeNode : TreeNode
    {
        private int _orderInParent;
        private DragActors _dragActors;
        private DragScripts _dragScripts;
        private DragAssets _dragAssets;
        private DragActorType _dragActorType;
        private DragControlType _dragControlType;
        private DragScriptItems _dragScriptItems;
        private DragHandlers _dragHandlers;
        private List<Rectangle> _highlights;
        private float _highlightsTextLeftOffset;
        private bool _hasSearchFilter;
        private bool _activeCheckboxPressed;
        private string _sceneIconTooltip;
        private string _shownSceneIconTooltip;
        private Rectangle _sceneIconTooltipArea;

        private const float SceneIconSize = 16.0f;
        private const float SceneIconSpacing = 3.0f;
        private const float SceneIconRightMargin = 4.0f;
        private const float SceneIconTextPadding = 6.0f;
        private const float ActiveCheckboxSize = 12.0f;
        private const float ActiveCheckboxSpacing = 4.0f;
        private const float ActiveCheckboxColumnWidth = ActiveCheckboxSize + ActiveCheckboxSpacing;

        private static bool _sceneTypeIconsLoaded;
        private static Texture _iconPointLight;
        private static Texture _iconDirectionalLight;
        private static Texture _iconEnvironmentProbe;
        private static Texture _iconSkybox;
        private static Texture _iconSkyLight;
        private static Texture _iconAudioListener;
        private static Texture _iconAudioSource;
        private static Texture _iconDecal;
        private static Texture _iconParticleEffect;
        private static Texture _iconSceneAnimationPlayer;

        private struct SceneRowIcon
        {
            public SpriteHandle Sprite;
            public Texture Texture;
            public SemanticIcons.Glyph Glyph;
            public bool UseGlyph;
            public Color Color;
            public string Tooltip;

            public bool IsValid => UseGlyph || Sprite.IsValid || Texture != null;
        }

        /// <summary>
        /// The actor node that owns this node.
        /// </summary>
        protected ActorNode _actorNode;

        /// <summary>
        /// Gets the actor.
        /// </summary>
        public Actor Actor => _actorNode.Actor;

        /// <summary>
        /// Gets the actor node.
        /// </summary>
        public ActorNode ActorNode => _actorNode;

        /// <inheritdoc />
        protected override float HeaderTextLeftOffset => SceneIconSize + 4.0f;

        /// <inheritdoc />
        public override float MinimumWidth => base.MinimumWidth + ActiveCheckboxColumnWidth;

        private bool ShouldDrawActiveCheckbox
        {
            get
            {
                var actor = _actorNode?.Actor;
                return actor && (IsMouseOverHeader || _activeCheckboxPressed);
            }
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ActorTreeNode"/> class.
        /// </summary>
        public ActorTreeNode()
        : base(true)
        {
            ChildrenIndent = 16.0f;
            TextMargin = new Margin(ActiveCheckboxColumnWidth + 2.0f, 2.0f, 2.0f, 2.0f);
        }

        internal virtual void LinkNode(ActorNode node)
        {
            _actorNode = node;
            var actor = node.Actor;
            if (actor != null)
            {
                _orderInParent = actor.OrderInParent;
                Visible = (actor.HideFlags & HideFlags.HideInHierarchy) == 0;

                // Pick the correct id when inside a prefab window.
                var id = actor.HasPrefabLink && !actor.HasScene ? actor.PrefabObjectID : actor.ID;
                if (Editor.Instance.ProjectCache.IsExpandedActor(ref id))
                {
                    Expand(true);
                }
            }
            else
            {
                _orderInParent = 0;
            }

            UpdateText();
        }

        internal void OnParentChanged(Actor actor, ActorNode parentNode)
        {
            // Update cached value
            _orderInParent = actor.OrderInParent;

            // Update UI (special case if actor is spawned and added to existing scene tree)
            var parentTreeNode = parentNode?.TreeNode;
            if (parentTreeNode != null && !parentTreeNode.IsLayoutLocked)
            {
                parentTreeNode.IsLayoutLocked = true;
                Parent = parentTreeNode;
                IndexInParent = _orderInParent;
                parentTreeNode.IsLayoutLocked = false;

                // Skip UI update if node won't be in a view
                if (parentTreeNode.IsCollapsedInHierarchy)
                {
                    UnlockChildrenRecursive();
                }
                else
                {
                    // Try to perform layout at the level where it makes it the most performant (the least computations)
                    var tree = parentTreeNode.ParentTree;
                    if (tree != null)
                    {
                        if (tree.Parent is Panel treeParent)
                            treeParent.PerformLayout();
                        else
                            tree.PerformLayout();
                    }
                    else
                    {
                        parentTreeNode.PerformLayout();
                    }
                }
            }
            else
            {
                Parent = parentTreeNode;
            }
        }

        internal void OnOrderInParentChanged()
        {
            // Use cached value to check if we need to update UI layout (and update siblings order at once)
            if (Parent is ActorTreeNode parent)
            {
                var anyChanged = false;
                var children = parent.Children;
                for (int i = 0; i < children.Count; i++)
                {
                    if (children[i] is ActorTreeNode child && child.Actor)
                    {
                        var orderInParent = child.Actor.OrderInParent;
                        anyChanged |= child._orderInParent != orderInParent;
                        if (anyChanged)
                            child._orderInParent = orderInParent;
                    }
                }
                if (anyChanged)
                    parent.SortChildren();
            }
            else if (Actor)
            {
                _orderInParent = Actor.OrderInParent;
            }
        }

        /// <summary>
        /// Updates the tree node text.
        /// </summary>
        public virtual void UpdateText()
        {
            Text = _actorNode.Name;
        }

        /// <summary>
        /// Updates the query search filter.
        /// </summary>
        /// <param name="filterText">The filter text.</param>
        public void UpdateFilter(string filterText)
        {
            // Skip hidden actors
            var actor = Actor;
            if (actor != null && (actor.HideFlags & HideFlags.HideInHierarchy) != 0)
                return;

            bool noFilter = string.IsNullOrWhiteSpace(filterText);
            _hasSearchFilter = !noFilter;

            // Update itself
            bool isThisVisible;
            if (noFilter)
            {
                // Clear filter
                _highlights?.Clear();
                isThisVisible = true;
            }
            else if (filterText.Contains(':'))
            {
                var splitFilter = filterText.Split(',');
                var hasAllFilters = true;
                foreach (var filter in splitFilter)
                {
                    if (string.IsNullOrEmpty(filter))
                        continue;
                    var trimmedFilter = filter.Trim();
                    var hasFilter = false;
                    
                    // Check if script
                    if (trimmedFilter.Contains("s:", StringComparison.OrdinalIgnoreCase))
                    {
                        // Check for any scripts
                        if (trimmedFilter.Equals("s:", StringComparison.OrdinalIgnoreCase))
                        {
                            if (actor != null && actor.ScriptsCount > 0)
                                hasFilter = true;
                        }
                        else
                        {
                            var scriptText = trimmedFilter.Replace("s:", "", StringComparison.OrdinalIgnoreCase).Trim();
                            var scriptFound = false;
                            if (actor != null)
                            {
                                var scripts = actor.Scripts;
                                foreach (var script in scripts)
                                {
                                    var name = TypeUtils.GetTypeDisplayName(script.GetType());
                                    var nameNoSpaces = name.Replace(" ", "");
                                    if (name.Contains(scriptText, StringComparison.OrdinalIgnoreCase) || nameNoSpaces.Contains(scriptText, StringComparison.OrdinalIgnoreCase))
                                    {
                                        scriptFound = true;
                                        break;
                                    }
                                }
                            }

                            hasFilter = scriptFound;
                        }
                    }
                    // Check for actor type
                    else if (trimmedFilter.Contains("a:", StringComparison.OrdinalIgnoreCase))
                    {
                        if (trimmedFilter.Equals("a:", StringComparison.OrdinalIgnoreCase))
                        {
                            if (actor != null)
                                hasFilter = true;
                        }
                        else
                        {
                            if (actor != null)
                            {
                                var actorTypeText = trimmedFilter.Replace("a:", "", StringComparison.OrdinalIgnoreCase).Trim();
                                var name = TypeUtils.GetTypeDisplayName(actor.GetType());
                                var nameNoSpaces = name.Replace(" ", "");
                                if (name.Contains(actorTypeText, StringComparison.OrdinalIgnoreCase) || nameNoSpaces.Contains(actorTypeText, StringComparison.OrdinalIgnoreCase))
                                    hasFilter = true;
                            }
                        }
                    }
                    // Check for control type
                    else if (trimmedFilter.Contains("c:", StringComparison.OrdinalIgnoreCase))
                    {
                        if (trimmedFilter.Equals("c:", StringComparison.OrdinalIgnoreCase))
                        {
                            if (actor != null)
                                hasFilter = true;
                        }
                        else
                        {
                            if (actor is UIControl uiControl && uiControl.Control != null)
                            {
                                var controlTypeText = trimmedFilter.Replace("c:", "", StringComparison.OrdinalIgnoreCase).Trim();
                                var name = TypeUtils.GetTypeDisplayName(uiControl.Control.GetType());
                                var nameNoSpaces = name.Replace(" ", "");
                                if (name.Contains(controlTypeText, StringComparison.OrdinalIgnoreCase) || nameNoSpaces.Contains(controlTypeText, StringComparison.OrdinalIgnoreCase))
                                    hasFilter = true;
                            }
                        }
                    }
                    // Match text
                    else
                    {
                        var text = Text;
                        if (QueryFilterHelper.Match(trimmedFilter, text, out QueryFilterHelper.Range[] ranges))
                        {
                            // Update highlights
                            if (_highlights == null)
                                _highlights = new List<Rectangle>(ranges.Length);
                            else
                                _highlights.Clear();
                            var font = Style.Current.FontSmall;
                            var textRect = TextRect;
                            for (int i = 0; i < ranges.Length; i++)
                            {
                                var range = ranges[i];
                                var start = font.GetCharPosition(text, range.StartIndex);
                                var end = font.GetCharPosition(text, range.EndIndex);
                                _highlights.Add(new Rectangle(start.X + textRect.X, textRect.Y, end.X - start.X, textRect.Height));
                            }
                            _highlightsTextLeftOffset = HeaderTextLeftOffset;
                            hasFilter = true;
                        }
                    }
                    
                    if (!hasFilter)
                    {
                        hasAllFilters = false;
                        break;
                    }
                }

                isThisVisible = hasAllFilters;
                if (!hasAllFilters)
                    _highlights?.Clear();
            }
            else if (QueryFilterHelper.Match(filterText, Text, out QueryFilterHelper.Range[] ranges))
            {
                // Update highlights
                if (_highlights == null)
                    _highlights = new List<Rectangle>(ranges.Length);
                else
                    _highlights.Clear();
                var font = Style.Current.FontSmall;
                var textRect = TextRect;
                var text = Text;
                for (int i = 0; i < ranges.Length; i++)
                {
                    var range = ranges[i];
                    var start = font.GetCharPosition(text, range.StartIndex);
                    var end = font.GetCharPosition(text, range.EndIndex);
                    _highlights.Add(new Rectangle(start.X + textRect.X, textRect.Y, end.X - start.X, textRect.Height));
                }
                _highlightsTextLeftOffset = HeaderTextLeftOffset;
                isThisVisible = true;
            }
            else
            {
                // Hide
                _highlights?.Clear();
                isThisVisible = false;
            }

            // Update children
            bool isAnyChildVisible = false;
            for (int i = 0; i < _children.Count; i++)
            {
                if (_children[i] is ActorTreeNode child)
                {
                    child.UpdateFilter(filterText);
                    isAnyChildVisible |= child.Visible;
                }
            }

            bool isExpanded = isAnyChildVisible;

            // Restore cached state on query filter clear
            if (noFilter && actor != null)
            {
                // Pick the correct id when inside a prefab window.
                var id = actor.HasPrefabLink && actor.Scene == null ? actor.PrefabObjectID : actor.ID;
                isExpanded = Editor.Instance.ProjectCache.IsExpandedActor(ref id);
            }

            if (!noFilter)
            {
                if (isExpanded)
                    Expand(true);
                else
                    Collapse(true);
            }

            Visible = isThisVisible | isAnyChildVisible;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            // Update hidden state
            var actor = Actor;
            if (actor && !_hasSearchFilter)
            {
                Visible = (actor.HideFlags & HideFlags.HideInHierarchy) == 0;
            }

            base.Update(deltaTime);
        }


        /// <inheritdoc />
        protected override bool ShowTooltip => true;

        private void RestartTooltip()
        {
            var tooltip = Tooltip;
            tooltip?.OnMouseLeaveControl(this);
            tooltip?.OnMouseEnterControl(this);
        }

        /// <inheritdoc />
        public override bool OnTestTooltipOverControl(ref Float2 location)
        {
            if (TryGetSceneIconTooltip(ref location, out var tooltip, out var area))
            {
                if (Tooltip?.Visible == true && !string.Equals(_shownSceneIconTooltip, tooltip, StringComparison.Ordinal))
                {
                    _sceneIconTooltip = tooltip;
                    _sceneIconTooltipArea = area;
                    RestartTooltip();
                }

                _sceneIconTooltip = tooltip;
                _sceneIconTooltipArea = area;
                return true;
            }

            if (Tooltip?.Visible == true && !string.IsNullOrEmpty(_shownSceneIconTooltip))
            {
                _sceneIconTooltip = null;
                _sceneIconTooltipArea = Rectangle.Empty;
                _shownSceneIconTooltip = null;
                if (base.OnTestTooltipOverControl(ref location))
                {
                    RestartTooltip();
                    return true;
                }
            }

            _sceneIconTooltip = null;
            return base.OnTestTooltipOverControl(ref location);
        }

        /// <inheritdoc />
        public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
        {
            var mouseLocation = PointFromScreen(Input.MouseScreenPosition);
            if (TryGetSceneIconTooltip(ref mouseLocation, out text, out area))
            {
                _shownSceneIconTooltip = text;
                _sceneIconTooltip = text;
                _sceneIconTooltipArea = area;
                location = _sceneIconTooltipArea.Location + _sceneIconTooltipArea.Size * new Float2(0.5f, 1.0f);
                return true;
            }

            _shownSceneIconTooltip = null;
            _sceneIconTooltip = null;

            // Evaluate tooltip text once it's actually needed
            var actor = _actorNode.Actor;
            if (string.IsNullOrEmpty(TooltipText) && actor)
                TooltipText = GetActorTooltipText(actor);

            return base.OnShowTooltip(out text, out location, out area);
        }

        /// <inheritdoc />
        protected override Color CacheTextColor()
        {
            // Update node text color (based on ActorNode.IsActiveInHierarchy but with optimized logic a little)
            if (Parent is ActorTreeNode)
            {
                Color color = Style.Current.Foreground;
                var actor = Actor;
                if (actor)
                {
                    if (actor.HasPrefabLink)
                    {
                        // Prefab
                        color = Style.Current.BorderSelected;
                    }

                    if (!actor.IsActiveInHierarchy)
                    {
                        // Inactive
                        return Style.Current.ForegroundGrey;
                    }

                    if (actor.HasScene && Editor.Instance.StateMachine.IsPlayMode && actor.IsStatic)
                    {
                        // Static
                        return color * 0.85f;
                    }
                }

                // Default
                return color;
            }

            return base.CacheTextColor();
        }

        private static void LoadSceneTypeIcons()
        {
            if (_sceneTypeIconsLoaded)
                return;

            _sceneTypeIconsLoaded = true;
            _iconPointLight = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/PointLight");
            _iconDirectionalLight = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/DirectionalLight");
            _iconEnvironmentProbe = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/EnvironmentProbe");
            _iconSkybox = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/Skybox");
            _iconSkyLight = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/SkyLight");
            _iconAudioListener = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/AudioListner");
            _iconAudioSource = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/AudioSource");
            _iconDecal = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/Decal");
            _iconParticleEffect = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/ParticleEffect");
            _iconSceneAnimationPlayer = FlaxEngine.Content.LoadAsyncInternal<Texture>("Editor/Icons/Textures/SceneAnimationPlayer");
        }

        private static string GetSceneObjectTypeName(object obj)
        {
            if (obj == null)
                return "Null";

            var type = TypeUtils.GetObjectType(obj);
            if (type)
                return type.Name;
            return obj.GetType().GetTypeDisplayName();
        }

        private static string GetPrefabInstanceTypeTooltip(Actor actor)
        {
            return "Type: Prefab Instance\nPrefab Instance Type: " + GetSceneObjectTypeName(actor);
        }

        private static string GetActorTooltipText(Actor actor)
        {
            var description = Surface.SurfaceUtils.GetVisualScriptTypeDescription(TypeUtils.GetObjectType(actor));
            if (!actor.HasPrefabLink)
                return description;

            var prefabTooltip = GetPrefabInstanceTypeTooltip(actor);
            return string.IsNullOrEmpty(description) ? prefabTooltip : prefabTooltip + "\n\n" + description;
        }

        private static bool IsSimpleActor(Actor actor)
        {
            if (!actor)
                return true;

            var type = TypeUtils.GetObjectType(actor);
            if (type)
                return type.TypeName == typeof(Actor).FullName || type.TypeName == typeof(EmptyActor).FullName;
            var managedType = actor.GetType();
            return managedType == typeof(Actor) || managedType == typeof(EmptyActor);
        }

        private static Color GetHashedTypeColor(string typeName)
        {
            typeName ??= "Actor";
            unchecked
            {
                int hash = 23;
                for (int i = 0; i < typeName.Length; i++)
                    hash = hash * 31 + typeName[i];
                var hue = Math.Abs(hash % 360);
                return Color.FromHSV(hue, 0.58f, 0.95f);
            }
        }

        private static Color GetActorTypeColor(Actor actor)
        {
            if (actor is Scene)
                return new Color(0.28f, 0.64f, 1.0f, 1.0f);
            if (actor is Light)
                return new Color(1.0f, 0.82f, 0.24f, 1.0f);
            if (actor is AudioSource || actor is AudioListener)
                return new Color(0.73f, 0.54f, 1.0f, 1.0f);
            if (actor is Collider || actor is RigidBody || actor is Joint || actor is Cloth)
                return new Color(0.48f, 0.84f, 0.45f, 1.0f);
            if (actor is UICanvas || actor is UIControl)
                return new Color(1.0f, 0.50f, 0.77f, 1.0f);
            if (actor is NavMesh || actor is NavLink || actor is NavMeshBoundsVolume || actor is NavModifierVolume)
                return new Color(0.20f, 0.86f, 0.72f, 1.0f);
            if (actor is AnimatedModel || actor is BoneSocket || actor is SceneAnimationPlayer)
                return new Color(0.96f, 0.58f, 0.27f, 1.0f);
            if (actor is StaticModel || actor is Terrain || actor is Foliage || actor is Decal || actor is ParticleEffect || actor is SpriteRender || actor is TextRender)
                return new Color(0.32f, 0.75f, 1.0f, 1.0f);

            var type = TypeUtils.GetObjectType(actor);
            return GetHashedTypeColor(type ? type.TypeName : actor.GetType().FullName);
        }

        private static bool TryGetActorTypeTexture(Actor actor, out Texture texture)
        {
            LoadSceneTypeIcons();

            if (actor is PointLight || actor is SpotLight)
                texture = _iconPointLight;
            else if (actor is DirectionalLight)
                texture = _iconDirectionalLight;
            else if (actor is EnvironmentProbe)
                texture = _iconEnvironmentProbe;
            else if (actor is SkyLight)
                texture = _iconSkyLight;
            else if (actor is Skybox || actor is Sky || actor is ExponentialHeightFog)
                texture = _iconSkybox;
            else if (actor is AudioListener)
                texture = _iconAudioListener;
            else if (actor is AudioSource)
                texture = _iconAudioSource;
            else if (actor is Decal)
                texture = _iconDecal;
            else if (actor is ParticleEffect)
                texture = _iconParticleEffect;
            else if (actor is SceneAnimationPlayer || actor is VideoPlayer)
                texture = _iconSceneAnimationPlayer;
            else
                texture = null;

            return texture != null;
        }

        private static bool TryGetActorTypeSprite(Actor actor, out SpriteHandle sprite)
        {
            var icons = Editor.Instance.Icons;
            if (actor is Scene)
                sprite = icons.Globe32;
            else if (actor is Camera)
                sprite = icons.CameraFill32;
            else if (actor is BoneSocket)
                sprite = icons.Bone32;
            else
                sprite = icons.VisjectBoxClosed32;

            return sprite.IsValid;
        }

        private static bool TryGetActorTypeIcon(Actor actor, out SceneRowIcon icon)
        {
            icon = new SceneRowIcon();
            if (!actor)
                return false;

            var isPrefabInstance = actor.HasPrefabLink;
            icon.Color = isPrefabInstance ? Style.Current.BorderSelected : GetActorTypeColor(actor);
            icon.Tooltip = isPrefabInstance ? GetPrefabInstanceTypeTooltip(actor) : "Type: " + GetSceneObjectTypeName(actor);
            if (isPrefabInstance && !TryGetActorTypeTexture(actor, out icon.Texture))
            {
                icon.UseGlyph = true;
                icon.Glyph = SemanticIcons.Glyph.Prefab;
                return true;
            }
            if (actor is Collider)
            {
                icon.UseGlyph = true;
                icon.Glyph = SemanticIcons.Glyph.Collider;
                return true;
            }
            if (IsSimpleActor(actor))
            {
                icon.UseGlyph = true;
                icon.Glyph = SemanticIcons.Glyph.Model;
                icon.Color = Style.Current.ForegroundGrey;
                return true;
            }
            if (!TryGetActorTypeTexture(actor, out icon.Texture))
                TryGetActorTypeSprite(actor, out icon.Sprite);
            return icon.IsValid;
        }

        private static SceneRowIcon GetScriptIcon(Script script)
        {
            return new SceneRowIcon
            {
                Sprite = Editor.Instance.Icons.CSharpScript128,
                Color = Color.White,
                Tooltip = "Script: " + GetSceneObjectTypeName(script),
            };
        }

        private static void DrawSceneRowIcon(ref SceneRowIcon icon, Rectangle rect, bool active)
        {
            var color = active ? icon.Color : icon.Color.AlphaMultiplied(0.45f);
            if (icon.Texture != null)
            {
                Render2D.DrawTexture(icon.Texture, rect, active ? Color.White : Color.White.AlphaMultiplied(0.45f));
                Render2D.FillRectangle(new Rectangle(rect.X, rect.Bottom - 2.0f, rect.Width, 2.0f), color);
            }
            else if (icon.UseGlyph)
            {
                SemanticIcons.Draw(icon.Glyph, rect, color);
            }
            else if (icon.Sprite.IsValid)
            {
                Render2D.DrawSprite(icon.Sprite, rect, color);
            }
            else
            {
                Render2D.FillRectangle(rect, color);
            }
        }

        private bool TryGetSceneRowIcon(Actor actor, int iconIndex, out SceneRowIcon icon)
        {
            for (int i = 0; i < actor.ScriptsCount; i++)
            {
                var script = actor.GetScript(i);
                if (!script)
                    continue;
                if (iconIndex == 0)
                {
                    icon = GetScriptIcon(script);
                    return icon.IsValid;
                }
                iconIndex--;
            }

            icon = new SceneRowIcon();
            return false;
        }

        private int GetSceneRowIconsCount(Actor actor)
        {
            if (!actor)
                return 0;

            int result = 0;
            for (int i = 0; i < actor.ScriptsCount; i++)
            {
                if (actor.GetScript(i))
                    result++;
            }
            return result;
        }

        private Rectangle GetSceneIconRect(float startX, int iconIndex)
        {
            return new Rectangle(startX + iconIndex * (SceneIconSize + SceneIconSpacing), (HeaderHeight - SceneIconSize) * 0.5f, SceneIconSize, SceneIconSize);
        }

        private float GetSceneIconsRightEdge()
        {
            var rightEdge = Width;
            var originInParent = Float2.Zero;
            for (Control control = this; control.Parent != null; control = control.Parent)
            {
                originInParent = control.PointToParent(ref originInParent);
                if (control.IsScrollable && control.Parent is ScrollableControl scrollableParent)
                    originInParent += scrollableParent.ViewOffset;

                if (control.Parent is Panel panel)
                {
                    panel.GetDesireClientArea(out var clientArea);
                    rightEdge = Mathf.Min(rightEdge, clientArea.Right - originInParent.X);
                    break;
                }
            }
            return Mathf.Max(0.0f, rightEdge);
        }

        private bool TryGetSceneIconsStart(out float startX, out int iconsCount)
        {
            startX = 0.0f;
            iconsCount = GetSceneRowIconsCount(Actor);
            if (iconsCount == 0)
                return false;

            var font = TextFont.GetFont();
            if (!font)
                return false;

            var textRect = TextRect;
            var textWidth = font.MeasureText(Text ?? string.Empty).X;
            var iconsWidth = iconsCount * SceneIconSize + (iconsCount - 1) * SceneIconSpacing;
            startX = GetSceneIconsRightEdge() - SceneIconRightMargin - iconsWidth;
            return startX > textRect.Left && textRect.Left + textWidth + SceneIconTextPadding <= startX;
        }

        private void DrawSceneRowIcons()
        {
            var actor = Actor;
            if (!actor || !TryGetSceneIconsStart(out var startX, out var iconsCount))
                return;

            bool active = actor.IsActiveInHierarchy;
            for (int i = 0; i < iconsCount; i++)
            {
                if (TryGetSceneRowIcon(actor, i, out var icon))
                    DrawSceneRowIcon(ref icon, GetSceneIconRect(startX, i), active);
            }
        }

        private Rectangle GetLeadingActorIconRect()
        {
            var textRect = TextRect;
            return new Rectangle(textRect.Left - HeaderTextLeftOffset, (HeaderHeight - SceneIconSize) * 0.5f, SceneIconSize, SceneIconSize);
        }

        private void DrawLeadingActorIcon()
        {
            var actor = Actor;
            if (!actor || !TryGetActorTypeIcon(actor, out var icon))
                return;
            DrawSceneRowIcon(ref icon, GetLeadingActorIconRect(), actor.IsActiveInHierarchy);
        }

        private Rectangle GetActiveCheckboxRect()
        {
            return new Rectangle(2.0f, (HeaderHeight - ActiveCheckboxSize) * 0.5f, ActiveCheckboxSize, ActiveCheckboxSize);
        }

        private bool TestActiveCheckboxHit(ref Float2 location)
        {
            var actor = Actor;
            if (!actor || !HeaderRect.Contains(ref location))
                return false;

            var rect = GetActiveCheckboxRect();
            return rect.Contains(ref location);
        }

        private bool CanToggleActorActive()
        {
            var actor = Actor;
            if (!actor)
                return false;

            var state = Editor.Instance.StateMachine.CurrentState;
            return actor.HasScene ? state.CanEditScene && Level.IsAnySceneLoaded : state.CanEditContent;
        }

        private void ToggleActorActive()
        {
            var actor = Actor;
            if (!actor || !CanToggleActorActive())
                return;

            using (new UndoBlock(ActorNode.Root.Undo, actor, actor.IsActive ? "Deactivate Actor" : "Activate Actor"))
                actor.IsActive = !actor.IsActive;
        }

        private void DrawActiveCheckbox()
        {
            var actor = Actor;
            if (!actor || !ShouldDrawActiveCheckbox)
                return;

            var style = Style.Current;
            var rect = GetActiveCheckboxRect();
            var mouseLocation = PointFromScreen(Input.MouseScreenPosition);
            var enabled = CanToggleActorActive();
            var isMouseOverCheckbox = rect.Contains(ref mouseLocation);
            var highlighted = enabled && (_activeCheckboxPressed || isMouseOverCheckbox);
            var fillColor = actor.IsActive
                ? (highlighted ? Color.Lerp(style.BorderSelected, Color.White, 0.28f) : style.BorderSelected)
                : Color.Lerp(style.Background, style.Foreground, highlighted ? 0.30f : 0.14f);
            if (!enabled)
                fillColor = Color.Lerp(fillColor, style.Background, 0.45f);
            StyleRendering.FillCheckBox(rect, fillColor);

            if (actor.IsActive && style.CheckBoxTick.IsValid)
                Render2D.DrawSprite(style.CheckBoxTick, rect, enabled ? Color.White : style.ForegroundDisabled);
        }

        private bool TryGetSceneIconTooltip(ref Float2 location, out string tooltip, out Rectangle area)
        {
            tooltip = null;
            area = Rectangle.Empty;

            var actor = Actor;
            if (!actor)
                return false;

            var leadingRect = GetLeadingActorIconRect();
            if (leadingRect.Contains(ref location) && TryGetActorTypeIcon(actor, out var leadingIcon))
            {
                tooltip = leadingIcon.Tooltip;
                area = leadingRect;
                return !string.IsNullOrEmpty(tooltip);
            }

            if (!TryGetSceneIconsStart(out var startX, out var iconsCount))
                return false;

            for (int i = 0; i < iconsCount; i++)
            {
                var rect = GetSceneIconRect(startX, i);
                if (!rect.Contains(ref location))
                    continue;
                if (!TryGetSceneRowIcon(actor, i, out var icon))
                    return false;

                tooltip = icon.Tooltip;
                area = rect;
                return !string.IsNullOrEmpty(tooltip);
            }

            return false;
        }

        /// <inheritdoc />
        public override int Compare(Control other)
        {
            if (other is ActorTreeNode node)
            {
                return _orderInParent - node._orderInParent;
            }
            return base.Compare(other);
        }

        /// <summary>
        /// Starts the actor renaming action.
        /// </summary>
        public void StartRenaming(EditorWindow window = null, Panel treePanel = null)
        {
            // Block renaming during scripts reload
            if (Editor.Instance.ProgressReporting.CompileScripts.IsActive)
                return;

            Select();

            // Disable scrolling of view
            if (window is SceneTreeWindow)
                (window as SceneTreeWindow).ScrollingOnSceneTreeView(false);
            else if (window is PrefabWindow)
                (window as PrefabWindow).ScrollingOnTreeView(false);

            // Start renaming the actor
            var rect = TextRect;
            if (treePanel != null)
            {
                treePanel.ScrollViewTo(this, true);
                rect.Size = new Float2(treePanel.Width - TextRect.Location.X, TextRect.Height);
            }
            var dialog = RenamePopup.Show(this, rect, _actorNode.Name, false);
            dialog.Renamed += OnRenamed;
            dialog.Closed += popup =>
            {
                // Enable scrolling of view
                if (window is SceneTreeWindow)
                    (window as SceneTreeWindow).ScrollingOnSceneTreeView(true);
                else if (window is PrefabWindow)
                    (window as PrefabWindow).ScrollingOnTreeView(true);
            };
        }

        private void OnRenamed(RenamePopup renamePopup)
        {
            using (new UndoBlock(ActorNode.Root.Undo, Actor, "Rename"))
                Actor.Name = renamePopup.Text.Trim();
        }

        /// <inheritdoc />
        protected override void OnExpandedChanged()
        {
            base.OnExpandedChanged();
            var actor = Actor;

            if (!IsLayoutLocked && actor)
            {
                // Pick the correct id when inside a prefab window.
                var id = actor.HasPrefabLink && !actor.HasScene ? actor.PrefabObjectID : actor.ID;
                Editor.Instance.ProjectCache.SetExpandedActor(ref id, IsExpanded);
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();
            if (!ShowHeader)
                return;

            // Draw all highlights
            if (_highlights != null)
            {
                var style = Style.Current;
                var color = style.ProgressNormal * 0.6f;
                var textOffset = HeaderTextLeftOffset - _highlightsTextLeftOffset;
                for (int i = 0; i < _highlights.Count; i++)
                {
                    var rect = _highlights[i];
                    rect.X += textOffset;
                    Render2D.FillRectangle(rect, color);
                }
            }

            DrawActiveCheckbox();
            DrawLeadingActorIcon();
            DrawSceneRowIcons();
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && TestActiveCheckboxHit(ref location))
            {
                _activeCheckboxPressed = true;
                Focus();
                return true;
            }

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && _activeCheckboxPressed)
            {
                _activeCheckboxPressed = false;
                if (TestActiveCheckboxHit(ref location))
                    ToggleActorActive();
                Focus();
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && TestActiveCheckboxHit(ref location))
                return true;

            return base.OnMouseDoubleClick(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            _activeCheckboxPressed = false;
            base.OnMouseLeave();
        }

        /// <inheritdoc />
        protected override bool OnMouseDoubleClickHeader(ref Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left)
            {
                var sceneContext = this.GetSceneContext();
                switch (Editor.Instance.Options.Options.Input.DoubleClickSceneNode)
                {
                case SceneNodeDoubleClick.RenameActor:
                    sceneContext.RenameSelection();
                    return true;
                case SceneNodeDoubleClick.FocusActor:
                    sceneContext.FocusSelection();
                    return true;
                case SceneNodeDoubleClick.OpenPrefab:
                    Editor.Instance.Prefabs.OpenPrefab(ActorNode);
                    return true;
                case SceneNodeDoubleClick.Expand:
                default: break;
                }
            }
            return base.OnMouseDoubleClickHeader(ref location, button);
        }

        /// <inheritdoc />
        protected override DragDropEffect OnDragEnterHeader(DragData data)
        {
            // Check if cannot edit scene or there is no scene loaded (handle case for actors in prefab editor)
            if (_actorNode?.ParentScene != null)
            {
                if (!Editor.Instance.StateMachine.CurrentState.CanEditScene || !Level.IsAnySceneLoaded)
                    return DragDropEffect.None;
            }
            else
            {
                if (!Editor.Instance.StateMachine.CurrentState.CanEditContent)
                    return DragDropEffect.None;
            }

            if (_dragHandlers == null)
                _dragHandlers = new DragHandlers();

            // Check if drop actors
            if (_dragActors == null)
            {
                _dragActors = new DragActors(ValidateDragActor);
                _dragHandlers.Add(_dragActors);
            }
            if (_dragActors.OnDragEnter(data))
                return _dragActors.Effect;

            // Check if drop scripts
            if (_dragScripts == null)
            {
                _dragScripts = new DragScripts(ValidateDragScript);
                _dragHandlers.Add(_dragScripts);
            }
            if (_dragScripts.OnDragEnter(data))
                return _dragScripts.Effect;

            // Check if drag assets
            if (_dragAssets == null)
            {
                _dragAssets = new DragAssets(ValidateDragAsset);
                _dragHandlers.Add(_dragAssets);
            }
            if (_dragAssets.OnDragEnter(data))
                return _dragAssets.Effect;

            // Check if drag actor type
            if (_dragActorType == null)
            {
                _dragActorType = new DragActorType(ValidateDragActorType);
                _dragHandlers.Add(_dragActorType);
            }
            if (_dragActorType.OnDragEnter(data))
                return _dragActorType.Effect;

            // Check if drag control type
            if (_dragControlType == null)
            {
                _dragControlType = new DragControlType(ValidateDragControlType);
                _dragHandlers.Add(_dragControlType);
            }
            if (_dragControlType.OnDragEnter(data))
                return _dragControlType.Effect;

            // Check if drag script item
            if (_dragScriptItems == null)
            {
                _dragScriptItems = new DragScriptItems(ValidateDragScriptItem);
                _dragHandlers.Add(_dragScriptItems);
            }
            if (_dragScriptItems.OnDragEnter(data))
                return _dragScriptItems.Effect;

            return DragDropEffect.None;
        }

        /// <inheritdoc />
        protected override DragDropEffect OnDragMoveHeader(DragData data)
        {
            return _dragHandlers.Effect;
        }

        /// <inheritdoc />
        protected override void OnDragLeaveHeader()
        {
            _dragHandlers.OnDragLeave();
        }

        /// <inheritdoc />
        protected override DragDropEffect OnDragDropHeader(DragData data)
        {
            var result = DragDropEffect.None;

            Actor myActor = Actor;
            Actor newParent;
            int newOrder = -1;

            // Check if has no actor (only for Root Actor)
            if (myActor == null)
            {
                // Append to the last scene
                var scenes = Level.Scenes;
                if (scenes == null || scenes.Length == 0)
                    throw new InvalidOperationException("No scene loaded.");
                newParent = scenes[scenes.Length - 1];
            }
            else
            {
                newParent = myActor;

                // Use drag positioning to change target parent and index
                if (DragOverMode == DragItemPositioning.Above)
                {
                    if (myActor.HasParent)
                    {
                        newParent = myActor.Parent;
                        newOrder = myActor.OrderInParent;
                    }
                }
                else if (DragOverMode == DragItemPositioning.Below)
                {
                    if (myActor.HasParent)
                    {
                        newParent = myActor.Parent;
                        newOrder = myActor.OrderInParent + 1;
                    }
                }
            }
            if (newParent == null)
                throw new InvalidOperationException("Missing parent actor.");

            // Drag actors
            if (_dragActors != null && _dragActors.HasValidDrag)
            {
                bool worldPositionsStays = Root.GetKey(KeyboardKeys.Control) == false;
                var objects = new SceneObject[_dragActors.Objects.Count];
                var treeNodes = new TreeNode[_dragActors.Objects.Count];
                for (int i = 0; i < objects.Length; i++)
                {
                    objects[i] = _dragActors.Objects[i].Actor;
                    treeNodes[i] = _dragActors.Objects[i].TreeNode;
                }
                var action = new ParentActorsAction(objects, newParent, newOrder, worldPositionsStays);
                ActorNode.Root.Undo?.AddAction(action);
                action.Do();
                ParentTree.Focus();
                ParentTree.Select(treeNodes.ToList());
                result = DragDropEffect.Move;
            }
            // Drag scripts
            else if (_dragScripts != null && _dragScripts.HasValidDrag)
            {
                var objects = new SceneObject[_dragScripts.Objects.Count];
                for (int i = 0; i < objects.Length; i++)
                    objects[i] = _dragScripts.Objects[i];
                var action = new ParentActorsAction(objects, newParent, newOrder);
                ActorNode.Root.Undo?.AddAction(action);
                action.Do();
                Select();
                result = DragDropEffect.Move;
            }
            // Drag assets
            else if (_dragAssets != null && _dragAssets.HasValidDrag)
            {
                var spawnParent = myActor;
                if (DragOverMode == DragItemPositioning.Above || DragOverMode == DragItemPositioning.Below)
                    spawnParent = newParent;

                for (int i = 0; i < _dragAssets.Objects.Count; i++)
                {
                    var item = _dragAssets.Objects[i];
                    var actor = item.OnEditorDrop(this);
                    if (spawnParent.GetType() != typeof(Scene))
                    {
                        // Set all Actors static flags to match parents
                        List<Actor> childActors = new List<Actor>();
                        Utilities.Utils.GetActorsTree(childActors, actor);
                        foreach (var child in childActors)
                        {
                            child.StaticFlags = spawnParent.StaticFlags;
                        }
                    }
                    actor.Name = item.ShortName;
                    if (_dragAssets.Objects[i] is not PrefabItem)
                        actor.Transform = Transform.Identity;
                    var previousTrans = actor.Transform;
                    ActorNode.Root.Spawn(actor, spawnParent, newOrder);
                    actor.LocalTransform = previousTrans;
                }
                result = DragDropEffect.Move;
            }
            // Drag actor type
            else if (_dragActorType != null && _dragActorType.HasValidDrag)
            {
                for (int i = 0; i < _dragActorType.Objects.Count; i++)
                {
                    var item = _dragActorType.Objects[i];
                    var actor = item.CreateInstance() as Actor;
                    if (actor == null)
                    {
                        Editor.LogWarning("Failed to spawn actor of type " + item.TypeName);
                        continue;
                    }
                    actor.StaticFlags = newParent.StaticFlags;
                    actor.Name = item.Name;
                    ActorNode.Root.Spawn(actor, newParent, newOrder);
                }
                result = DragDropEffect.Move;
            }
            // Drag control type
            else if (_dragControlType != null && _dragControlType.HasValidDrag)
            {
                for (int i = 0; i < _dragControlType.Objects.Count; i++)
                {
                    var item = _dragControlType.Objects[i];
                    var control = item.CreateInstance() as Control;
                    if (control == null)
                    {
                        Editor.LogWarning("Failed to spawn UIControl with control type " + item.TypeName);
                        continue;
                    }
                    var uiControl = new UIControl
                    {
                        Control = control,
                        StaticFlags = newParent.StaticFlags,
                        Name = item.Name,
                    };
                    ActorNode.Root.Spawn(uiControl, newParent, newOrder);
                }
                result = DragDropEffect.Move;
            }
            // Drag script item
            else if (_dragScriptItems != null && _dragScriptItems.HasValidDrag)
            {
                var spawnParent = myActor;
                if (DragOverMode == DragItemPositioning.Above || DragOverMode == DragItemPositioning.Below)
                    spawnParent = newParent;

                for (int i = 0; i < _dragScriptItems.Objects.Count; i++)
                {
                    var item = _dragScriptItems.Objects[i];
                    var actorType = Editor.Instance.CodeEditing.Actors.Get(item);
                    var scriptType = Editor.Instance.CodeEditing.Scripts.Get(item);
                    if (actorType != ScriptType.Null)
                    {
                        var actor = actorType.CreateInstance() as Actor;
                        if (actor == null)
                        {
                            Editor.LogWarning("Failed to spawn actor of type " + actorType.TypeName);
                            continue;
                        }
                        actor.StaticFlags = spawnParent.StaticFlags;
                        actor.Name = actorType.Name;
                        actor.Transform = spawnParent.Transform;
                        ActorNode.Root.Spawn(actor, spawnParent, newOrder);
                    }
                    else if (scriptType != ScriptType.Null)
                    {
                        if (DragOverMode == DragItemPositioning.Above || DragOverMode == DragItemPositioning.Below)
                        {
                            Editor.LogWarning("Failed to spawn script of type " + actorType.TypeName);
                            continue;
                        }
                        IUndoAction action = new AddRemoveScript(true, newParent, scriptType);
                        Select();
                        ActorNode.Root.Undo?.AddAction(action);
                        action.Do();
                    }
                }
                result = DragDropEffect.Move;
            }

            // Clear cache
            _dragHandlers.OnDragDrop(null);

            // Check if scene has been modified
            if (result != DragDropEffect.None)
            {
                var node = SceneGraphFactory.FindNode(newParent.ID) as ActorNode;
                node?.TreeNode.Expand();
            }

            return result;
        }

        private bool ValidateDragActor(ActorNode actorNode)
        {
            // Reject dragging actors not linked to scene (eg. from prefab) or in the opposite way
            var thisHasScene = ActorNode.ParentScene != null;
            var otherHasScene = actorNode.ParentScene != null;
            if (thisHasScene != otherHasScene)
                return false;

            // Reject dragging actors between prefab windows (different roots)
            if (!thisHasScene && ActorNode.Root != actorNode.Root)
                return false;

            // Reject dragging parents and itself
            return actorNode.Actor != null && actorNode != ActorNode && actorNode.Find(Actor) == null;
        }

        private bool ValidateDragScript(Script script)
        {
            // Reject dragging scripts not linked to scene (eg. from prefab) or in the opposite way
            var thisHasScene = Actor.HasScene;
            var otherHasScene = script.HasScene;
            if (thisHasScene != otherHasScene)
                return false;

            // Reject dragging parents and itself
            return script.Actor != null && script.Parent != Actor;
        }

        private bool ValidateDragAsset(AssetItem assetItem)
        {
            return assetItem.OnEditorDrag(this);
        }

        private static bool ValidateDragActorType(ScriptType actorType)
        {
            return Editor.Instance.CodeEditing.Actors.Get().Contains(actorType);
        }

        private static bool ValidateDragControlType(ScriptType controlType)
        {
            return Editor.Instance.CodeEditing.Controls.Get().Contains(controlType);
        }

        private bool ValidateDragScriptItem(ScriptItem script)
        {
            return Editor.Instance.CodeEditing.Actors.Get(script) != ScriptType.Null || Editor.Instance.CodeEditing.Scripts.Get(script) != ScriptType.Null;
        }

        /// <inheritdoc />
        protected override void DoDragDrop()
        {
            DragData data;
            var tree = ParentTree;

            // Check if this node is selected
            if (tree.Selection.Contains(this))
            {
                // Get selected actors
                var actors = new List<ActorNode>();
                for (var i = 0; i < tree.Selection.Count; i++)
                {
                    var e = tree.Selection[i];

                    // Skip if parent is already selected to keep correct parenting
                    if (tree.Selection.Contains(e.Parent))
                        continue;

                    if (e is ActorTreeNode node && node.ActorNode.CanDrag)
                        actors.Add(node.ActorNode);
                }
                data = DragActors.GetDragData(actors);
            }
            else
            {
                data = DragActors.GetDragData(ActorNode);
            }

            // Start drag operation
            DoDragDrop(data);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _dragActors = null;
            _dragScripts = null;
            _dragAssets = null;
            _dragActorType = null;
            _dragControlType = null;
            _dragScriptItems = null;
            _dragHandlers?.Clear();
            _dragHandlers = null;
            _highlights = null;

            base.OnDestroy();
        }
    }
}
