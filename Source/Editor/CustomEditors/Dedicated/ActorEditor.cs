// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Actions;
using FlaxEditor.CustomEditors.Editors;
using FlaxEditor.CustomEditors.Elements;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Tree;
using FlaxEditor.SceneGraph;
using FlaxEditor.Scripting;
using FlaxEditor.Windows.Assets;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Json;
using FlaxEngine.Utilities;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace FlaxEditor.CustomEditors.Dedicated
{
    /// <summary>
    /// Dedicated custom editor for <see cref="Actor"/> objects.
    /// </summary>
    /// <seealso cref="FlaxEditor.CustomEditors.Editors.GenericEditor" />
    [CustomEditor(typeof(Actor)), DefaultEditor]
    public class ActorEditor : ScriptingObjectEditor
    {
        private Guid _linkedPrefabId;
        private Guid _linkedPrefabObjectId;

        /// <inheritdoc />
        public override void Initialize(LayoutElementsContainer layout)
        {
            // Check for prefab link
            var actor = Values.Count == 1 ? Values[0] as Actor : null;
            Prefab sourcePrefab = null;
            if (actor != null && actor.HasPrefabLink)
            {
                // TODO: consider editing more than one instance of the same prefab asset at once

                var prefab = FlaxEngine.Content.Load<Prefab>(actor.PrefabID);
                if (prefab)
                {
                    var prefabObjectId = actor.PrefabObjectID;
                    Prefab inspectedPrefab = null;
                    if (Presenter.Owner is FlaxEditor.Windows.PropertiesWindow propertiesWindow)
                        propertiesWindow.TryGetInspectedPrefab(actor, out inspectedPrefab);
                    else if (Presenter.Owner is PrefabWindow prefabWindow && prefabWindow.Graph.MainActor == actor)
                        inspectedPrefab = prefabWindow.Asset;

                    Actor prefabInstance;
                    Guid referenceObjectId;
                    if (inspectedPrefab)
                    {
                        inspectedPrefab.GetNestedObject(ref prefabObjectId, out var sourcePrefabId, out referenceObjectId);
                        sourcePrefab = FlaxEngine.Content.Load<Prefab>(sourcePrefabId);
                        prefabInstance = sourcePrefab ? sourcePrefab.GetDefaultInstance(ref referenceObjectId) as Actor : null;
                    }
                    else
                    {
                        referenceObjectId = prefabObjectId;
                        prefabInstance = prefab.GetDefaultInstance(ref referenceObjectId) as Actor;
                        prefab.GetNestedObject(ref prefabObjectId, out var nestedPrefabId, out var nestedPrefabObjectId);
                        var nestedPrefab = FlaxEngine.Content.Load<Prefab>(nestedPrefabId);
                        sourcePrefab = nestedPrefab ? nestedPrefab : prefab;
                    }

                    if (prefabInstance != null)
                    {
                        Values.SetReferenceValue(prefabInstance);
                        _linkedPrefabId = inspectedPrefab ? sourcePrefab.ID : prefab.ID;
                        _linkedPrefabObjectId = referenceObjectId;
                        Editor.Instance.Prefabs.PrefabApplying += OnPrefabApplying;
                        Editor.Instance.Prefabs.PrefabApplied += OnPrefabApplied;
                    }
                }
            }

            base.Initialize(layout);

            // Add custom settings button to General group
            for (int i = 0; i < layout.Children.Count; i++)
            {
                if (layout.Children[i] is GroupElement group && group.Panel.HeaderText == "General")
                {
                    if (actor != null)
                    {
                        group.Panel.TooltipText = Surface.SurfaceUtils.GetVisualScriptTypeDescription(TypeUtils.GetObjectType(actor));
                    }
                    AddSourcePrefabReference(group, sourcePrefab);
                    var settingsButton = group.AddSettingsButton();
                    settingsButton.Clicked += OnSettingsButtonClicked;
                    break;
                }
            }

            AddScriptsEditor(layout);
        }

        private void AddScriptsEditor(LayoutElementsContainer layout)
        {
            var type = Values.Count > 0 && Values[0] != null ? TypeUtils.GetObjectType(Values[0]) : new ScriptType(typeof(Actor));
            var scriptsMember = type.GetProperty("Scripts");
            if (scriptsMember == ScriptMemberInfo.Null)
                scriptsMember = new ScriptType(typeof(Actor)).GetProperty("Scripts");
            if (scriptsMember == ScriptMemberInfo.Null)
                return;

            var item = new ItemInfo(scriptsMember)
            {
                CustomEditor = new CustomEditorAttribute(typeof(ScriptsEditor)),
                IsReadOnly = false,
            };
            layout.Object(item.GetValues(Values), new ScriptsEditor());
        }

        private void AddSourcePrefabReference(GroupElement group, Prefab sourcePrefab)
        {
            if (!sourcePrefab)
                return;

            const float buttonWidth = 70.0f;
            const float buttonHeight = 22.0f;
            const float spacing = 4.0f;
            var row = group.CustomContainer<ContainerControl>("Source Prefab", "The prefab asset this object inherits from.").CustomControl;
            var picker = new AssetPicker();
            picker.UseCompactField = true;
            picker.ShowCompactPreview = Editor.Instance.Options.Options.Interface.ShowReferencePreviewsInProperties;
            picker.CanEdit = false;
            picker.Validator.AssetType = new ScriptType(typeof(Prefab));
            picker.Validator.SelectedID = sourcePrefab.ID;
            row.Height = picker.CompactHeight;
            picker.AnchorPreset = AnchorPresets.StretchAll;
            picker.Offsets = new Margin(0.0f, buttonWidth + spacing, 0.0f, 0.0f);
            picker.Parent = row;

            var overridesButton = new Button
            {
                Text = "Overrides",
                TooltipText = "View and manage prefab overrides.",
                AnchorPreset = AnchorPresets.TopRight,
                Bounds = new Rectangle(-buttonWidth, (row.Height - buttonHeight) * 0.5f, buttonWidth, buttonHeight),
                Parent = row,
            };
            overridesButton.Clicked += () => ViewChanges(overridesButton, new Float2(0.0f, overridesButton.Height));
        }

        private void OnSettingsButtonClicked(Image image, MouseButton mouseButton)
        {
            if (mouseButton != MouseButton.Left)
                return;

            var cm = new ContextMenu();
            var actor = (Actor)Values[0];
            var scriptType = TypeUtils.GetType(actor.TypeName);
            var item = scriptType.ContentItem;
            if (Presenter.Owner != null)
            {
                var lockButton = cm.AddButton(Presenter.Owner.LockSelection ? "Unlock" : "Lock");
                lockButton.ButtonClicked += button =>
                {
                    var owner = Presenter?.Owner;
                    if (owner == null)
                        return;
                    owner.LockSelection = !owner.LockSelection;

                    // Reselect current selection
                    if (!owner.LockSelection && owner.Selection.Count > 0)
                    {
                        var cachedSelection = owner.Selection.ToList();
                        owner.Select(null);
                        owner.Select(cachedSelection);
                    }
                };
            }
            if (Presenter.Owner is FlaxEditor.Windows.PropertiesWindow propertiesWindow)
            {
                bool isPinned = propertiesWindow.IsSelectionPinned();
                var pinButton = cm.AddButton(isPinned ? "Unpin" : "Pin");
                pinButton.Enabled = isPinned || propertiesWindow.CanPinSelection();
                pinButton.ButtonClicked += button =>
                {
                    if (isPinned)
                        propertiesWindow.UnpinSelection();
                    else
                        propertiesWindow.PinSelection();
                };
            }
            cm.AddButton("Copy ID", OnClickCopyId);
            cm.AddButton("Edit actor type", OnClickEditActorType).Enabled = item != null;
            var showButton = cm.AddButton("Show in content window", OnClickShowActorType);
            showButton.Enabled = item != null;
            showButton.Icon = Editor.Instance.Icons.Search12;
            cm.Show(image, image.Size);
        }

        private void OnClickCopyId()
        {
            var actor = (Actor)Values[0];
            Clipboard.Text = JsonSerializer.GetStringID(actor.ID);
        }

        private void OnClickEditActorType()
        {
            var actor = (Actor)Values[0];
            var scriptType = TypeUtils.GetType(actor.TypeName);
            var item = scriptType.ContentItem;
            if (item != null)
                Editor.Instance.ContentEditing.Open(item);
        }

        private void OnClickShowActorType()
        {
            var actor = (Actor)Values[0];
            var scriptType = TypeUtils.GetType(actor.TypeName);
            var item = scriptType.ContentItem;
            if (item != null)
                Editor.Instance.Windows.ContentWin.Select(item);
        }

        /// <inheritdoc />
        protected override void Deinitialize()
        {
            base.Deinitialize();

            if (_linkedPrefabId != Guid.Empty)
            {
                _linkedPrefabId = Guid.Empty;
                _linkedPrefabObjectId = Guid.Empty;
                Editor.Instance.Prefabs.PrefabApplying -= OnPrefabApplying;
                Editor.Instance.Prefabs.PrefabApplied -= OnPrefabApplied;
            }
        }

        private void OnPrefabApplied(Prefab prefab, Actor instance)
        {
            if (prefab.ID == _linkedPrefabId)
            {
                // This works fine but in PrefabWindow when using live update it crashes on using color picker/float slider because UI is being rebuild
                //Presenter.BuildLayoutOnUpdate();

                // Better way is to just update the reference value using the new default instance of the prefab, created after changes apply
                if (Values != null && (Actor)Values[0] && prefab && !prefab.WaitForLoaded())
                {
                    var prefabObjectId = _linkedPrefabObjectId;
                    var prefabInstance = prefab.GetDefaultInstance(ref prefabObjectId);
                    if (prefabInstance != null)
                    {
                        Values.SetReferenceValue(prefabInstance);
                        RefreshReferenceValue();
                    }
                }
            }
        }

        private void OnPrefabApplying(Prefab prefab, Actor instance)
        {
            if (prefab.ID == _linkedPrefabId)
            {
                // Unlink reference value (it gets deleted by the prefabs system during apply)
                ClearReferenceValueAll();
            }
        }

        private TreeNode CreateDiffNode(CustomEditor editor)
        {
            var node = new TreeNode(false)
            {
                Tag = editor
            };

            // Removed Script
            if (editor is RemovedScriptDummy removed)
            {
                node.TextColor = Color.OrangeRed;
                node.Text = Utilities.Utils.GetPropertyNameUI(removed.PrefabObject.GetType().Name);
            }
            // Removed Actor
            else if (editor is RemovedActorDummy removedActor)
            {
                node.TextColor = Color.OrangeRed;
                node.Text = $"{removedActor.PrefabObject.Name} ({Utilities.Utils.GetPropertyNameUI(removedActor.PrefabObject.GetType().Name)})";
            }
            // Actor or Script
            else if (editor.Values[0] is SceneObject sceneObject)
            {
                node.TextColor = sceneObject.HasPrefabLink ? FlaxEngine.GUI.Style.Current.BorderSelected : FlaxEngine.GUI.Style.Current.BackgroundSelected;
                if (editor.Values.Info != ScriptMemberInfo.Null)
                {
                    if (editor.Values.GetAttributes().FirstOrDefault(x => x is EditorDisplayAttribute) is EditorDisplayAttribute editorDisplayAttribute && !string.IsNullOrEmpty(editorDisplayAttribute.Name))
                        node.Text = $"{Utilities.Utils.GetPropertyNameUI(editorDisplayAttribute.Name)} ({Utilities.Utils.GetPropertyNameUI(editor.Values.Info.Name)})";
                    else
                        node.Text = Utilities.Utils.GetPropertyNameUI(editor.Values.Info.Name);
                }
                else if (sceneObject is Actor actor)
                    node.Text = $"{actor.Name} ({Utilities.Utils.GetPropertyNameUI(sceneObject.GetType().Name)})";
                else
                    node.Text = Utilities.Utils.GetPropertyNameUI(sceneObject.GetType().Name);
            }
            // Array Item
            else if (editor.ParentEditor is CollectionEditor)
            {
                node.Text = "Element " + editor.ParentEditor.ChildrenEditors.IndexOf(editor);
            }
            // Common type
            else if (editor.Values.Info != ScriptMemberInfo.Null)
            {
                if (editor.Values.GetAttributes().FirstOrDefault(x => x is EditorDisplayAttribute) is EditorDisplayAttribute editorDisplayAttribute
                    && !string.IsNullOrEmpty(editorDisplayAttribute.Name)
                    && !editorDisplayAttribute.Name.Contains("_inline"))
                    node.Text = $"{Utilities.Utils.GetPropertyNameUI(editorDisplayAttribute.Name)} ({Utilities.Utils.GetPropertyNameUI(editor.Values.Info.Name)})";
                else
                    node.Text = Utilities.Utils.GetPropertyNameUI(editor.Values.Info.Name);
            }
            // Custom type
            else if (editor.Values[0] != null)
            {
                node.Text = editor.Values[0].ToString();
            }

            AppendDiffValues(node, editor);
            node.Expand(true);

            return node;
        }

        private static void AppendDiffValues(TreeNode node, CustomEditor editor)
        {
            if (editor is RemovedScriptDummy or RemovedActorDummy)
            {
                AppendDiffValues(node, "present", "<removed>");
                return;
            }

            var values = editor.Values;
            if (values == null || values.Count != 1)
                return;

            if (values[0] is SceneObject addedObject && values.Info == ScriptMemberInfo.Null)
            {
                if (!addedObject.HasPrefabLink)
                    AppendDiffValues(node, "<missing>", FormatDiffValue(addedObject));
                return;
            }

            if (!values.HasReferenceValue || !values.IsReferenceValueModified)
                return;
            AppendDiffValues(node, FormatDiffValue(values.ReferenceValue), FormatDiffValue(values[0]));
        }

        private static void AppendDiffValues(TreeNode node, string previous, string current)
        {
            previous ??= "<null>";
            current ??= "<null>";
            node.Text += $" — Previous: {TruncateDiffValue(previous)} | Current: {TruncateDiffValue(current)}";
            node.TooltipText = $"Previous: {previous}\nCurrent: {current}";
        }

        private static string TruncateDiffValue(string value)
        {
            const int maxLength = 64;
            return value.Length <= maxLength ? value : value.Substring(0, maxLength - 1) + "…";
        }

        private static string FormatDiffValue(object value)
        {
            if (value == null)
                return "<null>";
            if (value is string text)
                return $"\"{text.Replace("\r", string.Empty).Replace("\n", "↵")}\"";
            if (value is Actor actor)
                return $"{actor.Name} ({Utilities.Utils.GetPropertyNameUI(actor.GetType().Name)})";
            if (value is Script script)
                return $"{Utilities.Utils.GetPropertyNameUI(script.GetType().Name)} on {script.Actor?.Name ?? "<missing actor>"}";
            if (value is FlaxEngine.Object obj)
            {
                var item = Editor.Instance.ContentDatabase.FindAsset(obj.ID);
                return item?.ShortName ?? obj.ToString();
            }
            if (value is IDictionary dictionary)
                return $"{{{dictionary.Count} item{(dictionary.Count == 1 ? string.Empty : "s")}}}";
            if (value is IList list)
            {
                const int maxItems = 4;
                var items = new List<string>(Math.Min(list.Count, maxItems));
                for (int i = 0; i < list.Count && i < maxItems; i++)
                    items.Add(TruncateDiffValue(FormatDiffValue(list[i])));
                if (list.Count > maxItems)
                    items.Add($"… ({list.Count - maxItems} more)");
                return $"[{string.Join(", ", items)}]";
            }
            if (value is IFormattable formattable)
                return formattable.ToString(null, CultureInfo.InvariantCulture);
            return value.ToString();
        }

        private class RemovedScriptDummy : CustomEditor
        {
            /// <summary>
            /// The removed prefab object (from the prefab default instance).
            /// </summary>
            public Script PrefabObject;

            /// <summary>
            /// The prefab instance actor that should own the restored script.
            /// </summary>
            public Actor ParentActor;

            /// <inheritdoc />
            public override void Initialize(LayoutElementsContainer layout)
            {
                // Not used
            }
        }
        
        private class RemovedActorDummy : CustomEditor
        {
            /// <summary>
            /// The removed prefab object (from the prefab default instance).
            /// </summary>
            public Actor PrefabObject;

            /// <summary>
            /// The prefab instance's parent.
            /// </summary>
            public Actor ParentActor;

            /// <summary>
            /// The order of the removed actor in the parent.
            /// </summary>
            public int OrderInParent;

            /// <inheritdoc />
            public override void Initialize(LayoutElementsContainer layout)
            {
                // Not used
            }
        }

        private TreeNode ProcessDiff(CustomEditor editor, bool skipIfNotModified = true)
        {
            // Special case for new Script or child actor added to actor
            if ((editor.Values[0] is Script script && !script.HasPrefabLink) || (editor.Values[0] is Actor a && !a.HasPrefabLink))
                return CreateDiffNode(editor);

            // Skip if no change detected
            var isRefEdited = editor.Values.IsReferenceValueModified;
            if (!isRefEdited && skipIfNotModified && editor is not ScriptsEditor)
                return null;

            TreeNode result = null;
            if (editor.ChildrenEditors.Count == 0 || (isRefEdited && editor is CollectionEditor))
                result = CreateDiffNode(editor);
            bool isScriptEditorWithRefValue = editor is ScriptsEditor && editor.Values.HasReferenceValue;
            var editedActor = editor is ActorEditor ? editor.Values[0] as Actor : null;
            bool isPrefabRootActorEditor = editedActor && editedActor.IsPrefabRoot;
            for (int i = 0; i < editor.ChildrenEditors.Count; i++)
            {
                var childEditor = editor.ChildrenEditors[i];

                // Prefab root names identify instances/assets rather than inherited content.
                if (isPrefabRootActorEditor && childEditor.Values.Info.Name == "Name")
                    continue;

                // Root actor placement belongs to the scene instance rather than the prefab asset.
                if (isPrefabRootActorEditor && editedActor.HasScene && childEditor.Values.Info.Name is "LocalPosition" or "LocalOrientation" or "LocalScale")
                    continue;

                var child = ProcessDiff(childEditor, !isScriptEditorWithRefValue);
                if (child != null)
                {
                    if (result == null)
                        result = CreateDiffNode(editor);
                    result.AddChild(child);
                }
            }

            // Show scripts removed from prefab instance (user may want to restore them)
            if (editor is ScriptsEditor && editor.Values.HasReferenceValue && editor.Values.ReferenceValue is Script[] prefabObjectScripts)
            {
                for (int j = 0; j < prefabObjectScripts.Length; j++)
                {
                    var prefabObjectScript = prefabObjectScripts[j];
                    bool isRemoved = true;
                    for (int i = 0; i < editor.ChildrenEditors.Count; i++)
                    {
                        if (editor.ChildrenEditors[i].Values is ScriptsEditor.ScriptsContainer container && container.PrefabObjectId == prefabObjectScript.PrefabObjectID)
                        {
                            // Found
                            isRemoved = false;
                            break;
                        }
                    }
                    if (isRemoved)
                    {
                        var dummy = new RemovedScriptDummy
                        {
                            PrefabObject = prefabObjectScript,
                            ParentActor = FindEditedActor(editor),
                        };
                        var child = CreateDiffNode(dummy);
                        if (result == null)
                            result = CreateDiffNode(editor);
                        result.AddChild(child);
                    }
                }
            }

            // Compare child actors for removed actors.
            if (editor is ActorEditor && editor.Values.HasReferenceValue && editor.Values.ReferenceValue is Actor prefabObjectActor)
            {
                var thisActor = editor.Values[0] as Actor;
                for (int i = 0; i < prefabObjectActor.ChildrenCount; i++)
                {
                    var prefabActorChild = prefabObjectActor.Children[i];
                    if (thisActor == null)
                        continue;
                    bool isRemoved = true;
                    for (int j = 0; j < thisActor.ChildrenCount; j++)
                    {
                        var actorChild = thisActor.Children[j];
                        if (actorChild.PrefabObjectID == prefabActorChild.PrefabObjectID)
                        {
                            isRemoved = false;
                            break;
                        }
                    }
                    if (isRemoved)
                    {
                        var dummy = new RemovedActorDummy
                        {
                            PrefabObject = prefabActorChild,
                            ParentActor = thisActor,
                            OrderInParent = prefabActorChild.OrderInParent,
                        };
                        var child = CreateDiffNode(dummy);
                        if (result == null)
                            result = CreateDiffNode(editor);
                        result.AddChild(child);
                    }
                }
            }

            if (editor is ScriptsEditor && result != null && result.ChildrenCount == 0)
                return null;

            return result;
        }

        private static Actor FindEditedActor(CustomEditor editor)
        {
            for (var current = editor; current != null; current = current.ParentEditor)
            {
                if (current.Values.Count > 0 && current.Values[0] is Actor actor)
                    return actor;
            }
            return null;
        }

        private TreeNode CreateDiffTree(Actor actor, Actor referenceActor, CustomEditorPresenter presenter, LayoutElementsContainer layout)
        {
            var actorNode = Editor.Instance.Scene.GetActorNode(actor);
            var editableObject = actorNode?.EditableObject ?? actor;
            ValueContainer vc = new ValueContainer(ScriptMemberInfo.Null);
            vc.SetType(new ScriptType(editableObject.GetType()));
            vc.Add(editableObject);
            var editor = CustomEditorsUtil.CreateEditor(vc, null, false);
            editor.Initialize(presenter, layout, vc);
            if (referenceActor)
            {
                editor.Values.SetReferenceValue(referenceActor);
                editor.RefreshReferenceValue();
            }
            var node = ProcessDiff(editor, false);
            layout.ClearLayout();
            foreach (var child in actor.Children)
            {
                var childNode = CreateDiffTree(child, FindReferenceActor(child, referenceActor), presenter, layout);
                if (childNode == null)
                    continue;
                if (node == null)
                    node = CreateDiffNode(editor);
                node.AddChild(childNode);
            }
            return node;
        }

        private static Actor FindReferenceActor(Actor actor, Actor referenceParent)
        {
            if (!actor || !referenceParent || !actor.HasPrefabLink)
                return null;

            var prefabId = actor.PrefabID;
            var prefabObjectId = actor.PrefabObjectID;
            var visited = new HashSet<Guid>();
            while (prefabObjectId != Guid.Empty)
            {
                for (int i = 0; i < referenceParent.ChildrenCount; i++)
                {
                    var child = referenceParent.GetChild(i);
                    if (child.PrefabObjectID == prefabObjectId)
                        return child;
                }

                if (prefabId == Guid.Empty || !visited.Add(prefabId))
                    break;
                var prefab = FlaxEngine.Content.Load<Prefab>(prefabId);
                var currentObjectId = prefabObjectId;
                if (!prefab || prefab.WaitForLoaded() || !prefab.GetNestedObject(ref currentObjectId, out prefabId, out prefabObjectId))
                    break;
            }
            return null;
        }

        private void ViewChanges(Control target, Float2 targetLocation)
        {
            // Build a tree out of modified properties
            var thisActor = (Actor)Values[0];
            var rootActor = thisActor.IsPrefabRoot ? thisActor : thisActor.GetPrefabRoot();
            var referenceRoot = rootActor == thisActor ? Values.ReferenceValue as Actor : null;
            var presenter = new CustomEditorPresenter(null);
            var layout = new CustomElementsContainer<ContainerControl>();
            var rootNode = CreateDiffTree(rootActor, referenceRoot, presenter, layout);

            // Skip if no changes detected
            if (rootNode == null)
            {
                var cm1 = new ContextMenu();
                cm1.AddButton("No changes detected");
                cm1.Show(target, targetLocation);
                return;
            }

            // Create context menu
            var cm = new PrefabDiffContextMenu(620, 320);
            cm.Tree.AddChild(rootNode);
            cm.Tree.RightClick += OnDiffNodeRightClick;
            cm.Tree.Tag = cm;
            cm.RevertAll += OnDiffRevertAll;
            cm.ApplyAll += OnDiffApplyAll;
            cm.Show(target, targetLocation);
        }

        private void OnDiffNodeRightClick(TreeNode node, Float2 location)
        {
            var diffMenu = (PrefabDiffContextMenu)node.ParentTree.Tag;
            var editor = (CustomEditor)node.Tag;

            var menu = new ContextMenu();
            if (CanDiffApply(editor))
                menu.AddButton("Apply Changes", () => OnDiffApply(editor));
            menu.AddButton("Revert", () => OnDiffRevert(editor));
            menu.AddSeparator();
            menu.AddButton("Revert All", OnDiffRevertAll);
            menu.AddButton("Apply All", OnDiffApplyAll);

            diffMenu.ShowChild(menu, node.PointToParent(diffMenu, new Float2(location.X, node.HeaderHeight)));
        }

        private void OnDiffRevertAll()
        {
            RevertToReferenceValue();
        }

        private void OnDiffApplyAll()
        {
            Editor.Instance.Prefabs.ApplyAll((Actor)Values[0]);

            // Ensure to refresh the layout
            Presenter.BuildLayoutOnUpdate();
        }

        private bool CanDiffApply(CustomEditor editor)
        {
            return editor?.Values != null &&
                   editor.Values.Count == 1 &&
                   editor.Values[0] is SceneObject sceneObject &&
                   CustomEditor.TryGetAddedPrefabObjectApplyTargets(sceneObject, out _, out _);
        }

        private void OnDiffApply(CustomEditor editor)
        {
            if (!CanDiffApply(editor))
                return;

            CustomEditor.TryGetAddedPrefabObjectApplyTargets((SceneObject)editor.Values[0], out var prefabRoot, out var objectToApply);
            Editor.Instance.Prefabs.ApplyAddedObject(prefabRoot, objectToApply);

            // Ensure to refresh the layout
            Presenter.BuildLayoutOnUpdate();
        }

        private static void GetAllPrefabObjects(List<object> objects, Actor actor)
        {
            objects.Add(actor);
            objects.AddRange(actor.Scripts);
            var children = actor.Children;
            foreach (var child in children)
                GetAllPrefabObjects(objects, child);
        }

        private void OnDiffRevert(CustomEditor editor)
        {
            // Special case for removed Script from actor
            if (editor is RemovedScriptDummy removed)
            {
                Editor.Log("Reverting removed script changes to prefab (adding it)");

                var actor = removed.ParentActor;
                if (!actor)
                {
                    Editor.LogWarning("Cannot restore removed prefab script because its instance actor is missing.");
                    return;
                }
                var restored = RestoreRemovedPrefabScript(actor, removed.PrefabObject);
                if (!restored)
                    return;

                var action = AddRemoveScript.Added(restored);
                Presenter.Undo?.AddAction(action);

                return;
            }

            // Special case for reverting removed Actors
            if (editor is RemovedActorDummy removedActor)
            {
                Editor.Log("Reverting removed actor changes to prefab (adding it)");

                var parentActor = removedActor.ParentActor;
                var restored = parentActor.AddChild(removedActor.PrefabObject.GetType());
                var prefabId = parentActor.PrefabID;
                var prefabObjectId = removedActor.PrefabObject.PrefabObjectID;
                string data = JsonSerializer.Serialize(removedActor.PrefabObject);
                JsonSerializer.Deserialize(restored, data);
                Presenter.Owner.SceneContext.Spawn(restored, parentActor, removedActor.OrderInParent);
                Actor.Internal_LinkPrefab(FlaxEngine.Object.GetUnmanagedPtr(restored), ref prefabId, ref prefabObjectId);
                return;
            }

            // Special case for new Script added to actor
            if (editor.Values[0] is Script script && !script.HasPrefabLink)
            {
                Editor.Log("Reverting added script changes to prefab (removing it)");

                var action = AddRemoveScript.Remove(script);
                action.Do();
                Presenter.Undo?.AddAction(action);

                return;
            }
            
            // Special case for new Actor added to actor
            if (editor.Values[0] is Actor a && !a.HasPrefabLink)
            {
                Editor.Log("Reverting added actor changes to prefab (removing it)");

                // TODO: Keep previous selection.
                var context = Presenter.Owner.SceneContext;
                context.Select(SceneGraph.SceneGraphFactory.FindNode(a.ID));
                context.DeleteSelection();

                return;
            }

            if (Presenter.Undo != null && Presenter.Undo.Enabled)
            {
                var thisActor = (Actor)Values[0];
                var rootActor = thisActor.IsPrefabRoot ? thisActor : thisActor.GetPrefabRoot();
                var prefabObjects = new List<object>();
                GetAllPrefabObjects(prefabObjects, rootActor);
                using (new UndoMultiBlock(Presenter.Undo, prefabObjects, "Revert to Prefab"))
                {
                    editor.RevertToReferenceValue();
                    editor.RefreshInternal();
                }
            }
            else
            {
                editor.RevertToReferenceValue();
                editor.RefreshInternal();
            }
        }

        private static Script RestoreRemovedPrefabScript(Actor actor, Script prefabObject)
        {
            if (!actor || !prefabObject)
                return null;

            var restored = actor.AddScript(prefabObject.GetType());
            if (!restored)
            {
                Editor.LogWarning("Cannot restore removed prefab script because its type could not be instantiated.");
                return null;
            }
            string data = JsonSerializer.Serialize(prefabObject);
            JsonSerializer.Deserialize(restored, data);
            restored.Parent = actor;
            var prefabId = prefabObject.PrefabID;
            var prefabObjectId = prefabObject.PrefabObjectID;
            Script.Internal_LinkPrefab(FlaxEngine.Object.GetUnmanagedPtr(restored), ref prefabId, ref prefabObjectId);
            return restored;
        }
    }
}
