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

        public ActorCreationContextMenu(Editor editor, Action<Entry> created)
        : base(BuildMenu(editor, created), "New")
        {
        }

        private static LegacyContextMenu BuildMenu(Editor editor, Action<Entry> created)
        {
            var menu = new LegacyContextMenu();
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

            AddTypes(menu, created, editor.CodeEditing.Actors.Get(), EntryKind.Actor, "Actors");
            AddTypes(menu, created, editor.CodeEditing.Controls.Get(), EntryKind.Control, "GUI");
            return menu;
        }

        private static void AddTypes(LegacyContextMenu menu, Action<Entry> created, IEnumerable<ScriptType> types, EntryKind kind, string fallbackCategory)
        {
            foreach (var type in types)
            {
                if (type.IsAbstract)
                    continue;

                ActorToolboxAttribute toolbox = null;
                foreach (var attribute in type.GetAttributes(false))
                {
                    if (attribute is ActorToolboxAttribute actorToolbox)
                    {
                        toolbox = actorToolbox;
                        break;
                    }
                }

                var name = toolbox != null && !string.IsNullOrWhiteSpace(toolbox.Name)
                    ? toolbox.Name.Trim()
                    : FlaxEditor.Utilities.Utils.GetPropertyNameUI(type.Name);
                var category = toolbox != null && !string.IsNullOrWhiteSpace(toolbox.Group)
                    ? toolbox.Group.Trim()
                    : fallbackCategory;
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
