// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Content;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Tools;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.SceneGraph.Actors
{
    /// <summary>
    /// Scene tree node for <see cref="StaticModel"/> actor type.
    /// </summary>
    /// <seealso cref="ActorNode" />
    [HideInEditor]
    public sealed class StaticModelNode : ActorNode
    {
        private Dictionary<IntPtr, Float3[]> _vertices;
        private Dictionary<IntPtr, uint[]> _triangles;
        private Vector3[] _selectionPoints;
        private Transform _selectionPointsTransform;
        private Model _selectionPointsModel;

        /// <summary>
        /// Whether the model of the static model is one of the primitive models (box/sphere/capsule/etc.).
        /// </summary>
        public bool IsPrimitive
        {
            get
            {
                Model model = ((StaticModel)Actor).Model;
                if (!model)
                    return false;
                string path = model.Path;
                return path.EndsWith("/Primitives/Cube.flax", StringComparison.Ordinal) ||
                       path.EndsWith("/Primitives/Sphere.flax", StringComparison.Ordinal) ||
                       path.EndsWith("/Primitives/Plane.flax", StringComparison.Ordinal) ||
                       path.EndsWith("/Primitives/Capsule.flax", StringComparison.Ordinal);
            }
        }

        /// <summary>
        /// Gets the model used by this actor.
        /// </summary>
        public Model Model => ((StaticModel)Actor).Model;

        /// <inheritdoc />
        public StaticModelNode(Actor actor)
        : base(actor)
        {
        }

        private bool TryGetPrimitiveSnapPoints(out Vector3[] points)
        {
            points = null;
            if (!IsPrimitive)
                return false;

            var model = ((StaticModel)Actor).Model;
            if (!model || model.WaitForLoaded())
                return false;

            points = model.GetBox(0).GetCorners();
            var transform = Actor.Transform;
            for (int i = 0; i < points.Length; i++)
                points[i] = transform.LocalToWorld(points[i]);
            return points != null && points.Length != 0;
        }

        /// <inheritdoc />
        public override void OnDispose()
        {
            _vertices = null;
            _triangles = null;
            _selectionPoints = null;
            _selectionPointsModel = null;

            base.OnDispose();
        }

        private bool TryGetCachedMeshData(Mesh mesh, out Float3[] vertices, out uint[] triangles)
        {
            vertices = null;
            triangles = null;
            if (mesh == null)
                return false;

            if (_vertices == null)
                _vertices = new();
            if (_triangles == null)
                _triangles = new();

            var key = FlaxEngine.Object.GetUnmanagedPtr(mesh);
            bool hasVertices = _vertices.TryGetValue(key, out vertices);
            bool hasTriangles = _triangles.TryGetValue(key, out triangles);
            if (hasVertices && hasTriangles)
                return vertices != null && triangles != null;

            var accessor = new MeshAccessor();
            if (accessor.LoadMesh(mesh))
                return false;

            if (!hasVertices)
            {
                vertices = accessor.Positions;
                if (vertices != null)
                    _vertices.Add(key, vertices);
            }
            if (!hasTriangles)
            {
                triangles = accessor.Triangles;
                if (triangles != null)
                    _triangles.Add(key, triangles);
            }
            return vertices != null && triangles != null;
        }

        private static bool TryGetVertex(Float3[] vertices, uint index, out Float3 vertex)
        {
            vertex = new Float3();
            if (vertices == null || index >= (uint)vertices.Length)
                return false;
            vertex = vertices[(int)index];
            return true;
        }

        private static bool IsVertexAtPosition(Float3[] vertices, uint index, Vector3 localVertex, Real toleranceSquared)
        {
            return TryGetVertex(vertices, index, out var vertex) &&
                   Vector3.DistanceSquared(vertex, localVertex) <= toleranceSquared;
        }

        private static void AddStaticModelVertexSnapEdge(List<Vector3> connectedVertices, Transform transform, Float3[] vertices, uint index)
        {
            if (TryGetVertex(vertices, index, out var vertex))
                AddUniqueVertexSnapEdge(connectedVertices, transform.LocalToWorld(vertex));
        }

        /// <inheritdoc />
        public override bool OnVertexSnap(ref Ray ray, Real hitDistance, out Vector3 result)
        {
            result = Vector3.Zero;
            if (TryGetPrimitiveSnapPoints(out var primitivePoints))
                return FindClosestVertexToRay(ref ray, primitivePoints, primitivePoints.Length, out result);

            var model = ((StaticModel)Actor).Model;
            if (model && !model.WaitForLoaded())
            {
                // TODO: move to C++ and use cached vertex buffer internally inside the Mesh
                if (_vertices == null)
                    _vertices = new();
                var transform = Actor.Transform;
                var minDistance = Real.MaxValue;
                var minRayDistance = Real.MaxValue;
                var lodIndex = 0; // TODO: use LOD index based on the game view
                if (model.LODs == null || model.LODs.Length <= lodIndex)
                    return false;
                var lod = model.LODs[lodIndex];
                if (lod.Meshes == null || lod.Meshes.Length == 0)
                    return false;
                {
                    var hit = false;
                    foreach (var mesh in lod.Meshes)
                    {
                        var key = FlaxEngine.Object.GetUnmanagedPtr(mesh);
                        if (!_vertices.TryGetValue(key, out var verts))
                        {
                            var accessor = new MeshAccessor();
                            if (accessor.LoadMesh(mesh))
                                continue;
                            verts = accessor.Positions;
                            if (verts == null)
                                continue;
                            _vertices.Add(key, verts);
                        }
                        for (int i = 0; i < verts.Length; i++)
                        {
                            ref var v = ref verts[i];
                            Vector3 vertex = transform.LocalToWorld(v);
                            if (UpdateClosestVertexToRay(ref ray, vertex, ref minDistance, ref minRayDistance, ref result))
                                hit = true;
                        }
                    }
                    if (hit)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// <inheritdoc />
        public override bool OnVertexSnap(ref Ray ray, Real hitDistance, FlaxEditor.Viewport.EditorViewport viewport, Float2 mousePosition, out Vector3 result, out Real screenDistance)
        {
            result = Vector3.Zero;
            screenDistance = Real.MaxValue;
            if (TryGetPrimitiveSnapPoints(out var primitivePoints))
                return FindClosestVertexToScreen(ref ray, viewport, mousePosition, primitivePoints, primitivePoints.Length, hitDistance, out result, out screenDistance);

            var model = ((StaticModel)Actor).Model;
            if (model && !model.WaitForLoaded())
            {
                // TODO: move to C++ and use cached vertex buffer internally inside the Mesh
                if (_vertices == null)
                    _vertices = new();
                var transform = Actor.Transform;
                var minRayDistance = Real.MaxValue;
                var lodIndex = 0; // TODO: use LOD index based on the game view
                if (model.LODs == null || model.LODs.Length <= lodIndex)
                    return false;
                var lod = model.LODs[lodIndex];
                if (lod.Meshes == null || lod.Meshes.Length == 0)
                    return false;
                {
                    var hit = false;
                    foreach (var mesh in lod.Meshes)
                    {
                        var key = FlaxEngine.Object.GetUnmanagedPtr(mesh);
                        if (!_vertices.TryGetValue(key, out var verts))
                        {
                            var accessor = new MeshAccessor();
                            if (accessor.LoadMesh(mesh))
                                continue;
                            verts = accessor.Positions;
                            if (verts == null)
                                continue;
                            _vertices.Add(key, verts);
                        }
                        for (int i = 0; i < verts.Length; i++)
                        {
                            ref var v = ref verts[i];
                            Vector3 vertex = transform.LocalToWorld(v);
                            if (UpdateClosestVertexToScreen(ref ray, viewport, mousePosition, vertex, hitDistance, ref screenDistance, ref minRayDistance, ref result))
                                hit = true;
                        }
                    }
                    if (hit)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// <inheritdoc />
        public override void OnVertexSnapEdges(Vector3 vertex, List<Vector3> connectedVertices)
        {
            if (TryGetPrimitiveSnapPoints(out var primitivePoints))
            {
                GetBoxVertexSnapEdges(primitivePoints, vertex, connectedVertices);
                return;
            }

            var model = ((StaticModel)Actor).Model;
            if (!model || model.WaitForLoaded())
                return;
            var lodIndex = 0; // TODO: use LOD index based on the game view
            if (model.LODs == null || model.LODs.Length <= lodIndex)
                return;
            var lod = model.LODs[lodIndex];
            if (lod.Meshes == null || lod.Meshes.Length == 0)
                return;

            var transform = Actor.Transform;
            var localVertex = transform.WorldToLocal(vertex);
            Real closestDistance = Real.MaxValue;
            foreach (var mesh in lod.Meshes)
            {
                if (!TryGetCachedMeshData(mesh, out var vertices, out _))
                    continue;
                for (int i = 0; i < vertices.Length; i++)
                {
                    var distance = Vector3.DistanceSquared(vertices[i], localVertex);
                    if (distance < closestDistance)
                        closestDistance = distance;
                }
            }
            if (closestDistance == Real.MaxValue)
                return;

            var toleranceSquared = closestDistance + (Real)0.0001;
            if (toleranceSquared < (Real)0.0001)
                toleranceSquared = (Real)0.0001;

            foreach (var mesh in lod.Meshes)
            {
                if (!TryGetCachedMeshData(mesh, out var vertices, out var triangles))
                    continue;
                for (int i = 0; i + 2 < triangles.Length; i += 3)
                {
                    var a = triangles[i];
                    var b = triangles[i + 1];
                    var c = triangles[i + 2];
                    bool aSelected = IsVertexAtPosition(vertices, a, localVertex, toleranceSquared);
                    bool bSelected = IsVertexAtPosition(vertices, b, localVertex, toleranceSquared);
                    bool cSelected = IsVertexAtPosition(vertices, c, localVertex, toleranceSquared);
                    if (aSelected)
                    {
                        AddStaticModelVertexSnapEdge(connectedVertices, transform, vertices, b);
                        AddStaticModelVertexSnapEdge(connectedVertices, transform, vertices, c);
                    }
                    if (bSelected)
                    {
                        AddStaticModelVertexSnapEdge(connectedVertices, transform, vertices, a);
                        AddStaticModelVertexSnapEdge(connectedVertices, transform, vertices, c);
                    }
                    if (cSelected)
                    {
                        AddStaticModelVertexSnapEdge(connectedVertices, transform, vertices, a);
                        AddStaticModelVertexSnapEdge(connectedVertices, transform, vertices, b);
                    }
                }
            }
        }

        /// <inheritdoc />
        public override void OnContextMenu(ContextMenu contextMenu, EditorWindow window)
        {
            base.OnContextMenu(contextMenu, window);

            // Check if every selected node is a primitive or has collision asset
            var selection = GetSelection(window);
            bool autoOptionEnabled = true;
            foreach (var node in selection)
            {
                if (node is StaticModelNode staticModelNode && (!staticModelNode.IsPrimitive && GetCollisionData(staticModelNode.Model) == null))
                {
                    autoOptionEnabled = false;
                    break;
                }
            }

            var menu = contextMenu.AddChildMenu("Add collider");
            menu.Enabled = ((StaticModel)Actor).Model != null;
            var b = menu.ContextMenu.AddButton("Auto", () => OnAddCollider(window, CreateAuto));
            b.TooltipText = "Add the best fitting collider to every model that uses an in-built Editor primitive.";
            b.Enabled = autoOptionEnabled;
            b = menu.ContextMenu.AddButton("Box", () => OnAddCollider(window, CreateBox));
            b.TooltipText = "Add a box collider to every selected model that will auto resize based on the model bounds.";
            b = menu.ContextMenu.AddButton("Sphere", () => OnAddCollider(window, CreateSphere));
            b.TooltipText = "Add a sphere collider to every selected model that will auto resize based on the model bounds.";
            b = menu.ContextMenu.AddButton("Capsule", () => OnAddCollider(window, CreateCapsule));
            b.TooltipText = "Add a capsule collider to every selected model that will auto resize based on the model bounds.";
            b = menu.ContextMenu.AddButton("Convex", () => OnAddCollider(window, CreateConvex));
            b.TooltipText = "Generate and add a convex collider for every selected model.";
            b = menu.ContextMenu.AddButton("Triangle Mesh", () => OnAddCollider(window, CreateTriangle));
            b.TooltipText = "Generate and add a triangle mesh collider for every selected model.";

            var bakeBtn = contextMenu.AddButton("Bake Scale to New Model", () => OnBakeScale(window));
            bakeBtn.TooltipText = "Bakes the actor's scale directly into the mesh geometry, saves a new Model asset in Content/SceneData/<Scene>/Models/Bakes/, generates a fresh 1:1 SDF, and resets the actor scale to 1.";
            bakeBtn.Enabled = ((StaticModel)Actor).Model != null;
        }

        /// <inheritdoc />
        public override Vector3[] GetActorSelectionPoints()
        {
            if (Actor is StaticModel sm && sm.Model)
            {
                // Try to use cache
                var model = sm.Model;
                var transform = Actor.Transform;
                if (_selectionPoints != null &&
                    _selectionPointsTransform == transform &&
                    _selectionPointsModel == model)
                    return _selectionPoints;
                Profiler.BeginEvent("GetActorSelectionPoints");

                // Check collision proxy points for more accurate selection
                var vecPoints = new List<Vector3>();
                var m = model.LODs[0];
                foreach (var mesh in m.Meshes)
                {
                    var points = mesh.GetCollisionProxyPoints();
                    vecPoints.EnsureCapacity(vecPoints.Count + points.Length);
                    for (int i = 0; i < points.Length; i++)
                    {
                        vecPoints.Add(transform.LocalToWorld(points[i]));
                    }
                }

                Profiler.EndEvent();
                if (vecPoints.Count != 0)
                {
                    _selectionPoints = vecPoints.ToArray();
                    _selectionPointsTransform = transform;
                    _selectionPointsModel = model;
                    return _selectionPoints;
                }
            }
            return base.GetActorSelectionPoints();
        }

        private delegate void Spawner(Collider collider);

        private delegate void CreateCollider(StaticModel actor, Spawner spawner, bool singleNode);

        private IEnumerable<SceneGraphNode> GetSelection(EditorWindow window)
        {
            if (window is SceneTreeWindow)
                return Editor.Instance.SceneEditing.Selection;
            if (window is PrefabWindow prefabWindow)
                return prefabWindow.Selection;
            return Array.Empty<SceneGraphNode>();
        }

        private static bool TryCollisionData(Model model, BinaryAssetItem assetItem, out CollisionData collisionData)
        {
            collisionData = FlaxEngine.Content.LoadAsset<CollisionData>(assetItem.ObjectID);
            if (collisionData)
            {
                var options = collisionData.Options;
                if (options.Model == model.ID || options.Model == Guid.Empty)
                    return true;
            }
            return false;
        }

        private CollisionData GetCollisionData(Model model)
        {
            if (model == null)
                return null;

            // Check if there already is collision data for that model to reuse
            var modelItem = (AssetItem)Editor.Instance.ContentDatabase.Find(model.ID);
            if (modelItem?.ParentFolder != null)
            {
                foreach (var child in modelItem.ParentFolder.Children)
                {
                    // Check if there is collision that was made with this model
                    if (child is BinaryAssetItem b && b.IsOfType<CollisionData>())
                    {
                        if (TryCollisionData(model, b, out var collisionData))
                            return collisionData;
                    }

                    // Check if there is an auto-imported collision
                    if (child is ContentFolder childFolder && childFolder.ShortName == modelItem.ShortName)
                    {
                        foreach (var childFolderChild in childFolder.Children)
                        {
                            if (childFolderChild is BinaryAssetItem c && c.IsOfType<CollisionData>())
                            {
                                if (TryCollisionData(model, c, out var collisionData))
                                    return collisionData;
                            }
                        }
                    }
                }
            }

            return null;
        }

        private void CreateAuto(StaticModel actor, Spawner spawner, bool singleNode)
        {
            // Special case for in-built Editor models that can use analytical collision
            Model model = actor.Model;
            var modelPath = model.Path;
            if (modelPath.EndsWith("/Primitives/Cube.flax", StringComparison.Ordinal))
            {
                var collider = new BoxCollider
                {
                    Transform = actor.Transform,
                };
                spawner(collider);
            }
            else if (modelPath.EndsWith("/Primitives/Sphere.flax", StringComparison.Ordinal))
            {
                var collider = new SphereCollider
                {
                    Transform = actor.Transform,
                };
                spawner(collider);
                collider.LocalTransform = Transform.Identity;
            }
            else if (modelPath.EndsWith("/Primitives/Plane.flax", StringComparison.Ordinal))
            {
                spawner(new BoxCollider
                {
                    Transform = actor.Transform,
                    Size = new Float3(100.0f, 100.0f, 1.0f),
                });
            }
            else if (modelPath.EndsWith("/Primitives/Capsule.flax", StringComparison.Ordinal))
            {
                var collider = new CapsuleCollider
                {
                    Transform = actor.Transform,
                    Radius = 25.0f,
                    Height = 50.0f,
                };
                spawner(collider);
                collider.LocalPosition = new Vector3(0, 50.0f, 0);
                collider.LocalOrientation = Quaternion.Euler(0, 0, 90.0f);
            }
            else
            {
                var collider = new MeshCollider
                {
                    Transform = actor.Transform,
                    CollisionData = GetCollisionData(model),
                };
                spawner(collider);
            }
        }

        private void CreateBox(StaticModel actor, Spawner spawner, bool singleNode)
        {
            var collider = new BoxCollider
            {
                Transform = actor.Transform,
            };
            spawner(collider);
            // BoxColliderNode fits the box collider automatically on spawn
        }

        private void CreateSphere(StaticModel actor, Spawner spawner, bool singleNode)
        {
            var bounds = actor.Sphere;
            var collider = new SphereCollider
            {
                Transform = actor.Transform,

                // Refit into the sphere bounds that are usually calculated from mesh box bounds
                Position = bounds.Center,
                Radius = (float)bounds.Radius / Mathf.Max((float)actor.Scale.MaxValue, 0.0001f) * 0.707f,
            };
            spawner(collider);
        }

        private void CreateCapsule(StaticModel actor, Spawner spawner, bool singleNode)
        {
            var collider = new CapsuleCollider
            {
                Transform = actor.Transform,
                Position = actor.Box.Center,

                // Size the capsule to best fit the actor
                Radius = (float)actor.Sphere.Radius / Mathf.Max((float)actor.Scale.MaxValue, 0.0001f) * 0.707f,
                Height = 100f,
            };
            spawner(collider);
        }

        private void CreateConvex(StaticModel actor, Spawner spawner, bool singleNode)
        {
            CreateMeshCollider(actor, spawner, singleNode, CollisionDataType.ConvexMesh);
        }

        private void CreateTriangle(StaticModel actor, Spawner spawner, bool singleNode)
        {
            CreateMeshCollider(actor, spawner, singleNode, CollisionDataType.TriangleMesh);
        }

        private void CreateMeshCollider(StaticModel actor, Spawner spawner, bool singleNode, CollisionDataType type)
        {
            // Create collision data (or reuse) and add collision actor
            var created = (CollisionData collisionData) =>
            {
                var collider = new MeshCollider
                {
                    Transform = actor.Transform,
                    CollisionData = collisionData,
                };
                spawner(collider);
            };
            var collisionDataProxy = (CollisionDataProxy)Editor.Instance.ContentDatabase.GetProxy<CollisionData>();
            collisionDataProxy.CreateCollisionDataFromModel(actor.Model, created, singleNode, false, type);
        }

        private void OnAddCollider(EditorWindow window, CreateCollider createCollider)
        {
            // Allow collider to be added to every static model selection
            var selection = GetSelection(window).ToArray();
            var createdNodes = new List<SceneGraphNode>();
            foreach (var node in selection)
            {
                if (node is not StaticModelNode staticModelNode)
                    continue;
                var actor = (StaticModel)staticModelNode.Actor;
                var model = ((StaticModel)staticModelNode.Actor).Model;
                if (!model)
                    continue;
                Spawner spawner = collider =>
                {
                    collider.StaticFlags = staticModelNode.Actor.StaticFlags;
                    staticModelNode.Root.Spawn(collider, staticModelNode.Actor);
                    var colliderNode = window is PrefabWindow prefabWindow ? prefabWindow.Graph.Root.Find(collider) : Editor.Instance.Scene.GetActorNode(collider);
                    createdNodes.Add(colliderNode);
                };

                createCollider(actor, spawner, selection.Length == 1);
            }

            // Select all created nodes
            if (window is SceneTreeWindow)
            {
                Editor.Instance.SceneEditing.Select(createdNodes);
            }
            else if (window is PrefabWindow prefabWindow)
            {
                prefabWindow.Select(createdNodes);
            }
        }

        private void OnBakeScale(EditorWindow window)
        {
            var selection = GetSelection(window).ToArray();
            if (selection.Length == 0)
            {
                if (Actor is StaticModel staticModel)
                    ModelTransformBaker.BakeScale(staticModel);
                return;
            }

            foreach (var node in selection)
            {
                if (node is StaticModelNode staticModelNode && staticModelNode.Actor is StaticModel staticModel)
                {
                    ModelTransformBaker.BakeScale(staticModel);
                }
            }
        }
    }
}
