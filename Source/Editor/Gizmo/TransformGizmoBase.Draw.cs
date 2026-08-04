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
        private Model _modelRotationSphere;
        private Model _modelRotationScreenRing;
        private Model _modelRotationTrackballTriangle;
        private Model _modelRotationTrackballPoint;
        private Model _modelSphere;
        private Model _modelCube;

        // Materials
        private MaterialInstance _materialAxisX;
        private MaterialInstance _materialAxisY;
        private MaterialInstance _materialAxisZ;
        private MaterialInstance _materialAxisFocus;
        private MaterialInstance _materialAxisBack;
        private MaterialInstance _materialTrackballFocus;
        private MaterialInstance _materialTrackballTriangle;
        private MaterialInstance _materialTrackballPoint;
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
        private const float _rotationSphereRadiusRaw = RotateRadiusRaw + _rotationHandleThicknessRaw;
        private const float _rotationSphereOpacity = 0.16f;
        private const float _rotationTrackballRadiusRaw = _rotationSphereRadiusRaw;
        private const float _rotationTrackballOpacity = 0.35f;
        private const float _rotationTrackballTriangleOpacity = 0.45f;
        private const float _rotationTrackballPointRadiusRaw = 0.12f;
        private const float _rotationScreenRingRadiusRaw = _rotationSphereRadiusRaw + 0.20f;
        private const float _rotationScreenRingThicknessRaw = 0.045f;
        private const float _vertexSnapPointOuterScale = 0.0018f;
        private const float _vertexSnapPointInnerScale = 0.0011f;
        private const int _vertexSnapEdgeSegments = 10;
        private const float _vertexSnapEdgeThickness = 2.0f;
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
            _modelTranslationAxis = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Gizmo/TranslationAxis");
            _modelScaleAxis = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Gizmo/ScaleAxis");
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
            _modelRotationSphere = CreateSphereModel(_rotationSphereRadiusRaw, _rotationArcSegments, _rotationArcSegments / 2);
            _modelRotationScreenRing = CreateTorusSegmentModel(_rotationScreenRingRadiusRaw, _rotationScreenRingThicknessRaw, 0.0f, Mathf.TwoPi, _rotationArcSegments, _rotationTubeSegments);
            _modelRotationTrackballTriangle = CreateTriangleModel();
            _modelRotationTrackballPoint = CreateSphereModel(_rotationTrackballPointRadiusRaw, 12, 6);
            _materialAxisBack = _materialAxisX.CreateVirtualInstance();
            _materialAxisBack.SetParameterValue(_colorParamName, new Color(0.42f, 0.42f, 0.42f, 1.0f));
            _materialTrackballFocus = _materialAxisFocus.CreateVirtualInstance();
            _materialTrackballFocus.SetParameterValue(_opacityParamName, _rotationTrackballOpacity);
            _materialTrackballTriangle = _materialAxisX.CreateVirtualInstance();
            _materialTrackballTriangle.SetParameterValue(_colorParamName, new Color(0.22f, 0.22f, 0.22f, 1.0f));
            _materialTrackballTriangle.SetParameterValue(_opacityParamName, _rotationTrackballTriangleOpacity);
            _materialTrackballPoint = _materialAxisX.CreateVirtualInstance();
            _materialTrackballPoint.SetParameterValue(_colorParamName, Color.White);
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

        private static Model CreateTriangleModel()
        {
            var model = FlaxEngine.Content.CreateVirtualAsset<Model>();
            model.SetupLODs(new[] { 1 });
            UpdateTriangleModel(model, Vector3.Zero, Vector3.UnitX, Vector3.UnitY);
            return model;
        }

        private static void UpdateTriangleModel(Model model, Vector3 center, Vector3 start, Vector3 current)
        {
            var normal = Vector3.Cross(start - center, current - center);
            if (normal.LengthSquared < 0.0001f)
                normal = Vector3.Up;
            else
                normal.Normalize();

            var vertices = new Float3[] { center, start, current };
            var normals = new Float3[] { normal, normal, normal };
            var indices = new[] { 0, 1, 2, 0, 2, 1 };
            model.LODs[0].Meshes[0].UpdateMesh(vertices, indices, normals);
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
            _materialTrackballTriangle.SetParameterValue(_opacityParamName, opacity * _rotationTrackballTriangleOpacity);
            _materialTrackballPoint.SetParameterValue(_opacityParamName, opacity);
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
            _materialTrackballTriangle.SetParameterValue(_brightnessParamName, brightness);
            _materialTrackballPoint.SetParameterValue(_brightnessParamName, brightness);
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

        private void DrawRotationAxis(ref RenderContext renderContext, Mesh arcMesh, ref Transform transform, ref Matrix world, Vector3 normal, MaterialBase frontMaterial, sbyte sortOrder)
        {
            Vector3 frontDirection = GetRotateFrontDirectionLocal(ref transform, normal);
            Float3 up = normal;
            Float3 forward = frontDirection;
            Quaternion.LookRotation(ref forward, ref up, out var rotation);
            Matrix.RotationQuaternion(ref rotation, out var m2);
            Matrix.Multiply(ref m2, ref world, out var m3);
            arcMesh.Draw(ref renderContext, frontMaterial, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
        }

        private void DrawRotationSphere(ref RenderContext renderContext, Mesh mesh, ref Matrix world, MaterialBase material, sbyte sortOrder)
        {
            mesh.Draw(ref renderContext, material, ref world, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
        }

        private void DrawRotationScreenRing(ref RenderContext renderContext, Mesh mesh, MaterialBase material, sbyte sortOrder)
        {
            var viewDirection = Owner.ViewDirection;
            var up = Float3.Up * Owner.ViewOrientation;
            Quaternion.LookRotation(ref up, ref viewDirection, out var rotation);
            var transform = new Transform(Position, rotation, new Float3(_screenScale));
            renderContext.View.GetWorldMatrix(ref transform, out var world);
            mesh.Draw(ref renderContext, material, ref world, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
        }

        private void DrawRotationTrackballPoint(ref RenderContext renderContext, Mesh pointMesh, Vector3 point, ref Matrix world, sbyte sortOrder)
        {
            Float3 pointLocal = point;
            Matrix.Translation(ref pointLocal, out var m2);
            Matrix.Multiply(ref m2, ref world, out var m3);
            pointMesh.Draw(ref renderContext, _materialTrackballPoint, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
        }

        private Vector3 GetRotationDragPointLocal(Transform drawTransform, Vector3 point)
        {
            drawTransform.WorldToLocal(ref point, out var result);
            return result;
        }

        private void DrawRotationTrackballTriangle(ref RenderContext renderContext, Mesh triangleMesh, ref Transform drawTransform, ref Matrix world, sbyte sortOrder)
        {
            if (!_isDrawingRotationDrag)
                return;

            Vector3 center = GetRotationDragPointLocal(drawTransform, Position);
            Vector3 start = GetRotationDragPointLocal(drawTransform, _rotationDragStartPointWorld);
            Vector3 current = GetRotationDragPointLocal(drawTransform, _rotationDragCurrentPointWorld);
            UpdateTriangleModel(_modelRotationTrackballTriangle, center, start, current);
            triangleMesh.Draw(ref renderContext, _materialTrackballTriangle, ref world, StaticFlags.None, true, DrawPass.Default, 0.0f, (sbyte)(sortOrder - 1));
        }

        private void DrawRotationTrackballPoints(ref RenderContext renderContext, Mesh pointMesh, ref Transform drawTransform, ref Matrix world, sbyte sortOrder)
        {
            if (!_isDrawingRotationDrag)
                return;

            DrawRotationTrackballPoint(ref renderContext, pointMesh, GetRotationDragPointLocal(drawTransform, Position), ref world, (sbyte)(sortOrder + 1));
            DrawRotationTrackballPoint(ref renderContext, pointMesh, GetRotationDragPointLocal(drawTransform, _rotationDragStartPointWorld), ref world, (sbyte)(sortOrder + 1));
            DrawRotationTrackballPoint(ref renderContext, pointMesh, GetRotationDragPointLocal(drawTransform, _rotationDragCurrentPointWorld), ref world, (sbyte)(sortOrder + 1));
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

            Vector3 moveDelta = Position - _translationDragStartPosition;
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

            if (!viewport.UseOrthographicProjection)
            {
                var toPoint = worldPosition - Owner.ViewPosition;
                if (Vector3.Dot(toPoint, (Vector3)Owner.ViewDirection) <= 0.0f)
                    return false;
            }

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
            return (float)(vLength.Length / GizmoScaleFactor * gizmoSize);
        }

        private void DrawVertexSnapPointHighlight(ref RenderContext renderContext, Mesh sphereMesh, Vector3 worldPosition, MaterialBase material, sbyte sortOrder)
        {
            float screenScale = GetVertexSnapPointScreenScale(worldPosition);
            var transform = new Transform(worldPosition, Quaternion.Identity, new Float3(screenScale));
            renderContext.View.GetWorldMatrix(ref transform, out var world);

            Matrix.Scaling(_vertexSnapPointOuterScale, out var scale);
            Matrix.Multiply(ref scale, ref world, out var markerWorld);
            sphereMesh.Draw(ref renderContext, _materialVertexSnapPointShadow, ref markerWorld, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

            Matrix.Scaling(_vertexSnapPointInnerScale, out scale);
            Matrix.Multiply(ref scale, ref world, out markerWorld);
            sphereMesh.Draw(ref renderContext, material, ref markerWorld, StaticFlags.None, true, DrawPass.Default, 0.0f, (sbyte)(sortOrder + 1));
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
            DrawTranslationDistance();
        }

        /// <inheritdoc />
        public override void Draw(ref RenderContext renderContext)
        {
            if (!_isActive || !IsActive)
                return;
            if (!_modelCube || !_modelCube.IsLoaded)
                return;

            // Update the gizmo brightness every frame to ensure it updates correctly
            UpdateGizmoBrightness(Editor.Instance.Options.Options);

            // As all axisMesh have the same pivot, add a little offset to the x axisMesh, this way SortDrawCalls is able to sort the draw order
            // https://github.com/FlaxEngine/FlaxEngine/issues/680

            Matrix m1, m2, m3, mx1;
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
            const float gizmoModelsScale2RealGizmoSize = 0.075f;
            Mesh cubeMesh = _modelCube.LODs[0].Meshes[0];
            Mesh sphereMesh = _modelSphere.LODs[0].Meshes[0];

            Matrix.Scaling(gizmoModelsScale2RealGizmoSize, out m3);
            Matrix.Multiply(ref m3, ref world, out m1);
            mx1 = m1;
            mx1.M41 += 0.05f;

            switch (_activeMode)
            {
            case Mode.Translate:
            {
                if (!_modelTranslationAxis || !_modelTranslationAxis.IsLoaded)
                    break;
                var transAxisMesh = _modelTranslationAxis.LODs[0].Meshes[0];

                // X axis
                Matrix.RotationY(isShowingTranslationDistance && _activeAxis == Axis.X && isTranslationDistanceReversed ? Mathf.PiOverTwo : -Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance xAxisMaterialTransform = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                if (!isShowingTranslationDistance || _activeAxis == Axis.X)
                    transAxisMesh.Draw(ref renderContext, xAxisMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Y axis
                Matrix.RotationX(isShowingTranslationDistance && _activeAxis == Axis.Y && isTranslationDistanceReversed ? -Mathf.PiOverTwo : Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance yAxisMaterialTransform = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                if (!isShowingTranslationDistance || _activeAxis == Axis.Y)
                    transAxisMesh.Draw(ref renderContext, yAxisMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Z axis
                Matrix.RotationX(isShowingTranslationDistance && _activeAxis == Axis.Z && isTranslationDistanceReversed ? 0.0f : Mathf.Pi, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance zAxisMaterialTransform = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                if (!isShowingTranslationDistance || _activeAxis == Axis.Z)
                    transAxisMesh.Draw(ref renderContext, zAxisMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                if (!isShowingTranslationDistance)
                {
                    // XY plane
                    m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationX(Mathf.PiOverTwo), new Vector3(boxSize * boxScale, boxSize * boxScale, 0.0f));
                    Matrix.Multiply(ref m2, ref m1, out m3);
                    MaterialInstance xyPlaneMaterialTransform = (_activeAxis == Axis.XY && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                    cubeMesh.Draw(ref renderContext, xyPlaneMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                    // ZX plane
                    m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.Identity, new Vector3(boxSize * boxScale, 0.0f, boxSize * boxScale));
                    Matrix.Multiply(ref m2, ref m1, out m3);
                    MaterialInstance zxPlaneMaterialTransform = (_activeAxis == Axis.ZX && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                    cubeMesh.Draw(ref renderContext, zxPlaneMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                    // YZ plane
                    m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationZ(Mathf.PiOverTwo), new Vector3(0.0f, boxSize * boxScale, boxSize * boxScale));
                    Matrix.Multiply(ref m2, ref m1, out m3);
                    MaterialInstance yzPlaneMaterialTransform = (_activeAxis == Axis.YZ && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                    cubeMesh.Draw(ref renderContext, yzPlaneMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                    // Center sphere
                    if (_vertexSnapObject == null)
                    {
                        Matrix.Scaling(gizmoModelsScale2RealGizmoSize, out m2);
                        Matrix.Multiply(ref m2, ref m1, out m3);
                        sphereMesh.Draw(ref renderContext, isCenter ? _materialAxisFocus : _materialSphere, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
                    }
                }

                break;
            }

            case Mode.Rotate:
            {
                if (!_modelRotationArc || !_modelRotationArc.IsLoaded ||
                    !_modelRotationSphere || !_modelRotationSphere.IsLoaded ||
                    !_modelRotationScreenRing || !_modelRotationScreenRing.IsLoaded ||
                    !_modelRotationTrackballTriangle || !_modelRotationTrackballTriangle.IsLoaded ||
                    !_modelRotationTrackballPoint || !_modelRotationTrackballPoint.IsLoaded)
                    break;
                var rotationArcMesh = _modelRotationArc.LODs[0].Meshes[0];
                var rotationSphereMesh = _modelRotationSphere.LODs[0].Meshes[0];
                var rotationScreenRingMesh = _modelRotationScreenRing.LODs[0].Meshes[0];
                var rotationTrackballTriangleMesh = _modelRotationTrackballTriangle.LODs[0].Meshes[0];
                var rotationTrackballPointMesh = _modelRotationTrackballPoint.LODs[0].Meshes[0];
                var rotationDrawTransform = _gizmoWorld;
                if (_activeTransformSpace == TransformSpace.World && !_rotationGizmoDelta.IsIdentity)
                    rotationDrawTransform.Orientation = _rotationGizmoDelta * rotationDrawTransform.Orientation;
                renderContext.View.GetWorldMatrix(ref rotationDrawTransform, out var rotationWorld);

                // Trackball and background
                if (isCenter && !_isDisabled)
                    DrawRotationSphere(ref renderContext, rotationSphereMesh, ref rotationWorld, _materialTrackballFocus, (sbyte)(sortOrder - 2));
                DrawRotationScreenRing(ref renderContext, rotationScreenRingMesh, _activeAxis == Axis.Screen && !_isDisabled ? _materialAxisFocus : _materialAxisBack, (sbyte)(sortOrder - 1));
                DrawRotationTrackballTriangle(ref renderContext, rotationTrackballTriangleMesh, ref rotationDrawTransform, ref rotationWorld, sortOrder);

                // X axis
                MaterialInstance xAxisMaterialRotate = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                DrawRotationAxis(ref renderContext, rotationArcMesh, ref rotationDrawTransform, ref rotationWorld, Vector3.UnitX, xAxisMaterialRotate, sortOrder);

                // Y axis
                MaterialInstance yAxisMaterialRotate = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                DrawRotationAxis(ref renderContext, rotationArcMesh, ref rotationDrawTransform, ref rotationWorld, Vector3.UnitY, yAxisMaterialRotate, sortOrder);

                // Z axis
                MaterialInstance zAxisMaterialRotate = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                DrawRotationAxis(ref renderContext, rotationArcMesh, ref rotationDrawTransform, ref rotationWorld, Vector3.UnitZ, zAxisMaterialRotate, sortOrder);
                DrawRotationTrackballPoints(ref renderContext, rotationTrackballPointMesh, ref rotationDrawTransform, ref rotationWorld, sortOrder);

                break;
            }

            case Mode.Scale:
            {
                if (!_modelScaleAxis || !_modelScaleAxis.IsLoaded)
                    break;
                var scaleAxisMesh = _modelScaleAxis.LODs[0].Meshes[0];

                // X axis
                Matrix.RotationY(-Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref mx1, out m3);
                MaterialInstance xAxisMaterialRotate = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                scaleAxisMesh.Draw(ref renderContext, xAxisMaterialRotate, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Y axis
                Matrix.RotationX(Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance yAxisMaterialRotate = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                scaleAxisMesh.Draw(ref renderContext, yAxisMaterialRotate, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Z axis
                Matrix.RotationX(Mathf.Pi, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance zAxisMaterialRotate = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                scaleAxisMesh.Draw(ref renderContext, zAxisMaterialRotate, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // XY plane
                m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationX(Mathf.PiOverTwo), new Vector3(boxSize * boxScale, boxSize * boxScale, 0.0f));
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance xyPlaneMaterialScale = (_activeAxis == Axis.XY && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                cubeMesh.Draw(ref renderContext, xyPlaneMaterialScale, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // ZX plane
                m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.Identity, new Vector3(boxSize * boxScale, 0.0f, boxSize * boxScale));
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance zxPlaneMaterialScale = (_activeAxis == Axis.ZX && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                cubeMesh.Draw(ref renderContext, zxPlaneMaterialScale, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // YZ plane
                m2 = Matrix.Transformation(new Vector3(boxSize, boxSize * 0.1f, boxSize), Quaternion.RotationZ(Mathf.PiOverTwo), new Vector3(0.0f, boxSize * boxScale, boxSize * boxScale));
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance yzPlaneMaterialScale = (_activeAxis == Axis.YZ && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                cubeMesh.Draw(ref renderContext, yzPlaneMaterialScale, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Center box
                if (_vertexSnapObject == null)
                {
                    Matrix.Scaling(gizmoModelsScale2RealGizmoSize, out m2);
                    Matrix.Multiply(ref m2, ref m1, out m3);
                    sphereMesh.Draw(ref renderContext, isCenter ? _materialAxisFocus : _materialSphere, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
                }

                break;
            }
            }

            DrawVertexSnapPointHighlights(ref renderContext, sphereMesh, (sbyte)(sortOrder + 3));
        }
    }
}
