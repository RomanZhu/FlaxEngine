// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.SceneGraph
{
    /// <summary>
    /// Stable reference to a scene graph node that does not retain the live node across Scene unload.
    /// </summary>
    [Serializable]
    public struct SceneGraphNodeReference : IEquatable<SceneGraphNodeReference>
    {
        /// <summary>
        /// Owning Scene identifier, or empty for a local editor graph.
        /// </summary>
        public Guid SceneId;

        /// <summary>
        /// Scene graph node identifier.
        /// </summary>
        public Guid NodeId;

        /// <summary>
        /// Last known display name, used only for diagnostics and metadata.
        /// </summary>
        public string Name;

        /// <summary>
        /// Captures a stable reference from a live node.
        /// </summary>
        public static SceneGraphNodeReference Capture(SceneGraphNode node)
        {
            return new SceneGraphNodeReference
            {
                SceneId = node?.ParentScene?.Scene?.ID ?? Guid.Empty,
                NodeId = node?.ID ?? Guid.Empty,
                Name = node?.Name,
            };
        }

        /// <summary>
        /// Captures stable references from live nodes.
        /// </summary>
        public static SceneGraphNodeReference[] Capture(SceneGraphNode[] nodes)
        {
            if (nodes == null || nodes.Length == 0)
                return Array.Empty<SceneGraphNodeReference>();
            var result = new SceneGraphNodeReference[nodes.Length];
            for (int i = 0; i < nodes.Length; i++)
                result[i] = Capture(nodes[i]);
            return result;
        }

        /// <summary>
        /// Resolves the current live node, if its owning Scene and object are available.
        /// </summary>
        public SceneGraphNode Resolve()
        {
            if (NodeId == Guid.Empty)
                return null;
            if (SceneId != Guid.Empty && Level.FindScene(SceneId) == null)
                return null;
            var node = SceneGraphFactory.FindNode(NodeId);
            if (node == null)
                return null;
            if (SceneId != Guid.Empty && node.ParentScene?.Scene?.ID != SceneId)
                return null;
            return node;
        }

        /// <summary>
        /// Resolves all currently available nodes, filtering unavailable objects.
        /// </summary>
        public static SceneGraphNode[] Resolve(SceneGraphNodeReference[] references)
        {
            if (references == null || references.Length == 0)
                return Array.Empty<SceneGraphNode>();
            var result = new SceneGraphNode[references.Length];
            var count = 0;
            for (int i = 0; i < references.Length; i++)
            {
                var node = references[i].Resolve();
                if (node != null)
                    result[count++] = node;
            }
            if (count == result.Length)
                return result;
            Array.Resize(ref result, count);
            return result;
        }

        /// <inheritdoc />
        public bool Equals(SceneGraphNodeReference other)
        {
            return SceneId == other.SceneId && NodeId == other.NodeId;
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            return obj is SceneGraphNodeReference other && Equals(other);
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            unchecked
            {
                return (SceneId.GetHashCode() * 397) ^ NodeId.GetHashCode();
            }
        }
    }
}
