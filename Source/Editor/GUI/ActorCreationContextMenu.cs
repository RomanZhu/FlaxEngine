// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Scripting;
using FlaxEngine;
using LegacyContextMenu = FlaxEditor.GUI.ContextMenu.ContextMenu;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Shared, searchable cascading catalog used by editor surfaces that create scene objects.
    /// </summary>
    internal sealed class ActorCreationContextMenu : SearchableContextMenu
    {
        /// <inheritdoc />
        protected override float SubmenuAimDelay => float.PositiveInfinity;

        public enum EntryKind
        {
            Actor,
            Control,
            Primitive,
        }

        public sealed class Entry
        {
            public string Name;
            public string Category;
            public string SearchTerms;
            public EntryKind Kind;
            public ScriptType ScriptType;
            public string AssetPath;
        }

        private static readonly (string Name, string Path)[] Primitives =
        {
            ("Cube", "Primitives/Cube.flax"),
            ("Sphere", "Primitives/Sphere.flax"),
            ("Plane", "Primitives/Plane.flax"),
            ("Cylinder", "Primitives/Cylinder.flax"),
            ("Cone", "Primitives/Cone.flax"),
            ("Capsule", "Primitives/Capsule.flax"),
        };

        private static readonly string[] CategoryOrder =
        {
            "Primitives",
            "CSG Brushes",
            null,
            "Lights",
            "Physics",
            "Visuals",
            null,
            "Audio",
            "Animation",
            "Navigation",
            null,
            "UI",
            "GUI",
            null,
            "Actors",
            "Other",
        };

        public ActorCreationContextMenu(Editor editor, Action<Entry> created)
        : base(BuildMenu(editor, created), "New")
        {
        }

        private static LegacyContextMenu BuildMenu(Editor editor, Action<Entry> created)
        {
            var menu = new LegacyContextMenu();
            var actorTypes = editor.CodeEditing.Actors.Get();

            // Match the familiar Add menu flow: the generic empty object is always first.
            bool hasEmptyActor = false;
            foreach (var actorType in actorTypes)
            {
                if (actorType.Type != typeof(EmptyActor))
                    continue;
                AddEntry(menu, created, new Entry
                {
                    Name = "Empty Actor",
                    Category = string.Empty,
                    SearchTerms = $"Empty Actor {actorType.Name} {actorType.TypeName} actor entity object empty",
                    Kind = EntryKind.Actor,
                    ScriptType = actorType,
                });
                hasEmptyActor = true;
                break;
            }
            if (hasEmptyActor)
                menu.AddSeparator();

            // Pre-create the built-in groups to keep their root order stable.
            foreach (var category in CategoryOrder)
            {
                if (category == null)
                    menu.AddSeparator();
                else
                    menu.GetOrAddChildMenu(category);
            }

            foreach (var primitive in Primitives)
            {
                AddEntry(menu, created, new Entry
                {
                    Name = primitive.Name,
                    Category = "Primitives",
                    SearchTerms = $"{primitive.Name} mesh model geometry static t:{primitive.Name}",
                    Kind = EntryKind.Primitive,
                    AssetPath = primitive.Path,
                });
            }

            AddTypes(menu, created, actorTypes, EntryKind.Actor, "Actors");
            AddTypes(menu, created, editor.CodeEditing.Controls.Get(), EntryKind.Control, "GUI");
            SortSubmenus(menu);
            return menu;
        }

        private static void SortSubmenus(LegacyContextMenu menu)
        {
            foreach (var item in menu.Items)
            {
                if (item is ContextMenuChildMenu childMenu)
                {
                    SortSubmenus(childMenu.ContextMenu);
                    childMenu.ContextMenu.SortButtons(true);
                }
            }
        }

        private static void AddTypes(LegacyContextMenu menu, Action<Entry> created, IEnumerable<ScriptType> types, EntryKind kind, string fallbackCategory)
        {
            foreach (var type in types)
            {
                if (type.IsAbstract || (kind == EntryKind.Actor && type.Type == typeof(EmptyActor)))
                    continue;

                ActorToolboxAttribute toolbox = null;
                ActorContextMenuAttribute contextMenu = null;
                foreach (var attribute in type.GetAttributes(false))
                {
                    if (attribute is ActorToolboxAttribute actorToolbox)
                        toolbox = actorToolbox;
                    else if (attribute is ActorContextMenuAttribute actorContextMenu)
                        contextMenu = actorContextMenu;
                }

                var name = toolbox != null && !string.IsNullOrWhiteSpace(toolbox.Name)
                    ? toolbox.Name.Trim()
                    : FlaxEditor.Utilities.Utils.GetPropertyNameUI(type.Name);
                var category = toolbox != null && !string.IsNullOrWhiteSpace(toolbox.Group)
                    ? toolbox.Group.Trim()
                    : fallbackCategory;
                if (kind == EntryKind.Actor && contextMenu != null && !string.IsNullOrWhiteSpace(contextMenu.Path))
                    ApplyContextMenuPath(contextMenu.Path, ref category, ref name);
                if (kind == EntryKind.Actor && type.Type != null && typeof(BoxBrush).IsAssignableFrom(type.Type))
                    category = "CSG Brushes";
                AddEntry(menu, created, new Entry
                {
                    Name = name,
                    Category = category,
                    SearchTerms = $"{name} {type.Name} {type.TypeName} {category} {(kind == EntryKind.Control ? "ui gui widget control" : "actor entity object")}",
                    Kind = kind,
                    ScriptType = type,
                });
            }
        }

        private static void ApplyContextMenuPath(string path, ref string category, ref string name)
        {
            var parts = path.Split(new[] { '/', '\\' }, StringSplitOptions.RemoveEmptyEntries);
            int first = parts.Length > 0 && string.Equals(parts[0].Trim(), "New", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
            if (first >= parts.Length)
                return;

            name = parts[parts.Length - 1].Trim();
            int categoryCount = parts.Length - first - 1;
            if (categoryCount <= 0)
                return;

            var categories = new string[categoryCount];
            for (int i = 0; i < categoryCount; i++)
                categories[i] = parts[first + i].Trim();
            category = string.Join("/", categories);
        }

        private static void AddEntry(LegacyContextMenu root, Action<Entry> created, Entry entry)
        {
            var menu = root;
            var categories = (entry.Category ?? "Other").Split(new[] { '/', '\\' }, StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < categories.Length; i++)
                menu = menu.GetOrAddChildMenu(categories[i].Trim()).ContextMenu;

            var button = menu.AddButton(entry.Name, () => created?.Invoke(entry));
            button.TooltipText = entry.SearchTerms;
        }
    }
}
