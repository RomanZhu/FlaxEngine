// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Modules;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Resizes authored box dimensions changed by the selection bounds gizmo.
    /// </summary>
    [Serializable, HideInEditor]
    internal sealed class ResizeBoundsObjectsAction : ITryUndoAction, ISceneEditAction
    {
        [Serializable]
        internal struct Entry
        {
            public Guid ActorId;
            public Vector3 Before;
            public Vector3 After;

            public static bool TryCreate(Actor actor, out Entry entry)
            {
                entry = default;
                if (actor is BoxCollider boxCollider)
                {
                    entry.ActorId = actor.ID;
                    entry.Before = boxCollider.Size;
                    entry.After = entry.Before;
                    return true;
                }
                if (actor is AudioVolumeBase audioVolume && audioVolume.Shape == AudioVolumeShape.Box)
                {
                    entry.ActorId = actor.ID;
                    entry.Before = audioVolume.BoxSize;
                    entry.After = entry.Before;
                    return true;
                }
                return false;
            }

            public bool Apply(Vector3 size)
            {
                var id = ActorId;
                var actor = FlaxEngine.Object.Find<Actor>(ref id);
                if (actor is BoxCollider boxCollider)
                {
                    boxCollider.Size = size;
                    return true;
                }
                if (actor is AudioVolumeBase audioVolume)
                {
                    audioVolume.BoxSize = size;
                    return true;
                }
                return false;
            }

            public bool CanApply()
            {
                var id = ActorId;
                var actor = FlaxEngine.Object.Find<Actor>(ref id);
                return actor is BoxCollider or AudioVolumeBase;
            }

            public bool HasChanged()
            {
                var id = ActorId;
                var actor = FlaxEngine.Object.Find<Actor>(ref id);
                if (actor is BoxCollider boxCollider)
                    return (Vector3)boxCollider.Size != Before;
                if (actor is AudioVolumeBase audioVolume)
                    return audioVolume.BoxSize != Before;
                return false;
            }
        }

        [Serialize]
        private Entry[] _entries;

        public ResizeBoundsObjectsAction(IReadOnlyList<Entry> entries)
        {
            _entries = new Entry[entries.Count];
            for (int i = 0; i < entries.Count; i++)
            {
                var entry = entries[i];
                var id = entry.ActorId;
                var actor = FlaxEngine.Object.Find<Actor>(ref id);
                if (actor is BoxCollider boxCollider)
                    entry.After = boxCollider.Size;
                else if (actor is AudioVolumeBase audioVolume)
                    entry.After = audioVolume.BoxSize;
                _entries[i] = entry;
            }
        }

        public string ActionString => "Resize object bounds";

        public void Do()
        {
            TryDo();
        }

        public bool TryDo()
        {
            return Apply(false);
        }

        public void Undo()
        {
            TryUndo();
        }

        public bool TryUndo()
        {
            return Apply(true);
        }

        private bool Apply(bool before)
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                if (!_entries[i].CanApply())
                    return false;
            }
            for (int i = 0; i < _entries.Length; i++)
                _entries[i].Apply(before ? _entries[i].Before : _entries[i].After);
            return true;
        }

        public void Dispose()
        {
            _entries = null;
        }

        public void MarkSceneEdited(SceneModule sceneModule)
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                var id = _entries[i].ActorId;
                var actor = FlaxEngine.Object.Find<Actor>(ref id);
                if (actor?.Scene != null)
                    sceneModule.MarkSceneEdited(actor.Scene);
            }
        }
    }
}
