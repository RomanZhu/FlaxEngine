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
using System.Collections.Generic;
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

            node.Expand(true);

            return node;
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

        private enum PrefabOverrideKind
        {
            Group,
            Modified,
            Added,
            Removed,
        }

        private sealed class PrefabOverrideEntry
        {
            public CustomEditor Editor;
            public SceneObject Current;
            public SceneObject Source;
            public PrefabOverrideKind Kind;
            public string Title;
        }

        private static string GetOverrideTitle(SceneObject sceneObject)
        {
            var typeName = Utilities.Utils.GetPropertyNameUI(sceneObject.GetType().Name);
            return sceneObject is Actor actor ? $"{actor.Name} ({typeName})" : $"{typeName} (Script)";
        }

        private static bool IsSceneOverrideNode(TreeNode node)
        {
            if (!(node.Tag is CustomEditor editor))
                return false;
            return editor is RemovedScriptDummy or RemovedActorDummy ||
                   IsSceneObjectEditor(editor);
        }

        private static bool IsSceneObjectEditor(CustomEditor editor)
        {
            if (editor.Values == null || editor.Values.Count != 1)
                return false;
            return editor is ActorEditor && editor.Values[0] is Actor ||
                   editor.Values is ScriptsEditor.ScriptsContainer && editor.Values[0] is Script;
        }

        private static bool HasPropertyOverride(TreeNode node)
        {
            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (!(node.GetChild(i) is TreeNode child) || IsSceneOverrideNode(child))
                    continue;
                if (child.ChildrenCount == 0)
                    return true;
                if (HasPropertyOverride(child))
                    return true;
            }
            return false;
        }

        private static TreeNode CreateOverrideEntryNode(TreeNode diffNode, bool isRoot = false)
        {
            var editor = (CustomEditor)diffNode.Tag;
            var entry = new PrefabOverrideEntry
            {
                Editor = editor,
            };

            if (editor is RemovedScriptDummy removedScript)
            {
                entry.Kind = PrefabOverrideKind.Removed;
                entry.Source = removedScript.PrefabObject;
            }
            else if (editor is RemovedActorDummy removedActor)
            {
                entry.Kind = PrefabOverrideKind.Removed;
                entry.Source = removedActor.PrefabObject;
            }
            else
            {
                entry.Current = editor.Values[0] as SceneObject;
                entry.Source = editor.Values.ReferenceValue as SceneObject;
                entry.Kind = entry.Source == null && (!isRoot || !entry.Current.HasPrefabLink)
                    ? PrefabOverrideKind.Added
                    : HasPropertyOverride(diffNode) ? PrefabOverrideKind.Modified : PrefabOverrideKind.Group;
            }

            var sceneObject = entry.Current ?? entry.Source;
            entry.Title = GetOverrideTitle(sceneObject);
            var node = new TreeNode(false)
            {
                Tag = entry,
                Text = entry.Title,
                TooltipText = entry.Kind == PrefabOverrideKind.Group ? null : $"{entry.Kind} prefab component",
            };
            switch (entry.Kind)
            {
            case PrefabOverrideKind.Modified:
                node.Text += "  (Modified)";
                node.TextColor = FlaxEngine.GUI.Style.Current.BorderSelected;
                break;
            case PrefabOverrideKind.Added:
                node.Text += "  (Added)";
                node.TextColor = new Color(0.45f, 0.85f, 0.45f);
                break;
            case PrefabOverrideKind.Removed:
                node.Text += "  (Removed)";
                node.TextColor = Color.OrangeRed;
                break;
            }
            node.Expand(true);
            return node;
        }

        private static void AddOverrideEntries(TreeNode parent, TreeNode diffNode)
        {
            if (IsSceneOverrideNode(diffNode))
            {
                var node = CreateOverrideEntryNode(diffNode);
                parent.AddChild(node);
                for (int i = 0; i < diffNode.ChildrenCount; i++)
                {
                    if (diffNode.GetChild(i) is TreeNode child)
                        AddOverrideEntries(node, child);
                }
                return;
            }

            for (int i = 0; i < diffNode.ChildrenCount; i++)
            {
                if (diffNode.GetChild(i) is TreeNode child)
                    AddOverrideEntries(parent, child);
            }
        }

        private static TreeNode CreateOverridesTree(TreeNode diffRoot)
        {
            var root = CreateOverrideEntryNode(diffRoot, true);
            for (int i = 0; i < diffRoot.ChildrenCount; i++)
            {
                if (diffRoot.GetChild(i) is TreeNode child)
                    AddOverrideEntries(root, child);
            }
            return root;
        }

        private TreeNode ProcessDiff(CustomEditor editor, bool skipIfNotModified = true)
        {
            // Special case for new Script or child actor added to actor
            if (IsSceneObjectEditor(editor) && editor.Values[0] is SceneObject sceneObject && !sceneObject.HasPrefabLink)
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

        private TreeNode CreateDiffTree(Actor actor, Actor referenceActor, CustomEditorPresenter presenter, LayoutElementsContainer layout, List<CustomEditor> rootEditors)
        {
            var actorNode = Editor.Instance.Scene.GetActorNode(actor);
            var editableObject = actorNode?.EditableObject ?? actor;
            ValueContainer vc = new ValueContainer(ScriptMemberInfo.Null);
            vc.SetType(new ScriptType(editableObject.GetType()));
            vc.Add(editableObject);
            var editor = CustomEditorsUtil.CreateEditor(vc, null, false);
            editor.Initialize(presenter, layout, vc);
            rootEditors.Add(editor);
            if (referenceActor)
            {
                editor.Values.SetReferenceValue(referenceActor);
                editor.RefreshReferenceValue();
            }
            var node = ProcessDiff(editor, false);
            layout.ClearLayout();
            foreach (var child in actor.Children)
            {
                var childNode = CreateDiffTree(child, FindReferenceActor(child, referenceActor), presenter, layout, rootEditors);
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
            var diffEditors = new List<CustomEditor>();
            var diffRoot = CreateDiffTree(rootActor, referenceRoot, presenter, layout, diffEditors);

            // Skip if no changes detected
            if (diffRoot == null)
            {
                CleanupDiffEditors(diffEditors);
                var cm1 = new ContextMenu();
                cm1.AddButton("No changes detected");
                cm1.Show(target, targetLocation);
                return;
            }

            // Create component-level overrides popup
            var rootNode = CreateOverridesTree(diffRoot);
            diffRoot.Dispose();
            var cm = new PrefabDiffContextMenu();
            var details = new PrefabOverrideDetailsContextMenu(Presenter.Undo, Presenter.Owner);
            cm.Tree.AddChild(rootNode);
            cm.Tree.SelectedChanged += (_, selection) => OnDiffSelectionChanged(cm, details, selection);
            cm.Tree.RightClick += (node, location) => OnDiffNodeRightClick(node, location, diffEditors);
            cm.Tree.Tag = cm;
            cm.RevertAll += () => OnDiffRevertAll(rootNode);
            cm.ApplyAll += () =>
            {
                CleanupDiffEditors(diffEditors);
                OnDiffApplyAll();
            };
            cm.Closed += () => CleanupDiffEditors(diffEditors);
            cm.Show(target, targetLocation);
        }

        private static void CleanupDiffEditors(List<CustomEditor> editors)
        {
            for (int i = 0; i < editors.Count; i++)
                editors[i].Cleanup();
            editors.Clear();
        }

        private void OnDiffSelectionChanged(PrefabDiffContextMenu diffMenu, PrefabOverrideDetailsContextMenu details, List<TreeNode> selection)
        {
            if (selection.Count == 0 || !(selection[selection.Count - 1].Tag is PrefabOverrideEntry entry) || entry.Kind == PrefabOverrideKind.Group)
            {
                if (details.IsOpened)
                    details.Hide();
                return;
            }

            if (details.IsOpened)
                details.Hide();

            Action revert = () =>
            {
                RevertDiffEntry(entry, diffMenu);
            };
            Action<ContextMenu> setupApply = menu => SetupDiffApplyMenu(menu, entry, diffMenu);
            switch (entry.Kind)
            {
            case PrefabOverrideKind.Modified:
                details.ShowModified(entry.Title, entry.Source, entry.Current, revert, setupApply);
                break;
            case PrefabOverrideKind.Added:
                details.ShowAdded(entry.Title, entry.Current, revert, setupApply);
                break;
            case PrefabOverrideKind.Removed:
                details.ShowRemoved(entry.Title, entry.Source, revert, setupApply);
                break;
            }

            var topLeft = diffMenu.PointToScreen(Float2.Zero);
            var bottomRight = diffMenu.PointToScreen(diffMenu.Size);
            var monitor = Platform.GetMonitorBounds(topLeft);
            float dpiScale = diffMenu.RootWindow?.DpiScale ?? 1.0f;
            float detailsWidth = details.Width * dpiScale;
            float detailsHeight = details.Height * dpiScale;
            float leftSpace = topLeft.X - monitor.Left;
            float rightSpace = monitor.Right - bottomRight.X;
            float topSpace = topLeft.Y - monitor.Top;
            float bottomSpace = monitor.Bottom - bottomRight.Y;
            bool preferLeft = diffMenu.Direction is ContextMenuDirection.LeftDown or ContextMenuDirection.LeftUp;
            bool preferUp = diffMenu.Direction is ContextMenuDirection.LeftUp or ContextMenuDirection.RightUp;
            bool openLeft = leftSpace >= detailsWidth && (rightSpace < detailsWidth || preferLeft) ||
                            leftSpace < detailsWidth && rightSpace < detailsWidth && leftSpace > rightSpace;
            bool openUp = topSpace >= detailsHeight && (bottomSpace < detailsHeight || preferUp) ||
                          topSpace < detailsHeight && bottomSpace < detailsHeight && topSpace > bottomSpace;
            details.OpenDirection = openLeft
                ? openUp ? ContextMenuDirection.LeftUp : ContextMenuDirection.LeftDown
                : openUp ? ContextMenuDirection.RightUp : ContextMenuDirection.RightDown;
            var location = new Float2(openLeft ? 0.0f : diffMenu.Width, openUp ? diffMenu.Height : 0.0f);
            diffMenu.ShowChild(details, location, false);
        }

        private void SetupDiffApplyMenu(ContextMenu menu, PrefabOverrideEntry entry, PrefabDiffContextMenu diffMenu)
        {
            switch (entry.Kind)
            {
            case PrefabOverrideKind.Modified:
                entry.Editor.RefreshInternal();
                entry.Editor.AddApplyToPrefabButtons(menu, diffMenu.Hide);
                break;
            case PrefabOverrideKind.Added:
                CustomEditor.AddApplyAddedPrefabObjectButtons(menu, entry.Current, () =>
                {
                    Presenter.BuildLayoutOnUpdate();
                    diffMenu.Hide();
                });
                break;
            case PrefabOverrideKind.Removed:
                CustomEditor.AddApplyRemovedPrefabObjectButtons(menu, entry.Source, diffMenu.Hide);
                break;
            }
        }

        private void OnDiffNodeRightClick(TreeNode node, Float2 location, List<CustomEditor> diffEditors)
        {
            var diffMenu = (PrefabDiffContextMenu)node.ParentTree.Tag;
            if (!(node.Tag is PrefabOverrideEntry entry) || entry.Kind == PrefabOverrideKind.Group)
                return;

            var menu = new ContextMenu();
            SetupDiffApplyMenu(menu, entry, diffMenu);
            menu.AddButton("Revert", () =>
            {
                RevertDiffEntry(entry, diffMenu);
            });
            menu.AddSeparator();
            menu.AddButton("Revert All", () =>
            {
                OnDiffRevertAll(diffMenu.Tree.GetChild(0) as TreeNode);
                diffMenu.Hide();
            });
            menu.AddButton("Apply All", () =>
            {
                CleanupDiffEditors(diffEditors);
                OnDiffApplyAll();
                diffMenu.Hide();
            });

            diffMenu.ShowChild(menu, node.PointToParent(diffMenu, new Float2(location.X, node.HeaderHeight)));
        }

        private void RevertDiffEntry(PrefabOverrideEntry entry, PrefabDiffContextMenu diffMenu)
        {
            if (entry.Kind == PrefabOverrideKind.Modified)
                entry.Editor.RefreshInternal();
            if (!OnDiffRevert(entry))
                return;

            FlaxEngine.Scripting.FlushRemovedObjects();
            Presenter.OnModified();
            Presenter.BuildLayoutOnUpdate();
            diffMenu.Hide();
        }

        private void OnDiffRevertAll(TreeNode rootNode)
        {
            var removed = new List<PrefabOverrideEntry>();
            var modified = new List<PrefabOverrideEntry>();
            var added = new List<PrefabOverrideEntry>();
            CollectOverrideEntries(rootNode, removed, modified, added);

            foreach (var entry in removed)
                OnDiffRevert(entry);
            foreach (var entry in modified)
            {
                entry.Editor.RefreshInternal();
                OnDiffRevert(entry);
            }
            foreach (var entry in added)
                OnDiffRevert(entry);

            FlaxEngine.Scripting.FlushRemovedObjects();
            Presenter.OnModified();
            Presenter.BuildLayoutOnUpdate();
        }

        private static void CollectOverrideEntries(TreeNode node, List<PrefabOverrideEntry> removed, List<PrefabOverrideEntry> modified, List<PrefabOverrideEntry> added)
        {
            if (node == null)
                return;
            if (node.Tag is PrefabOverrideEntry entry)
            {
                switch (entry.Kind)
                {
                case PrefabOverrideKind.Removed:
                    removed.Add(entry);
                    break;
                case PrefabOverrideKind.Modified:
                    modified.Add(entry);
                    break;
                case PrefabOverrideKind.Added:
                    added.Add(entry);
                    if (entry.Current is Actor)
                        return;
                    break;
                }
            }

            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is TreeNode child)
                    CollectOverrideEntries(child, removed, modified, added);
            }
        }

        private void OnDiffApplyAll()
        {
            Editor.Instance.Prefabs.ApplyAll((Actor)Values[0]);

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

        private bool OnDiffRevert(PrefabOverrideEntry entry)
        {
            var editor = entry.Editor;

            // Special case for removed Script from actor
            if (editor is RemovedScriptDummy removed)
            {
                Editor.Log("Reverting removed script changes to prefab (adding it)");

                var actor = removed.ParentActor;
                if (!actor)
                {
                    Editor.LogWarning("Cannot restore removed prefab script because its instance actor is missing.");
                    return false;
                }
                var restored = RestoreRemovedPrefabScript(actor, removed.PrefabObject);
                if (!restored)
                    return false;

                var action = AddRemoveScript.Added(restored);
                Presenter.Undo?.AddAction(action);

                return true;
            }

            // Special case for reverting removed Actors
            if (editor is RemovedActorDummy removedActor)
            {
                Editor.Log("Reverting removed actor changes to prefab (adding it)");

                var parentActor = removedActor.ParentActor;
                if (!parentActor)
                {
                    Editor.LogWarning("Cannot restore removed prefab actor because its instance parent is missing.");
                    return false;
                }
                var restored = RestoreRemovedPrefabActor(parentActor, removedActor.PrefabObject, removedActor.OrderInParent);
                if (!restored)
                    return false;

                var node = SceneGraphFactory.FindNode(restored.ID);
                if (node == null)
                {
                    Editor.LogWarning("Cannot restore removed prefab actor because its scene graph node is missing.");
                    return false;
                }
                var nodes = node.BuildAllNodes().Where(x => x.CanDelete).ToList();
                Presenter.Undo?.AddAction(new DeleteActorsAction(nodes, true));
                return true;
            }

            // Special case for new Script added to actor
            if (entry.Kind == PrefabOverrideKind.Added && entry.Current is Script script)
            {
                Editor.Log("Reverting added script changes to prefab (removing it)");

                var action = AddRemoveScript.Remove(script);
                action.Do();
                Presenter.Undo?.AddAction(action);

                return true;
            }
            
            // Special case for new Actor added to actor
            if (entry.Kind == PrefabOverrideKind.Added && entry.Current is Actor a)
            {
                Editor.Log("Reverting added actor changes to prefab (removing it)");

                var node = SceneGraphFactory.FindNode(a.ID);
                if (node == null)
                    return false;
                var nodes = node.BuildAllNodes().Where(x => x.CanDelete).ToList();
                var context = node.Root?.SceneContext;
                if (context != null)
                {
                    for (int i = 0; i < nodes.Count; i++)
                    {
                        if (context.Selection.Contains(nodes[i]))
                            context.Deselect(nodes[i]);
                    }
                }
                var action = new DeleteActorsAction(nodes);
                action.Do();
                Presenter.Undo?.AddAction(action);

                return true;
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
            return true;
        }

        private static Actor RestoreRemovedPrefabActor(Actor parentActor, Actor prefabObject, int orderInParent)
        {
            if (!parentActor || !prefabObject)
                return null;

            var sourceActors = new List<Actor>();
            CollectActorHierarchy(prefabObject, sourceActors);
            var data = Actor.ToBytes(sourceActors.ToArray());
            if (data == null || data.Length == 0)
            {
                Editor.LogWarning("Cannot restore removed prefab actor because its hierarchy could not be serialized.");
                return null;
            }
            var serializedIds = Actor.TryGetSerializedObjectsIds(data);
            if (serializedIds == null)
            {
                Editor.LogWarning("Cannot restore removed prefab actor because its hierarchy could not be serialized.");
                return null;
            }

            var idsMapping = new Dictionary<Guid, Guid>(serializedIds.Length);
            for (int i = 0; i < serializedIds.Length; i++)
                idsMapping[serializedIds[i]] = Guid.NewGuid();
            AddExistingPrefabObjectMappings(prefabObject, parentActor, idsMapping);

            var restoredActors = Actor.FromBytes(data, idsMapping);
            if (restoredActors == null)
            {
                Editor.LogWarning("Cannot restore removed prefab actor because its hierarchy could not be deserialized.");
                return null;
            }

            var restoredId = idsMapping[prefabObject.ID];
            var restored = FlaxEngine.Object.Find<Actor>(ref restoredId);
            if (!restored)
                return null;
            restored.SetParent(parentActor, false);
            restored.OrderInParent = orderInParent;
            LinkRestoredPrefabHierarchy(prefabObject, idsMapping);
            Editor.Instance.Scene.MarkSceneEdited(restored.Scene);
            return restored;
        }

        private static void CollectActorHierarchy(Actor actor, List<Actor> result)
        {
            result.Add(actor);
            for (int i = 0; i < actor.ChildrenCount; i++)
                CollectActorHierarchy(actor.GetChild(i), result);
        }

        private static void CollectSceneObjectHierarchy(Actor actor, List<SceneObject> result)
        {
            result.Add(actor);
            for (int i = 0; i < actor.ScriptsCount; i++)
            {
                var script = actor.GetScript(i);
                if (script)
                    result.Add(script);
            }
            for (int i = 0; i < actor.ChildrenCount; i++)
                CollectSceneObjectHierarchy(actor.GetChild(i), result);
        }

        private static void AddExistingPrefabObjectMappings(Actor sourceActor, Actor targetActor, Dictionary<Guid, Guid> idsMapping)
        {
            var sourceRoot = sourceActor.IsPrefabRoot ? sourceActor : sourceActor.GetPrefabRoot();
            var targetRoot = targetActor.IsPrefabRoot ? targetActor : targetActor.GetPrefabRoot();
            if (!sourceRoot || !targetRoot)
                return;

            var sourceObjects = new List<SceneObject>();
            var targetObjects = new List<SceneObject>();
            CollectSceneObjectHierarchy(sourceRoot, sourceObjects);
            CollectSceneObjectHierarchy(targetRoot, targetObjects);
            var targetObjectsByPrefabId = new Dictionary<Guid, SceneObject>(targetObjects.Count);
            for (int i = 0; i < targetObjects.Count; i++)
            {
                var targetObject = targetObjects[i];
                if (targetObject.PrefabObjectID != Guid.Empty)
                    targetObjectsByPrefabId[targetObject.PrefabObjectID] = targetObject;
            }

            for (int i = 0; i < sourceObjects.Count; i++)
            {
                var sourceObject = sourceObjects[i];
                if (idsMapping.ContainsKey(sourceObject.ID) || !sourceObject.HasPrefabLink)
                    continue;
                if (CustomEditor.TryMapPrefabObjectId(sourceObject.PrefabID, sourceObject.PrefabObjectID, targetRoot.PrefabID, out var targetObjectId) &&
                    targetObjectsByPrefabId.TryGetValue(targetObjectId, out var targetObject))
                    idsMapping[sourceObject.ID] = targetObject.ID;
            }
        }

        private static void LinkRestoredPrefabHierarchy(Actor sourceActor, Dictionary<Guid, Guid> idsMapping)
        {
            LinkRestoredPrefabObject(sourceActor, idsMapping);
            for (int i = 0; i < sourceActor.ScriptsCount; i++)
            {
                var script = sourceActor.GetScript(i);
                if (script)
                    LinkRestoredPrefabObject(script, idsMapping);
            }
            for (int i = 0; i < sourceActor.ChildrenCount; i++)
                LinkRestoredPrefabHierarchy(sourceActor.GetChild(i), idsMapping);
        }

        private static void LinkRestoredPrefabObject(SceneObject sourceObject, Dictionary<Guid, Guid> idsMapping)
        {
            if (!idsMapping.TryGetValue(sourceObject.ID, out var restoredId))
                return;
            var restored = FlaxEngine.Object.Find<SceneObject>(ref restoredId);
            if (!restored)
                return;
            var prefabId = sourceObject.PrefabID;
            var prefabObjectId = sourceObject.PrefabObjectID;
            SceneObject.Internal_LinkPrefab(FlaxEngine.Object.GetUnmanagedPtr(restored), ref prefabId, ref prefabObjectId);
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
