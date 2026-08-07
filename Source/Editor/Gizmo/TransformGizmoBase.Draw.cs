// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using System.Globalization;
using FlaxEditor.Options;
using FlaxEditor.SceneGraph;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        // Models
        private Model _modelTranslationAxis;
        private Model _modelScaleAxis;
        private Model _modelRotationArc;
        private Model _modelRotationTrackballArc;
        private Model _modelRotationSphere;
        private Model _modelRotationScreenRing;
        private Model _modelSphere;
        private Model _modelCube;

        // Materials
        private MaterialInstance _materialAxisX;
        private MaterialInstance _materialAxisY;
        private MaterialInstance _materialAxisZ;
        private MaterialInstance _materialAxisFocus;
        private MaterialInstance _materialAxisBack;
        private MaterialInstance _materialTrackballFocus;
        private MaterialInstance _materialVertexSnapPoint;
        private MaterialInstance _materialVertexSnapTargetPoint;
        private MaterialInstance _materialVertexSnapPointShadow;
        private MaterialBase _materialSphere;
        private readonly List<Vector3> _vertexSnapEdgePoints = new List<Vector3>();

        // Material Parameter Names
        private const string _brightnessParamName = "Brightness";
        private const string _opacityParamName = "Opacity";
        private const string _colorParamName = "Color";
        private const int _rotationArcSegments = 48;
        private const int _rotationTubeSegments = 8;
        private const float _rotationHandleThicknessRaw = 0.08f;
        private const float _rotationScreenRingRadiusRaw = RotateRadiusRaw + _rotationHandleThicknessRaw + 0.20f;
        private const float _rotationScreenRingThicknessRaw = 0.045f;
        private const float _rotationSphereRadiusRaw = RotateRadiusRaw - _rotationHandleThicknessRaw * 1.5f;
        private const float _rotationSphereOpacity = 0.16f;
        private const float _rotationTrackballRadiusRaw = _rotationScreenRingRadiusRaw - _rotationScreenRingThicknessRaw;
        private const float _rotationTrackballOpacity = 0.50f;
        private const float _planeHandleCenterRaw = 0.085f * 300.0f * 0.075f;
        private const float _planeHandleHalfSizeRaw = 0.085f * 50.0f * 0.075f;
        private const float _vertexSnapPointOuterScale = 0.0018f;
        private const float _vertexSnapPointInnerScale = 0.0011f;
        private const int _vertexSnapEdgeSegments = 10;
        private const float _vertexSnapEdgeThickness = 2.0f;
        private const float _axisShaftRadiusRaw = 0.055f;
        private const float _axisArrowHeadRadiusRaw = 0.28f;
        private const int _axisRadialSegments = 8;
        private static readonly Color _translationDistanceColor = new Color(1.0f, 0.8980392f, 0.039215688f, 1.0f);
        private static readonly Color _translationDistancePillColor = new Color(0.0f, 0.0f, 0.0f, 0.68f);
        private static readonly Color _vertexSnapEdgeColor = new Color(0.0f, 0.72f, 1.0f, 1.0f);

        /// <summary>
        /// Used for example when the selection can't be moved because one actor is static.
        /// </summary>
        private bool _isDisabled;

        private void InitDrawing()
        {
            // Axis Models
            _modelTranslationAxis = CreateArrowModel();
            _modelScaleAxis = CreateScaleAxisModel();
            _modelSphere = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Primitives/Sphere");
            _modelCube = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Primitives/Cube");

            // Axis Materials
            _materialAxisX = FlaxEngine.Content.LoadAsyncInternal<MaterialInstance>("Editor/Gizmo/MaterialAxisX");
            _materialAxisY = FlaxEngine.Content.LoadAsyncInternal<MaterialInstance>("Editor/Gizmo/MaterialAxisY");
            _materialAxisZ = FlaxEngine.Content.LoadAsyncInternal<MaterialInstance>("Editor/Gizmo/MaterialAxisZ");
            _materialAxisFocus = FlaxEngine.Content.LoadAsyncInternal<MaterialInstance>("Editor/Gizmo/MaterialAxisFocus");
            _materialSphere = FlaxEngine.Content.LoadAsyncInternal<MaterialInstance>("Editor/Gizmo/MaterialSphere");

            // Ensure that every asset was loaded
            if (_modelTranslationAxis == null ||
                _modelScaleAxis == null ||
                _modelSphere == null ||
                _modelCube == null ||
                _materialAxisX == null ||
                _materialAxisY == null ||
                _materialAxisZ == null ||
                _materialAxisFocus == null ||
                _materialSphere == null)
            {
                Platform.Fatal("Failed to load transform gizmo resources.");
            }

            _modelRotationArc = CreateTorusSegmentModel(RotateRadiusRaw, _rotationHandleThicknessRaw, -Mathf.PiOverTwo, Mathf.Pi, _rotationArcSegments, _rotationTubeSegments);
            _modelRotationTrackballArc = CreateTorusSegmentModel(RotateRadiusRaw, _rotationHandleThicknessRaw, 0.0f, Mathf.TwoPi, _rotationArcSegments * 2, _rotationTubeSegments);
            _modelRotationSphere = CreateSphereModel(_rotationSphereRadiusRaw, _rotationArcSegments, _rotationArcSegments / 2);
            _modelRotationScreenRing = CreateTorusSegmentModel(_rotationScreenRingRadiusRaw, _rotationScreenRingThicknessRaw, 0.0f, Mathf.TwoPi, _rotationArcSegments, _rotationTubeSegments);
            _materialAxisBack = _materialAxisX.CreateVirtualInstance();
            _materialAxisBack.SetParameterValue(_colorParamName, new Color(0.42f, 0.42f, 0.42f, 1.0f));
            _materialTrackballFocus = _materialAxisFocus.CreateVirtualInstance();
            _materialTrackballFocus.SetParameterValue(_opacityParamName, _rotationTrackballOpacity);
            _materialVertexSnapPoint = _materialAxisFocus.CreateVirtualInstance();
            _materialVertexSnapPoint.SetParameterValue(_colorParamName, new Color(0.0f, 0.95f, 1.0f, 1.0f));
            _materialVertexSnapTargetPoint = _materialAxisFocus.CreateVirtualInstance();
            _materialVertexSnapTargetPoint.SetParameterValue(_colorParamName, new Color(1.0f, 0.10f, 0.82f, 1.0f));
            _materialVertexSnapPointShadow = _materialAxisX.CreateVirtualInstance();
            _materialVertexSnapPointShadow.SetParameterValue(_colorParamName, new Color(0.0f, 0.0f, 0.0f, 1.0f));

            // Setup editor options
            OnEditorOptionsChanged(Editor.Instance.Options.Options);
            Editor.Instance.Options.OptionsChanged += OnEditorOptionsChanged;
        }

        private static Model CreateArrowModel()
        {
            // The model points down local -Z to preserve the orientation of the
            // original gizmo assets. Its dimensions are authored directly in
            // gizmo space so the visible and semantic target sizes share the
            // same AxisLength/AxisArrowHeadLength constants.
            var model = FlaxEngine.Content.CreateVirtualAsset<Model>();
            model.SetupLODs(new[] { 1 });

            int sides = _axisRadialSegments;
            int shaftStart = 0;
            int shaftEnd = sides;
            int headBase = sides * 2;
            int tip = sides * 3;
            int startCap = tip + 1;
            var vertices = new Float3[startCap + 1];
            var normals = new Float3[vertices.Length];
            float shaftEndZ = -(AxisLength - AxisArrowHeadLength);

            for (int i = 0; i < sides; i++)
            {
                float angle = Mathf.TwoPi * i / sides;
                float x = Mathf.Cos(angle);
                float y = Mathf.Sin(angle);
                var radial = new Float3(x, y, 0.0f);
                vertices[shaftStart + i] = new Float3(x * _axisShaftRadiusRaw, y * _axisShaftRadiusRaw, -AxisVisualStart);
                vertices[shaftEnd + i] = new Float3(x * _axisShaftRadiusRaw, y * _axisShaftRadiusRaw, shaftEndZ);
                vertices[headBase + i] = new Float3(x * _axisArrowHeadRadiusRaw, y * _axisArrowHeadRadiusRaw, shaftEndZ);
                normals[shaftStart + i] = radial;
                normals[shaftEnd + i] = radial;
                normals[headBase + i] = Float3.Normalize(new Float3(x, y, -_axisArrowHeadRadiusRaw / AxisArrowHeadLength));
            }
            vertices[tip] = new Float3(0.0f, 0.0f, -AxisLength);
            normals[tip] = Float3.Backward;
            vertices[startCap] = new Float3(0.0f, 0.0f, -AxisVisualStart);
            normals[startCap] = Float3.Forward;

            // Shaft quads, shoulder, pointed head, and the small inner cap.
            var indices = new int[sides * 18];
            int index = 0;
            for (int i = 0; i < sides; i++)
            {
                int next = (i + 1) % sides;
                indices[index++] = shaftStart + i;
                indices[index++] = shaftEnd + i;
                indices[index++] = shaftEnd + next;
                indices[index++] = shaftStart + i;
                indices[index++] = shaftEnd + next;
                indices[index++] = shaftStart + next;

                indices[index++] = shaftEnd + i;
                indices[index++] = headBase + i;
                indices[index++] = headBase + next;
                indices[index++] = shaftEnd + i;
                indices[index++] = headBase + next;
                indices[index++] = shaftEnd + next;

                indices[index++] = headBase + i;
                indices[index++] = tip;
                indices[index++] = headBase + next;

                indices[index++] = startCap;
                indices[index++] = shaftStart + i;
                indices[index++] = shaftStart + next;
            }

            model.LODs[0].Meshes[0].UpdateMesh(vertices, indices, normals);
            return model;
        }

        private static Model CreateScaleAxisModel()
        {
            // Scale uses the same thin shaft as Translate, but terminates in a
            // compact cube so the operation remains identifiable at a glance.
            var model = FlaxEngine.Content.CreateVirtualAsset<Model>();
            model.SetupLODs(new[] { 1 });

            int sides = _axisRadialSegments;
            int shaftStart = 0;
            int shaftEnd = sides;
            int startCap = sides * 2;
            int cubeStart = startCap + 1;
            var vertices = new Float3[cubeStart + 24];
            var normals = new Float3[vertices.Length];
            float half = AxisScaleCubeSize * 0.5f;
            float cubeNearZ = -(AxisLength - half);
            float cubeFarZ = -(AxisLength + half);

            for (int i = 0; i < sides; i++)
            {
                float angle = Mathf.TwoPi * i / sides;
                float x = Mathf.Cos(angle);
                float y = Mathf.Sin(angle);
                var radial = new Float3(x, y, 0.0f);
                vertices[shaftStart + i] = new Float3(x * _axisShaftRadiusRaw, y * _axisShaftRadiusRaw, -AxisVisualStart);
                vertices[shaftEnd + i] = new Float3(x * _axisShaftRadiusRaw, y * _axisShaftRadiusRaw, cubeNearZ);
                normals[shaftStart + i] = radial;
                normals[shaftEnd + i] = radial;
            }
            vertices[startCap] = new Float3(0.0f, 0.0f, -AxisVisualStart);
            normals[startCap] = Float3.Forward;

            int vertex = cubeStart;
            AddScaleCubeFace(vertices, normals, ref vertex, new Float3(-half, -half, cubeNearZ), new Float3(half, -half, cubeNearZ), new Float3(half, half, cubeNearZ), new Float3(-half, half, cubeNearZ), Float3.Forward);
            AddScaleCubeFace(vertices, normals, ref vertex, new Float3(-half, -half, cubeFarZ), new Float3(-half, half, cubeFarZ), new Float3(half, half, cubeFarZ), new Float3(half, -half, cubeFarZ), Float3.Backward);
            AddScaleCubeFace(vertices, normals, ref vertex, new Float3(half, -half, cubeNearZ), new Float3(half, -half, cubeFarZ), new Float3(half, half, cubeFarZ), new Float3(half, half, cubeNearZ), Float3.Right);
            AddScaleCubeFace(vertices, normals, ref vertex, new Float3(-half, -half, cubeNearZ), new Float3(-half, half, cubeNearZ), new Float3(-half, half, cubeFarZ), new Float3(-half, -half, cubeFarZ), Float3.Left);
            AddScaleCubeFace(vertices, normals, ref vertex, new Float3(-half, half, cubeNearZ), new Float3(half, half, cubeNearZ), new Float3(half, half, cubeFarZ), new Float3(-half, half, cubeFarZ), Float3.Up);
            AddScaleCubeFace(vertices, normals, ref vertex, new Float3(-half, -half, cubeNearZ), new Float3(-half, -half, cubeFarZ), new Float3(half, -half, cubeFarZ), new Float3(half, -half, cubeNearZ), Float3.Down);

            var indices = new int[sides * 9 + 36];
            int index = 0;
            for (int i = 0; i < sides; i++)
            {
                int next = (i + 1) % sides;
                indices[index++] = shaftStart + i;
                indices[index++] = shaftEnd + i;
                indices[index++] = shaftEnd + next;
                indices[index++] = shaftStart + i;
                indices[index++] = shaftEnd + next;
                indices[index++] = shaftStart + next;
                indices[index++] = startCap;
                indices[index++] = shaftStart + i;
                indices[index++] = shaftStart + next;
            }
            for (int face = 0; face < 6; face++)
            {
                int faceStart = cubeStart + face * 4;
                indices[index++] = faceStart;
                indices[index++] = faceStart + 1;
                indices[index++] = faceStart + 2;
                indices[index++] = faceStart;
                indices[index++] = faceStart + 2;
                indices[index++] = faceStart + 3;
            }

            model.LODs[0].Meshes[0].UpdateMesh(vertices, indices, normals);
            return model;
        }

        private static void AddScaleCubeFace(Float3[] vertices, Float3[] normals, ref int vertex, Float3 a, Float3 b, Float3 c, Float3 d, Float3 normal)
        {
            vertices[vertex] = a;
            normals[vertex++] = normal;
            vertices[vertex] = b;
            normals[vertex++] = normal;
            vertices[vertex] = c;
            normals[vertex++] = normal;
            vertices[vertex] = d;
            normals[vertex++] = normal;
        }

        private static Model CreateTorusSegmentModel(float radius, float tubeRadius, float startAngle, float angle, int arcSegments, int tubeSegments)
        {
            var model = FlaxEngine.Content.CreateVirtualAsset<Model>();
            model.SetupLODs(new[] { 1 });

            int rings = arcSegments + 1;
            var vertices = new Float3[rings * tubeSegments];
            var normals = new Float3[vertices.Length];
            var indices = new int[arcSegments * tubeSegments * 6];
            int vertexIndex = 0;
            for (int i = 0; i < rings; i++)
            {
                float arcAlpha = (float)i / arcSegments;
                float theta = startAngle + angle * arcAlpha;
                var radial = new Float3(Mathf.Sin(theta), 0.0f, Mathf.Cos(theta));
                for (int j = 0; j < tubeSegments; j++)
                {
                    float tubeAlpha = (float)j / tubeSegments;
                    float phi = Mathf.TwoPi * tubeAlpha;
                    var normal = radial * Mathf.Cos(phi) + Float3.Up * Mathf.Sin(phi);
                    vertices[vertexIndex] = radial * (radius + tubeRadius * Mathf.Cos(phi)) + Float3.Up * (tubeRadius * Mathf.Sin(phi));
                    normals[vertexIndex] = normal;
                    vertexIndex++;
                }
            }

            int index = 0;
            for (int i = 0; i < arcSegments; i++)
            {
                int nextI = i + 1;
                for (int j = 0; j < tubeSegments; j++)
                {
                    int nextJ = (j + 1) % tubeSegments;
                    int a = i * tubeSegments + j;
                    int b = nextI * tubeSegments + j;
                    int c = nextI * tubeSegments + nextJ;
                    int d = i * tubeSegments + nextJ;
                    indices[index++] = a;
                    indices[index++] = b;
                    indices[index++] = c;
                    indices[index++] = a;
                    indices[index++] = c;
                    indices[index++] = d;
                }
            }

            model.LODs[0].Meshes[0].UpdateMesh(vertices, indices, normals);
            return model;
        }

        private static Model CreateSphereModel(float radius, int slices, int stacks)
        {
            var model = FlaxEngine.Content.CreateVirtualAsset<Model>();
            model.SetupLODs(new[] { 1 });

            int sliceVertices = slices + 1;
            var vertices = new Float3[(stacks + 1) * sliceVertices];
            var normals = new Float3[vertices.Length];
            int vertexIndex = 0;
            for (int stack = 0; stack <= stacks; stack++)
            {
                float stackAlpha = (float)stack / stacks;
                float phi = -Mathf.PiOverTwo + Mathf.Pi * stackAlpha;
                float y = Mathf.Sin(phi);
                float ringRadius = Mathf.Cos(phi);
                for (int slice = 0; slice <= slices; slice++)
                {
                    float theta = Mathf.TwoPi * slice / slices;
                    var normal = new Float3(Mathf.Sin(theta) * ringRadius, y, Mathf.Cos(theta) * ringRadius);
                    vertices[vertexIndex] = normal * radius;
                    normals[vertexIndex] = normal;
                    vertexIndex++;
                }
            }

            var indices = new int[stacks * slices * 6];
            int index = 0;
            for (int stack = 0; stack < stacks; stack++)
            {
                for (int slice = 0; slice < slices; slice++)
                {
                    int a = stack * sliceVertices + slice;
                    int b = (stack + 1) * sliceVertices + slice;
                    int c = (stack + 1) * sliceVertices + slice + 1;
                    int d = stack * sliceVertices + slice + 1;
                    indices[index++] = a;
                    indices[index++] = b;
                    indices[index++] = c;
                    indices[index++] = a;
                    indices[index++] = c;
                    indices[index++] = d;
                }
            }

            model.LODs[0].Meshes[0].UpdateMesh(vertices, indices, normals);
            return model;
        }

        private void OnEditorOptionsChanged(EditorOptions options)
        {
            UpdateGizmoBrightness(options);

            float opacity = options.Visual.TransformGizmoOpacity;
            _materialAxisX.SetParameterValue(_opacityParamName, opacity);
            _materialAxisY.SetParameterValue(_opacityParamName, opacity);
            _materialAxisZ.SetParameterValue(_opacityParamName, opacity);
            _materialAxisBack.SetParameterValue(_opacityParamName, opacity);
            _materialTrackballFocus.SetParameterValue(_opacityParamName, opacity * _rotationTrackballOpacity);
            _materialVertexSnapPoint.SetParameterValue(_opacityParamName, opacity);
            _materialVertexSnapTargetPoint.SetParameterValue(_opacityParamName, opacity);
            _materialVertexSnapPointShadow.SetParameterValue(_opacityParamName, opacity * 0.75f);
        }

        private void UpdateGizmoBrightness(EditorOptions options)
        {
            _isDisabled = ShouldGizmoBeLocked();

            float brightness = _isDisabled ? options.Visual.TransformGizmoBrightnessDisabled : options.Visual.TransformGizmoBrightness;
            var currentValue = _materialAxisX.GetParameterValue(_brightnessParamName);
            if (currentValue is not float currentValueFloat || Mathf.NearEqual(brightness, currentValueFloat))
                return;
            _materialAxisX.SetParameterValue(_brightnessParamName, brightness);
            _materialAxisY.SetParameterValue(_brightnessParamName, brightness);
            _materialAxisZ.SetParameterValue(_brightnessParamName, brightness);
            _materialAxisBack.SetParameterValue(_brightnessParamName, brightness);
            _materialVertexSnapPoint.SetParameterValue(_brightnessParamName, brightness);
            _materialVertexSnapTargetPoint.SetParameterValue(_brightnessParamName, brightness);
            _materialVertexSnapPointShadow.SetParameterValue(_brightnessParamName, brightness);
        }

        private bool ShouldGizmoBeLocked()
        {
            bool gizmoLocked = false;
            if (Editor.Instance.StateMachine.IsPlayMode && Owner is Viewport.EditorGizmoViewport)
            {
                // Block editing static scene objects in main view during play mode
                foreach (var obj in Editor.Instance.SceneEditing.Selection)
                {
                    if (obj.CanTransform == false)
                    {
                        gizmoLocked = true;
                        break;
                    }
                }
            }
            return gizmoLocked;
        }

        private Vector3 GetRotateToViewLocal(ref Transform transform)
        {
            Vector3 toView;
            if (Owner.Viewport.UseOrthographicProjection)
            {
                var viewDirection = (Vector3)Owner.ViewDirection;
                transform.WorldToLocalVector(ref viewDirection, out toView);
                toView = -toView;
            }
            else
            {
                var viewPosition = Owner.ViewPosition;
                transform.WorldToLocal(ref viewPosition, out toView);
            }

            if (toView.LengthSquared < 0.0001f)
                toView = Vector3.Forward;
            else
                toView.Normalize();
            return toView;
        }

        private Vector3 GetRotateToViewLocal()
        {
            var transform = _gizmoWorld;
            return GetRotateToViewLocal(ref transform);
        }

        private Vector3 GetRotateFrontDirectionLocal(ref Transform transform, Vector3 normal)
        {
            Vector3 toView = GetRotateToViewLocal(ref transform);
            toView = Vector3.ProjectOnPlane(toView, normal);
            if (toView.LengthSquared < 0.0001f)
            {
                toView = Vector3.Cross(normal, Vector3.Forward);
                if (toView.LengthSquared < 0.0001f)
                    toView = Vector3.Cross(normal, Vector3.Right);
            }
            toView.Normalize();
            return toView;
        }

        private Vector3 GetRotateFrontDirectionLocal(Vector3 normal)
        {
            var transform = _gizmoWorld;
            return GetRotateFrontDirectionLocal(ref transform, normal);
        }

        private static void DrawGizmoMesh(ref RenderContext renderContext, Mesh mesh, MaterialBase material, ref Matrix world, sbyte sortOrder)
        {
            mesh.Draw(ref renderContext, material, ref world, StaticFlags.None, true, DrawPass.Depth | DrawPass.Forward, 0.0f, sortOrder, 0, true);
        }

        private static void DrawGizmoOverlayMesh(ref RenderContext renderContext, Mesh mesh, MaterialBase material, ref Matrix world, sbyte sortOrder)
        {
            mesh.Draw(ref renderContext, material, ref world, StaticFlags.None, true, DrawPass.Forward, 0.0f, sortOrder);
        }

        private void DrawRotationAxis(ref RenderContext renderContext, Mesh arcMesh, ref Transform transform, ref Matrix world, Vector3 normal, MaterialBase frontMaterial, sbyte sortOrder)
        {
            Vector3 frontDirection = GetRotateFrontDirectionLocal(ref transform, normal);
            Float3 up = normal;
            Float3 forward = frontDirection;
            Quaternion.LookRotation(ref forward, ref up, out var rotation);
            Matrix.RotationQuaternion(ref rotation, out var m2);
            Matrix.Multiply(ref m2, ref world, out var m3);
            DrawGizmoMesh(ref renderContext, arcMesh, frontMaterial, ref m3, sortOrder);
        }

        private void DrawRotationSphere(ref RenderContext renderContext, Mesh mesh, ref Matrix world, MaterialBase material, sbyte sortOrder)
        {
            // The depth-aware pass masks the rear halves of the orientation rings.
            // The overlay pass supplies the visible translucent hover fill because
            // the focus material does not contribute color in the depth prepass.
            DrawGizmoMesh(ref renderContext, mesh, material, ref world, sortOrder);
            DrawGizmoOverlayMesh(ref renderContext, mesh, material, ref world, sortOrder);
        }

        private void DrawRotationTrackballOverlay()
        {
            if (!HasActiveTransaction || _activeMode != Mode.Rotate || _activeAxis != Axis.Center || !_isDrawingRotationDrag)
                return;
            if (!TryProjectGizmoPoint(Position, out var center) ||
                !TryProjectGizmoPoint(_rotationDragStartPointWorld, out var start) ||
                !TryProjectGizmoPoint(_rotationDragCurrentPointWorld, out var current))
                return;

            var features = Render2D.Features;
            Render2D.Features = features & ~Render2D.RenderingFeatures.VertexSnapping;
            try
            {
                var fill = new Color(0.20f, 0.20f, 0.20f, 0.42f);
                var line = new Color(0.64f, 0.64f, 0.64f, 0.92f);
                float trackballRadius = Mathf.Max((start - center).Length, 1.0f);
                TryGetRotationTrackballScreenRadius(center, ref trackballRadius);
                float radialFactor = Mathf.Clamp((start - center).Length / trackballRadius, 0.0f, 1.0f);
                Float2 chord = current - start;
                Float2 midpoint = (start + current) * 0.5f;
                Float2 outward = midpoint - center;
                if (outward.LengthSquared < 0.0001f)
                    outward = new Float2(-chord.Y, chord.X);
                if (outward.LengthSquared > 0.0001f)
                    outward /= outward.Length;
                Float2 control = midpoint + outward * chord.Length * 0.36f * radialFactor;

                const int curveSegments = 12;
                Float2 previous = start;
                for (int i = 1; i <= curveSegments; i++)
                {
                    float t = i / (float)curveSegments;
                    float inverse = 1.0f - t;
                    Float2 point = start * (inverse * inverse) + control * (2.0f * inverse * t) + current * (t * t);
                    Float2 pointOffset = point - center;
                    if (pointOffset.LengthSquared > trackballRadius * trackballRadius)
                        point = center + pointOffset / pointOffset.Length * trackballRadius;
                    Render2D.FillTriangles(new[] { center, previous, point }, fill);
                    Render2D.DrawLine(previous, point, line, 1.5f);
                    previous = point;
                }
                Render2D.DrawLine(center, start, line, 1.5f);
                Render2D.DrawLine(center, current, line, 1.5f);
                DrawTrackballScreenPoint(start, Color.White, 3.0f);
                DrawTrackballScreenPoint(current, Color.White, 3.0f);
                var centerColor = new Color(1.0f, 0.86f, 0.12f, 0.82f);
                Render2D.DrawLine(center - new Float2(5.0f, 0.0f), center + new Float2(5.0f, 0.0f), centerColor, 1.5f);
                Render2D.DrawLine(center - new Float2(0.0f, 5.0f), center + new Float2(0.0f, 5.0f), centerColor, 1.5f);
            }
            finally
            {
                Render2D.Features = features;
            }
        }

        private bool TryGetRotationTrackballScreenRadius(Float2 center, ref float radius)
        {
            var transform = GetRotationTrackballTransform();
            Vector3 viewNormal = GetRotateToViewLocal(ref transform);
            Vector3 tangent = Vector3.Cross(viewNormal, Vector3.Up);
            if (tangent.LengthSquared < 0.0001f)
                tangent = Vector3.Cross(viewNormal, Vector3.Right);
            if (tangent.LengthSquared < 0.0001f)
                return false;
            tangent.Normalize();
            Vector3 rim = transform.LocalToWorld(tangent * _rotationTrackballRadiusRaw);
            if (!TryProjectGizmoPoint(rim, out var rimScreen))
                return false;
            float projectedRadius = (rimScreen - center).Length;
            if (projectedRadius < 1.0f)
                return false;
            radius = projectedRadius;
            return true;
        }

        private static void DrawTrackballScreenPoint(Float2 position, Color color, float radius)
        {
            var bounds = new Rectangle(position - new Float2(radius), new Float2(radius * 2.0f));
            StyleRendering.FillRoundedRectangle(bounds, color, 1.0f);
        }

        private bool IsScaleCursorHandle(Axis axis)
        {
            return HasActiveTransaction && _activeMode == Mode.Scale && _activeAxis == axis && (IsTranslateAxis(axis) || IsPlaneAxis(axis));
        }

        private bool IsTranslationPlaneFeedbackHandle(Axis axis)
        {
            return HasActiveTransaction && _activeMode == Mode.Translate && _activeAxis == axis && IsPlaneAxis(axis);
        }

        private bool IsTranslationPlaneComponentHandle()
        {
            return HasActiveTransaction && _activeMode == Mode.Translate && IsPlaneAxis(_activeAxis);
        }

        private bool IsScalePlaneComponentHandle()
        {
            return HasActiveTransaction && _activeMode == Mode.Scale && IsPlaneAxis(_activeAxis);
        }

        private void DrawScaleCursorOverlay()
        {
            if (!HasActiveTransaction || _activeMode != Mode.Scale || (!IsTranslateAxis(_activeAxis) && !IsPlaneAxis(_activeAxis)))
                return;
            if (!TryProjectGizmoPoint(Position, out var pivot))
                return;
            var cursor = Owner.Viewport.ViewMousePosition;
            var color = new Color(1.0f, 0.9f, 0.04f, 1.0f);
            if (IsTranslateAxis(_activeAxis))
            {
                if (!TryGetScaleAxisScreenDirection(_activeAxis, pivot, out var axisDirection))
                    return;
                float signedDistance = Float2.Dot(cursor - pivot, axisDirection);
                var arrowTip = pivot + axisDirection * signedDistance;
                var axisLine = arrowTip - pivot;
                if (axisLine.LengthSquared < 1.0f)
                    return;
                var direction = axisLine / axisLine.Length;
                var perpendicular = new Float2(-direction.Y, direction.X);
                const float capHalfSize = 6.0f;
                var capNear = arrowTip - direction * capHalfSize;
                var capFar = arrowTip + direction * capHalfSize;
                var cap = new[]
                {
                    capNear + perpendicular * capHalfSize,
                    capFar + perpendicular * capHalfSize,
                    capFar - perpendicular * capHalfSize,
                    capNear + perpendicular * capHalfSize,
                    capFar - perpendicular * capHalfSize,
                    capNear - perpendicular * capHalfSize,
                };
                Render2D.DrawLine(pivot, capNear, Color.Black.AlphaMultiplied(0.72f), 4.0f);
                Render2D.DrawLine(pivot, capNear, color, 2.0f);
                Render2D.FillTriangles(cap, color);
                return;
            }

            var planeLine = cursor - pivot;
            if (planeLine.LengthSquared < 1.0f)
                return;
            if (!TryGetPlaneHandleWorldCorners(_activeAxis, out var world0, out var world1, out var world2, out var world3) ||
                !TryProjectGizmoPoint(world0, out var screen0) ||
                !TryProjectGizmoPoint(world1, out var screen1) ||
                !TryProjectGizmoPoint(world2, out var screen2) ||
                !TryProjectGizmoPoint(world3, out var screen3))
                return;
            var handleCenter = (screen0 + screen1 + screen2 + screen3) * 0.25f;
            screen0 += cursor - handleCenter;
            screen1 += cursor - handleCenter;
            screen2 += cursor - handleCenter;
            screen3 += cursor - handleCenter;
            Render2D.DrawLine(pivot, cursor, Color.Black.AlphaMultiplied(0.65f), 3.0f);
            Render2D.DrawLine(pivot, cursor, color.AlphaMultiplied(0.78f), 1.5f);
            DrawScreenRectangleOutline(screen0, screen1, screen3, screen2, color, 2.0f);
        }

        private bool TryGetScaleAxisScreenDirection(Axis axis, Float2 pivot, out Float2 direction)
        {
            direction = Float2.Zero;
            if (!TryGetScaleDirectionLocal(axis, out var localDirection))
                return false;

            Vector3 axisTip = _gizmoWorld.LocalToWorld(localDirection * AxisLength);
            if (!TryProjectGizmoPoint(axisTip, out var axisTipScreen))
                return false;

            direction = axisTipScreen - pivot;
            if (direction.LengthSquared < 0.0001f)
                return false;
            direction /= direction.Length;
            return true;
        }

        private bool TryGetPlaneHandleWorldCorners(Axis axis, out Vector3 p0, out Vector3 p1, out Vector3 p2, out Vector3 p3)
        {
            float minimum = _planeHandleCenterRaw - _planeHandleHalfSizeRaw;
            float maximum = _planeHandleCenterRaw + _planeHandleHalfSizeRaw;
            Vector3 local0;
            Vector3 local1;
            Vector3 local2;
            Vector3 local3;
            switch (axis)
            {
            case Axis.XY:
                local0 = new Vector3(minimum, minimum, 0.0f);
                local1 = new Vector3(maximum, minimum, 0.0f);
                local2 = new Vector3(minimum, maximum, 0.0f);
                local3 = new Vector3(maximum, maximum, 0.0f);
                break;
            case Axis.ZX:
                local0 = new Vector3(minimum, 0.0f, minimum);
                local1 = new Vector3(maximum, 0.0f, minimum);
                local2 = new Vector3(minimum, 0.0f, maximum);
                local3 = new Vector3(maximum, 0.0f, maximum);
                break;
            case Axis.YZ:
                local0 = new Vector3(0.0f, minimum, minimum);
                local1 = new Vector3(0.0f, maximum, minimum);
                local2 = new Vector3(0.0f, minimum, maximum);
                local3 = new Vector3(0.0f, maximum, maximum);
                break;
            default:
                p0 = p1 = p2 = p3 = Vector3.Zero;
                return false;
            }
            p0 = _gizmoWorld.LocalToWorld(local0);
            p1 = _gizmoWorld.LocalToWorld(local1);
            p2 = _gizmoWorld.LocalToWorld(local2);
            p3 = _gizmoWorld.LocalToWorld(local3);
            return true;
        }

        private static void DrawScreenRectangleOutline(Float2 p0, Float2 p1, Float2 p2, Float2 p3, Color color, float thickness)
        {
            Render2D.DrawLine(p0, p1, color, thickness);
            Render2D.DrawLine(p1, p2, color, thickness);
            Render2D.DrawLine(p2, p3, color, thickness);
            Render2D.DrawLine(p3, p0, color, thickness);
        }

        private void DrawRotationScreenRing(ref RenderContext renderContext, Mesh mesh, MaterialBase material, sbyte sortOrder)
        {
            var viewDirection = Owner.ViewDirection;
            var up = Float3.Up * Owner.ViewOrientation;
            Quaternion.LookRotation(ref up, ref viewDirection, out var rotation);
            var transform = new Transform(Position, rotation, new Float3(_screenScale));
            renderContext.View.GetWorldMatrix(ref transform, out var world);
            DrawGizmoMesh(ref renderContext, mesh, material, ref world, sortOrder);
        }

        private bool ShouldDrawTranslationDistance()
        {
            return _isDrawingTranslationDistance &&
                   _activeMode == Mode.Translate &&
                   IsTranslateAxis(_activeAxis) &&
                   Owner.IsLeftMouseButtonDown &&
                   SelectionCount != 0 &&
                   !_isDisabled;
        }

        private bool TryGetTranslateAxisDirectionLocal(out Vector3 direction)
        {
            switch (_activeAxis)
            {
            case Axis.X:
                direction = Vector3.UnitX;
                return true;
            case Axis.Y:
                direction = Vector3.UnitY;
                return true;
            case Axis.Z:
                direction = Vector3.UnitZ;
                return true;
            default:
                direction = Vector3.Zero;
                return false;
            }
        }

        private bool IsTranslationDistanceReversed()
        {
            if (!ShouldDrawTranslationDistance() || !TryGetTranslateAxisDirectionLocal(out var axisDirectionLocal))
                return false;

            Vector3 moveDelta = HasActiveTransaction && _interactionResult != null
                ? _interactionResult.Translation
                : Position - _translationDragStartPosition;
            if (moveDelta.LengthSquared < 0.0001f)
                return false;

            Vector3 worldAxis = _gizmoWorld.LocalToWorldVector(axisDirectionLocal);
            if (worldAxis.LengthSquared < 0.0001f)
                return false;
            worldAxis.Normalize();
            return Vector3.Dot(moveDelta, worldAxis) < 0.0f;
        }

        private bool TryProjectGizmoPoint(Vector3 worldPosition, out Float2 screenPosition)
        {
            screenPosition = Float2.Zero;
            var viewport = Owner.Viewport;
            if (viewport.Width < Mathf.Epsilon || viewport.Height < Mathf.Epsilon)
                return false;

            if (!_gizmoProjectionValid)
                return false;

            var toPoint = worldPosition - Owner.ViewPosition;
            if (Vector3.Dot(toPoint, (Vector3)Owner.ViewDirection) <= 0.0f)
                return false;

            viewport.ProjectPoint(worldPosition, out screenPosition);
            return true;
        }

        private bool TryProjectTranslationMeasurePoint(Vector3 worldPosition, out Float2 screenPosition)
        {
            return TryProjectGizmoPoint(worldPosition, out screenPosition);
        }

        private static void DrawTranslationDistanceDashLine(Float2 start, Float2 end, Color color)
        {
            Float2 line = end - start;
            float length = line.Length;
            if (length < 1.0f)
                return;

            Float2 direction = line / length;
            const float dashLength = 5.0f;
            const float gapLength = 4.0f;
            for (float distance = 0.0f; distance < length; distance += dashLength + gapLength)
            {
                Float2 dashStart = start + direction * distance;
                Float2 dashEnd = start + direction * Mathf.Min(distance + dashLength, length);
                Render2D.DrawLine(dashStart, dashEnd, color, 1.5f);
            }
        }

        private static string FormatTranslationDistanceLabel(float distance)
        {
            float rounded = Mathf.Round(distance);
            if (Mathf.Abs(distance - rounded) < 0.05f)
                return string.Format(CultureInfo.InvariantCulture, "{0:0}cm", rounded);
            return string.Format(CultureInfo.InvariantCulture, "{0:0.#}cm", distance);
        }

        private void DrawTranslationDistance()
        {
            if (!ShouldDrawTranslationDistance() || !TryGetTranslateAxisDirectionLocal(out var axisDirectionLocal))
                return;
            if (IsTranslationDistanceReversed())
                axisDirectionLocal = -axisDirectionLocal;

            Vector3 currentPosition = Position;
            Vector3 arrowTip = _gizmoWorld.LocalToWorld(axisDirectionLocal * AxisLength);
            if (!TryProjectTranslationMeasurePoint(_translationDragStartPosition, out var startScreen) ||
                !TryProjectTranslationMeasurePoint(currentPosition, out var currentScreen) ||
                !TryProjectTranslationMeasurePoint(arrowTip, out var arrowTipScreen))
                return;

            var features = Render2D.Features;
            Render2D.Features = features & ~Render2D.RenderingFeatures.VertexSnapping;

            DrawTranslationDistanceDashLine(startScreen, currentScreen, _translationDistanceColor);

            const float pointRadius = 3.0f;
            var pointRect = new Rectangle(startScreen - new Float2(pointRadius), new Float2(pointRadius * 2.0f));
            StyleRendering.FillRoundedRectangle(pointRect, _translationDistanceColor, pointRadius);

            Float2 labelDirection = arrowTipScreen - currentScreen;
            if (labelDirection.LengthSquared < 0.0001f)
                labelDirection = new Float2(1.0f, 0.0f);
            else
                labelDirection /= labelDirection.Length;

            string label = FormatTranslationDistanceLabel((float)Vector3.Distance(_translationDragStartPosition, currentPosition));
            var font = Style.Current.FontSmall;
            Float2 textSize = font.MeasureText(label);
            Float2 pillSize = textSize + new Float2(18.0f, 8.0f);
            Float2 pillCenter = arrowTipScreen + labelDirection * (pillSize.X * 0.5f + 8.0f);
            var pillRect = new Rectangle(pillCenter - pillSize * 0.5f, pillSize);
            StyleRendering.FillRoundedRectangle(pillRect, _translationDistancePillColor, pillRect.Height * 0.5f);
            Render2D.DrawText(font, label, pillRect, Color.White, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap);

            Render2D.Features = features;
        }

        private float GetVertexSnapPointScreenScale(Vector3 worldPosition)
        {
            float gizmoSize = Editor.Instance.Options.Options.Visual.GizmoSize;
            if (Owner.Viewport.UseOrthographicProjection)
                return gizmoSize * (50 * Owner.Viewport.OrthographicScale);

            Vector3 vLength = Owner.ViewPosition - worldPosition;
            return (float)(vLength.Length / VertexSnapPointScaleFactor * gizmoSize);
        }

        private void DrawVertexSnapPointHighlight(ref RenderContext renderContext, Mesh sphereMesh, Vector3 worldPosition, MaterialBase material, sbyte sortOrder)
        {
            float screenScale = GetVertexSnapPointScreenScale(worldPosition);
            var transform = new Transform(worldPosition, Quaternion.Identity, new Float3(screenScale));
            renderContext.View.GetWorldMatrix(ref transform, out var world);

            Matrix.Scaling(_vertexSnapPointOuterScale, out var scale);
            Matrix.Multiply(ref scale, ref world, out var markerWorld);
            DrawGizmoOverlayMesh(ref renderContext, sphereMesh, _materialVertexSnapPointShadow, ref markerWorld, sortOrder);

            Matrix.Scaling(_vertexSnapPointInnerScale, out scale);
            Matrix.Multiply(ref scale, ref world, out markerWorld);
            DrawGizmoOverlayMesh(ref renderContext, sphereMesh, material, ref markerWorld, (sbyte)(sortOrder + 1));
        }

        private void DrawVertexSnapPointHighlights(ref RenderContext renderContext, Mesh sphereMesh, sbyte sortOrder)
        {
            if (!_isActive || !IsActive)
                return;
            if (_isVertexSnapTemporaryPivot)
                return;

            if (_vertexSnapObject != null)
            {
                var worldPosition = _vertexSnapObject.Transform.LocalToWorld(_vertexSnapPoint);
                DrawVertexSnapPointHighlight(ref renderContext, sphereMesh, worldPosition, _materialVertexSnapPoint, sortOrder);
            }
            if (_vertexSnapObjectTo != null)
            {
                var worldPosition = _vertexSnapObjectTo.Transform.LocalToWorld(_vertexSnapPointTo);
                DrawVertexSnapPointHighlight(ref renderContext, sphereMesh, worldPosition, _materialVertexSnapTargetPoint, (sbyte)(sortOrder + 2));
            }
        }

        private static void DrawVertexSnapEdgeFadeLine(Float2 start, Float2 end)
        {
            var line = end - start;
            if (line.LengthSquared < 1.0f)
                return;

            for (int i = 0; i < _vertexSnapEdgeSegments; i++)
            {
                float t0 = (float)i / _vertexSnapEdgeSegments;
                float t1 = (float)(i + 1) / _vertexSnapEdgeSegments;
                var segmentStart = start + line * t0;
                var segmentEnd = start + line * t1;
                float alpha = 0.9f * (1.0f - t0);
                Render2D.DrawLine(segmentStart, segmentEnd, Color.Black.AlphaMultiplied(alpha * 0.35f), _vertexSnapEdgeThickness + 2.0f);
                Render2D.DrawLine(segmentStart, segmentEnd, _vertexSnapEdgeColor.AlphaMultiplied(alpha), _vertexSnapEdgeThickness);
            }
        }

        private void DrawVertexSnapConnectedEdges(SceneGraphNode node, Vector3 worldPosition)
        {
            if (node == null || !TryProjectGizmoPoint(worldPosition, out var startScreen))
                return;

            _vertexSnapEdgePoints.Clear();
            node.OnVertexSnapEdges(worldPosition, _vertexSnapEdgePoints);
            for (int i = 0; i < _vertexSnapEdgePoints.Count; i++)
            {
                var connectedVertex = _vertexSnapEdgePoints[i];
                if (Vector3.DistanceSquared(connectedVertex, worldPosition) <= 0.0001)
                    continue;
                if (TryProjectGizmoPoint(connectedVertex, out var endScreen))
                    DrawVertexSnapEdgeFadeLine(startScreen, endScreen);
            }
        }

        private void DrawVertexSnapEdgeHighlights()
        {
            if (!_isActive || !IsActive || !Owner.SnapToVertex || _vertexSnapObject == null)
                return;

            var features = Render2D.Features;
            Render2D.Features = features & ~Render2D.RenderingFeatures.VertexSnapping;

            var worldPosition = _vertexSnapObject.Transform.LocalToWorld(_vertexSnapPoint);
            DrawVertexSnapConnectedEdges(_vertexSnapObject, worldPosition);
            if (_vertexSnapObjectTo != null)
            {
                worldPosition = _vertexSnapObjectTo.Transform.LocalToWorld(_vertexSnapPointTo);
                DrawVertexSnapConnectedEdges(_vertexSnapObjectTo, worldPosition);
            }

            Render2D.Features = features;
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();
            DrawVertexSnapEdgeHighlights();
            DrawRotationTrackballOverlay();
            DrawScaleCursorOverlay();
            DrawFeedbackOverlay();
        }

        /// <inheritdoc />
        public override void Draw(ref RenderContext renderContext)
        {
            if (!_isActive || !IsActive || !_gizmoProjectionValid)
                return;
            if (!_modelCube || !_modelCube.IsLoaded)
                return;

            // Update the gizmo brightness every frame to ensure it updates correctly
            UpdateGizmoBrightness(Editor.Instance.Options.Options);

            Matrix m1, m2, m3;
            float boxScale = 300f;
            float boxSize = 0.085f;
            bool isXAxis = _activeAxis == Axis.X || _activeAxis == Axis.XY || _activeAxis == Axis.ZX;
            bool isYAxis = _activeAxis == Axis.Y || _activeAxis == Axis.XY || _activeAxis == Axis.YZ;
            bool isZAxis = _activeAxis == Axis.Z || _activeAxis == Axis.YZ || _activeAxis == Axis.ZX;
            bool isCenter = _activeAxis == Axis.Center;
            bool isShowingTranslationDistance = ShouldDrawTranslationDistance();
            bool isTranslationDistanceReversed = IsTranslationDistanceReversed();
            renderContext.View.GetWorldMatrix(ref _gizmoWorld, out Matrix world);

            const sbyte sortOrder = 100; // Draw after any other editor shapes
            Mesh cubeMesh = _modelCube.LODs[0].Meshes[0];
            Mesh sphereMesh = _modelSphere.LODs[0].Meshes[0];

            Matrix.Scaling(GizmoModelsScale2RealGizmoSize, out m3);
            Matrix.Multiply(ref m3, ref world, out m1);

            switch (_activeMode)
            {
            case Mode.Translate:
            {
                if (!_modelTranslationAxis || !_modelTranslationAxis.IsLoaded)
                    break;
                var transAxisMesh = _modelTranslationAxis.LODs[0].Meshes[0];

                // X axis
                Matrix.RotationY(isShowingTranslationDistance && _activeAxis == Axis.X && isTranslationDistanceReversed ? Mathf.PiOverTwo : -Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref world, out m3);
                MaterialInstance xAxisMaterialTransform = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                if (ShouldDrawFeedbackHandle(Axis.X) && !IsTranslationPlaneComponentHandle() && (!isShowingTranslationDistance || _activeAxis == Axis.X))
                    DrawGizmoMesh(ref renderContext, transAxisMesh, xAxisMaterialTransform, ref m3, sortOrder);

                // Y axis
                Matrix.RotationX(isShowingTranslationDistance && _activeAxis == Axis.Y && isTranslationDistanceReversed ? -Mathf.PiOverTwo : Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref world, out m3);
                MaterialInstance yAxisMaterialTransform = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                if (ShouldDrawFeedbackHandle(Axis.Y) && !IsTranslationPlaneComponentHandle() && (!isShowingTranslationDistance || _activeAxis == Axis.Y))
                    DrawGizmoMesh(ref renderContext, transAxisMesh, yAxisMaterialTransform, ref m3, sortOrder);

                // Z axis
                Matrix.RotationX(isShowingTranslationDistance && _activeAxis == Axis.Z && isTranslationDistanceReversed ? 0.0f : Mathf.Pi, out m2);
                Matrix.Multiply(ref m2, ref world, out m3);
                MaterialInstance zAxisMaterialTransform = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                if (ShouldDrawFeedbackHandle(Axis.Z) && !IsTranslationPlaneComponentHandle() && (!isShowingTranslationDistance || _activeAxis == Axis.Z))
                    DrawGizmoMesh(ref renderContext, transAxisMesh, zAxisMaterialTransform, ref m3, sortOrder);

                if (!isShowingTranslationDistance)
                {
                    // XY plane
                    if (ShouldDrawFeedbackHandle(Axis.XY) && !IsTranslationPlaneFeedbackHandle(Axis.XY))
                    {
                        m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationX(Mathf.PiOverTwo), new Vector3(boxSize * boxScale, boxSize * boxScale, 0.0f));
                        Matrix.Multiply(ref m2, ref m1, out m3);
                        MaterialInstance xyPlaneMaterialTransform = (_activeAxis == Axis.XY && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                        DrawGizmoMesh(ref renderContext, cubeMesh, xyPlaneMaterialTransform, ref m3, sortOrder);
                    }

                    // ZX plane
                    if (ShouldDrawFeedbackHandle(Axis.ZX) && !IsTranslationPlaneFeedbackHandle(Axis.ZX))
                    {
                        m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.Identity, new Vector3(boxSize * boxScale, 0.0f, boxSize * boxScale));
                        Matrix.Multiply(ref m2, ref m1, out m3);
                        MaterialInstance zxPlaneMaterialTransform = (_activeAxis == Axis.ZX && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                        DrawGizmoMesh(ref renderContext, cubeMesh, zxPlaneMaterialTransform, ref m3, sortOrder);
                    }

                    // YZ plane
                    if (ShouldDrawFeedbackHandle(Axis.YZ) && !IsTranslationPlaneFeedbackHandle(Axis.YZ))
                    {
                        m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationZ(Mathf.PiOverTwo), new Vector3(0.0f, boxSize * boxScale, boxSize * boxScale));
                        Matrix.Multiply(ref m2, ref m1, out m3);
                        MaterialInstance yzPlaneMaterialTransform = (_activeAxis == Axis.YZ && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                        DrawGizmoMesh(ref renderContext, cubeMesh, yzPlaneMaterialTransform, ref m3, sortOrder);
                    }

                    // Center sphere
                    if (_vertexSnapObject == null && ShouldDrawFeedbackHandle(Axis.Center))
                    {
                        Matrix.Scaling(GizmoModelsScale2RealGizmoSize, out m2);
                        Matrix.Multiply(ref m2, ref m1, out m3);
                        DrawGizmoMesh(ref renderContext, sphereMesh, isCenter ? _materialAxisFocus : _materialSphere, ref m3, sortOrder);
                    }
                }

                break;
            }

            case Mode.Rotate:
            {
                if (!_modelRotationArc || !_modelRotationArc.IsLoaded ||
                    !_modelRotationTrackballArc || !_modelRotationTrackballArc.IsLoaded ||
                    !_modelRotationSphere || !_modelRotationSphere.IsLoaded ||
                    !_modelRotationScreenRing || !_modelRotationScreenRing.IsLoaded)
                    break;
                var rotationArcMesh = (isCenter ? _modelRotationTrackballArc : _modelRotationArc).LODs[0].Meshes[0];
                var rotationSphereMesh = _modelRotationSphere.LODs[0].Meshes[0];
                var rotationScreenRingMesh = _modelRotationScreenRing.LODs[0].Meshes[0];
                var rotationDrawTransform = _gizmoWorld;
                if (_activeTransformSpace == TransformSpace.World && !_rotationGizmoDelta.IsIdentity)
                    rotationDrawTransform.Orientation = _rotationGizmoDelta * rotationDrawTransform.Orientation;
                renderContext.View.GetWorldMatrix(ref rotationDrawTransform, out var rotationWorld);

                // Trackball and background
                if (ShouldDrawFeedbackHandle(Axis.Center) && isCenter && !_isDisabled)
                    DrawRotationSphere(ref renderContext, rotationSphereMesh, ref rotationWorld, _materialTrackballFocus, (sbyte)(sortOrder - 2));
                if (ShouldDrawFeedbackHandle(Axis.Screen))
                    DrawRotationScreenRing(ref renderContext, rotationScreenRingMesh, _activeAxis == Axis.Screen && !_isDisabled ? _materialAxisFocus : _materialAxisBack, (sbyte)(sortOrder - 1));
                // X axis
                MaterialInstance xAxisMaterialRotate = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                if (ShouldDrawFeedbackHandle(Axis.X))
                    DrawRotationAxis(ref renderContext, rotationArcMesh, ref rotationDrawTransform, ref rotationWorld, Vector3.UnitX, xAxisMaterialRotate, sortOrder);

                // Y axis
                MaterialInstance yAxisMaterialRotate = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                if (ShouldDrawFeedbackHandle(Axis.Y))
                    DrawRotationAxis(ref renderContext, rotationArcMesh, ref rotationDrawTransform, ref rotationWorld, Vector3.UnitY, yAxisMaterialRotate, sortOrder);

                // Z axis
                MaterialInstance zAxisMaterialRotate = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                if (ShouldDrawFeedbackHandle(Axis.Z))
                    DrawRotationAxis(ref renderContext, rotationArcMesh, ref rotationDrawTransform, ref rotationWorld, Vector3.UnitZ, zAxisMaterialRotate, sortOrder);
                break;
            }

            case Mode.Scale:
            {
                if (!_modelScaleAxis || !_modelScaleAxis.IsLoaded)
                    break;
                var scaleAxisMesh = _modelScaleAxis.LODs[0].Meshes[0];

                // X axis
                Matrix.RotationY(-Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref world, out m3);
                MaterialInstance xAxisMaterialRotate = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                if (ShouldDrawFeedbackHandle(Axis.X) && !IsScaleCursorHandle(Axis.X) && !IsScalePlaneComponentHandle())
                    DrawGizmoMesh(ref renderContext, scaleAxisMesh, xAxisMaterialRotate, ref m3, sortOrder);

                // Y axis
                Matrix.RotationX(Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref world, out m3);
                MaterialInstance yAxisMaterialRotate = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                if (ShouldDrawFeedbackHandle(Axis.Y) && !IsScaleCursorHandle(Axis.Y) && !IsScalePlaneComponentHandle())
                    DrawGizmoMesh(ref renderContext, scaleAxisMesh, yAxisMaterialRotate, ref m3, sortOrder);

                // Z axis
                Matrix.RotationX(Mathf.Pi, out m2);
                Matrix.Multiply(ref m2, ref world, out m3);
                MaterialInstance zAxisMaterialRotate = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                if (ShouldDrawFeedbackHandle(Axis.Z) && !IsScaleCursorHandle(Axis.Z) && !IsScalePlaneComponentHandle())
                    DrawGizmoMesh(ref renderContext, scaleAxisMesh, zAxisMaterialRotate, ref m3, sortOrder);

                // XY plane
                m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationX(Mathf.PiOverTwo), new Vector3(boxSize * boxScale, boxSize * boxScale, 0.0f));
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance xyPlaneMaterialScale = (_activeAxis == Axis.XY && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                if (ShouldDrawFeedbackHandle(Axis.XY) && !IsScaleCursorHandle(Axis.XY))
                    DrawGizmoMesh(ref renderContext, cubeMesh, xyPlaneMaterialScale, ref m3, sortOrder);

                // ZX plane
                m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.Identity, new Vector3(boxSize * boxScale, 0.0f, boxSize * boxScale));
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance zxPlaneMaterialScale = (_activeAxis == Axis.ZX && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                if (ShouldDrawFeedbackHandle(Axis.ZX) && !IsScaleCursorHandle(Axis.ZX))
                    DrawGizmoMesh(ref renderContext, cubeMesh, zxPlaneMaterialScale, ref m3, sortOrder);

                // YZ plane
                m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationZ(Mathf.PiOverTwo), new Vector3(0.0f, boxSize * boxScale, boxSize * boxScale));
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance yzPlaneMaterialScale = (_activeAxis == Axis.YZ && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                if (ShouldDrawFeedbackHandle(Axis.YZ) && !IsScaleCursorHandle(Axis.YZ))
                    DrawGizmoMesh(ref renderContext, cubeMesh, yzPlaneMaterialScale, ref m3, sortOrder);

                // Center box
                if (_vertexSnapObject == null && ShouldDrawFeedbackHandle(Axis.Center))
                {
                    Matrix.Scaling(GizmoModelsScale2RealGizmoSize, out m2);
                    Matrix.Multiply(ref m2, ref m1, out m3);
                    DrawGizmoMesh(ref renderContext, sphereMesh, isCenter ? _materialAxisFocus : _materialSphere, ref m3, sortOrder);
                }

                break;
            }
            }

            DrawVertexSnapPointHighlights(ref renderContext, sphereMesh, (sbyte)(sortOrder + 3));
        }
    }
}
