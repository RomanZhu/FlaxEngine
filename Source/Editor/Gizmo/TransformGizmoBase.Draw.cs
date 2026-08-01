// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Options;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        // Models
        private Model _modelTranslationAxis;
        private Model _modelScaleAxis;
        private Model _modelRotationArc;
        private Model _modelRotationSphere;
        private Model _modelSphere;
        private Model _modelCube;

        // Materials
        private MaterialInstance _materialAxisX;
        private MaterialInstance _materialAxisY;
        private MaterialInstance _materialAxisZ;
        private MaterialInstance _materialAxisFocus;
        private MaterialInstance _materialAxisBack;
        private MaterialInstance _materialTrackballFocus;
        private MaterialBase _materialSphere;

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
            _materialAxisBack = _materialAxisX.CreateVirtualInstance();
            _materialAxisBack.SetParameterValue(_colorParamName, new Color(0.42f, 0.42f, 0.42f, 1.0f));
            _materialTrackballFocus = _materialAxisFocus.CreateVirtualInstance();
            _materialTrackballFocus.SetParameterValue(_opacityParamName, _rotationTrackballOpacity);

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

        private void OnEditorOptionsChanged(EditorOptions options)
        {
            UpdateGizmoBrightness(options);

            float opacity = options.Visual.TransformGizmoOpacity;
            _materialAxisX.SetParameterValue(_opacityParamName, opacity);
            _materialAxisY.SetParameterValue(_opacityParamName, opacity);
            _materialAxisZ.SetParameterValue(_opacityParamName, opacity);
            _materialAxisBack.SetParameterValue(_opacityParamName, opacity * _rotationSphereOpacity);
            _materialTrackballFocus.SetParameterValue(_opacityParamName, opacity * _rotationTrackballOpacity);
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

        private Vector3 GetRotateToViewLocal()
        {
            Vector3 toView;
            if (Owner.Viewport.UseOrthographicProjection)
            {
                var viewDirection = (Vector3)Owner.ViewDirection;
                _gizmoWorld.WorldToLocalVector(ref viewDirection, out toView);
                toView = -toView;
            }
            else
            {
                var viewPosition = Owner.ViewPosition;
                _gizmoWorld.WorldToLocal(ref viewPosition, out toView);
            }

            if (toView.LengthSquared < 0.0001f)
                toView = Vector3.Forward;
            else
                toView.Normalize();
            return toView;
        }

        private Vector3 GetRotateFrontDirectionLocal(Vector3 normal)
        {
            Vector3 toView = GetRotateToViewLocal();
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

        private void DrawRotationAxis(ref RenderContext renderContext, Mesh arcMesh, ref Matrix world, Vector3 normal, MaterialBase frontMaterial, sbyte sortOrder)
        {
            Vector3 frontDirection = GetRotateFrontDirectionLocal(normal);
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
                Matrix.RotationY(-Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance xAxisMaterialTransform = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                transAxisMesh.Draw(ref renderContext, xAxisMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Y axis
                Matrix.RotationX(Mathf.PiOverTwo, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance yAxisMaterialTransform = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                transAxisMesh.Draw(ref renderContext, yAxisMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                // Z axis
                Matrix.RotationX(Mathf.Pi, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                MaterialInstance zAxisMaterialTransform = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                transAxisMesh.Draw(ref renderContext, zAxisMaterialTransform, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

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
                Matrix.Scaling(gizmoModelsScale2RealGizmoSize, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                sphereMesh.Draw(ref renderContext, isCenter ? _materialAxisFocus : _materialSphere, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                break;
            }

            case Mode.Rotate:
            {
                if (!_modelRotationArc || !_modelRotationArc.IsLoaded || !_modelRotationSphere || !_modelRotationSphere.IsLoaded)
                    break;
                var rotationArcMesh = _modelRotationArc.LODs[0].Meshes[0];
                var rotationSphereMesh = _modelRotationSphere.LODs[0].Meshes[0];

                // Trackball and background
                DrawRotationSphere(ref renderContext, rotationSphereMesh, ref world, _materialAxisBack, (sbyte)(sortOrder - 2));
                if (isCenter && !_isDisabled)
                    DrawRotationSphere(ref renderContext, rotationSphereMesh, ref world, _materialTrackballFocus, (sbyte)(sortOrder - 1));

                // X axis
                MaterialInstance xAxisMaterialRotate = (isXAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisX;
                DrawRotationAxis(ref renderContext, rotationArcMesh, ref world, Vector3.UnitX, xAxisMaterialRotate, sortOrder);

                // Y axis
                MaterialInstance yAxisMaterialRotate = (isYAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisY;
                DrawRotationAxis(ref renderContext, rotationArcMesh, ref world, Vector3.UnitY, yAxisMaterialRotate, sortOrder);

                // Z axis
                MaterialInstance zAxisMaterialRotate = (isZAxis && !_isDisabled) ? _materialAxisFocus : _materialAxisZ;
                DrawRotationAxis(ref renderContext, rotationArcMesh, ref world, Vector3.UnitZ, zAxisMaterialRotate, sortOrder);

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
                Matrix.Scaling(gizmoModelsScale2RealGizmoSize, out m2);
                Matrix.Multiply(ref m2, ref m1, out m3);
                sphereMesh.Draw(ref renderContext, isCenter ? _materialAxisFocus : _materialSphere, ref m3, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);

                break;
            }
            }

            // Vertex snapping
            if (_vertexSnapObject != null || _vertexSnapObjectTo != null)
            {
                Transform t = _vertexSnapObject?.Transform ?? _vertexSnapObjectTo.Transform;
                Vector3 p = t.LocalToWorld(_vertexSnapObject != null ? _vertexSnapPoint : _vertexSnapPointTo);
                Matrix matrix = new Transform(p, t.Orientation, new Float3(gizmoModelsScale2RealGizmoSize)).GetWorld();
                cubeMesh.Draw(ref renderContext, _materialSphere, ref matrix, StaticFlags.None, true, DrawPass.Default, 0.0f, sortOrder);
            }
        }
    }
}
