// Copyright (c) Wojciech Figat. All rights reserved.

using System.Globalization;
using FlaxEditor.Tools.CSG.Placement;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Tools
{
    /// <summary>Stages of the oriented-box draw interaction.</summary>
    public enum CSGBoxDrawStage
    {
        /// <summary>Shows the placement cursor without owning input.</summary>
        Hover,
        /// <summary>Solves width and depth in working-plane coordinates.</summary>
        Footprint,
        /// <summary>Solves signed height along the working-plane normal.</summary>
        Height,
    }

    /// <summary>Dimension currently targeted by numeric entry.</summary>
    public enum CSGBoxNumericDimension
    {
        /// <summary>Local X footprint dimension.</summary>
        Width,
        /// <summary>Local Z footprint dimension.</summary>
        Depth,
        /// <summary>Signed local Y extrusion.</summary>
        Height,
    }

    /// <summary>
    /// Stateful input adapter for staged box creation. Scene mutation is deliberately left to the owning gizmo.
    /// </summary>
    public sealed class CSGBoxDrawTool
    {
        private CSGWorkingPlane _plane;
        private Float2 _anchor;
        private Float2 _pointer;
        private float _height;
        private float? _widthOverride;
        private float? _depthOverride;
        private float? _heightOverride;
        private string _numericText = string.Empty;
        private bool _square;
        private bool _symmetricFootprint;
        private bool _symmetricExtrusion;

        /// <summary>Gets the active stage.</summary>
        public CSGBoxDrawStage Stage { get; private set; } = CSGBoxDrawStage.Hover;

        /// <summary>Gets whether an authoring transaction owns this tool.</summary>
        public bool IsInteracting => Stage != CSGBoxDrawStage.Hover;

        /// <summary>Gets the numeric-entry target.</summary>
        public CSGBoxNumericDimension NumericDimension { get; private set; } = CSGBoxNumericDimension.Width;

        /// <summary>Gets concise dimensions for the viewport overlay.</summary>
        public string StatusText
        {
            get
            {
                GetSolvedDimensions(out float width, out float depth, out float height);
                string entry = _numericText.Length == 0 ? string.Empty : $"  {NumericDimension}={_numericText}_";
                return Stage == CSGBoxDrawStage.Hover
                    ? "Click-drag footprint"
                    : $"{Stage}  W {width:0.###}  D {depth:0.###}  H {height:0.###}{entry}";
            }
        }

        /// <summary>Updates the hover plane and pointer without beginning a transaction.</summary>
        public void UpdateHover(ref CSGWorkingPlane plane, Vector3 point)
        {
            if (IsInteracting)
                return;
            _plane = plane;
            _anchor = _pointer = plane.ToPlane(point);
        }

        /// <summary>Begins a footprint drag on the frozen working plane.</summary>
        public bool Begin(ref CSGWorkingPlane plane, Vector3 point, bool square, bool symmetricFootprint, bool symmetricExtrusion)
        {
            if (IsInteracting || !plane.IsValid)
                return false;
            _plane = plane;
            _anchor = _pointer = plane.ToPlane(point);
            _height = plane.Spacing;
            _square = square;
            _symmetricFootprint = symmetricFootprint;
            _symmetricExtrusion = symmetricExtrusion;
            _widthOverride = null;
            _depthOverride = null;
            _heightOverride = null;
            _numericText = string.Empty;
            NumericDimension = CSGBoxNumericDimension.Width;
            Stage = CSGBoxDrawStage.Footprint;
            return true;
        }

        /// <summary>Updates live modifier state without changing the frozen plane.</summary>
        public void SetModifiers(bool square, bool symmetricFootprint, bool symmetricExtrusion)
        {
            _square = square;
            _symmetricFootprint = symmetricFootprint;
            _symmetricExtrusion = symmetricExtrusion;
        }

        /// <summary>Updates the footprint pointer in plane-local coordinates.</summary>
        public void UpdateFootprint(Vector3 point)
        {
            if (Stage == CSGBoxDrawStage.Footprint)
                _pointer = _plane.ToPlane(point);
        }

        /// <summary>Locks a valid footprint and advances to height.</summary>
        public bool CompleteFootprint()
        {
            if (Stage != CSGBoxDrawStage.Footprint || !HasValidFootprint())
                return false;
            Stage = CSGBoxDrawStage.Height;
            NumericDimension = CSGBoxNumericDimension.Height;
            _numericText = string.Empty;
            return true;
        }

        /// <summary>Updates signed height from a pointer ray.</summary>
        public bool UpdateHeight(ref Ray pointerRay, Vector3 viewDirection, bool snap, float increment)
        {
            if (Stage != CSGBoxDrawStage.Height)
                return false;
            GetFootprintBounds(out var minimum, out var maximum);
            var center = _plane.ToWorld((minimum + maximum) * 0.5f);
            if (!CSGBoxPlacementSolver.TrySolveHeight(ref _plane, center, ref pointerRay, viewDirection, out var height))
                return false;
            _height = snap ? CSGBoxPlacementSolver.SnapDimension(height, increment) : height;
            return true;
        }

        /// <summary>Sets an exact numeric dimension. Used by keyboard input and pure tests.</summary>
        public bool SetNumericOverride(CSGBoxNumericDimension dimension, float value)
        {
            if (float.IsNaN(value) || float.IsInfinity(value))
                return false;
            if (dimension != CSGBoxNumericDimension.Height && value <= CSGBoxPlacementSolver.MinimumDimension)
                return false;
            switch (dimension)
            {
            case CSGBoxNumericDimension.Width:
                _widthOverride = Mathf.Abs(value);
                break;
            case CSGBoxNumericDimension.Depth:
                _depthOverride = Mathf.Abs(value);
                break;
            case CSGBoxNumericDimension.Height:
                if (Mathf.Abs(value) <= CSGBoxPlacementSolver.MinimumDimension)
                    return false;
                _heightOverride = value;
                break;
            default:
                return false;
            }
            return true;
        }

        /// <summary>
        /// Handles numeric-entry keys. Return requests commit only after applying a height value.
        /// </summary>
        public bool OnKeyDown(KeyboardKeys key, out bool requestCommit)
        {
            requestCommit = false;
            if (!IsInteracting)
                return false;

            if (TryGetDigit(key, out char digit))
            {
                _numericText += digit;
                return true;
            }
            if ((key == KeyboardKeys.Period || key == KeyboardKeys.NumpadDecimal) && !_numericText.Contains("."))
            {
                _numericText = _numericText.Length == 0 ? "0." : _numericText + ".";
                return true;
            }
            if ((key == KeyboardKeys.Minus || key == KeyboardKeys.NumpadSubtract) && NumericDimension == CSGBoxNumericDimension.Height)
            {
                _numericText = _numericText.StartsWith("-") ? _numericText.Substring(1) : "-" + _numericText;
                return true;
            }
            if (key == KeyboardKeys.Backspace)
            {
                if (_numericText.Length != 0)
                    _numericText = _numericText.Substring(0, _numericText.Length - 1);
                return true;
            }
            if (key == KeyboardKeys.Tab && Stage == CSGBoxDrawStage.Footprint)
            {
                ApplyNumericText();
                NumericDimension = NumericDimension == CSGBoxNumericDimension.Width ? CSGBoxNumericDimension.Depth : CSGBoxNumericDimension.Width;
                _numericText = string.Empty;
                return true;
            }
            if (key != KeyboardKeys.Return)
                return false;

            if (_numericText.Length == 0)
                return false;

            bool applied = ApplyNumericText();
            if (Stage == CSGBoxDrawStage.Footprint)
            {
                if (NumericDimension == CSGBoxNumericDimension.Width)
                {
                    NumericDimension = CSGBoxNumericDimension.Depth;
                    _numericText = string.Empty;
                }
                else if (applied && CompleteFootprint())
                {
                    _numericText = string.Empty;
                }
            }
            else if (Stage == CSGBoxDrawStage.Height && applied)
            {
                requestCommit = true;
            }
            return true;
        }

        /// <summary>Gets the final positive-size placement.</summary>
        public bool TryGetPlacement(out CSGBoxPlacement placement)
        {
            placement = default;
            var pointer = ApplyFootprintOverrides();
            float height = _heightOverride ?? _height;
            return Stage == CSGBoxDrawStage.Height &&
                   CSGBoxPlacementSolver.TrySolve(ref _plane, _anchor, pointer, height, _square, _symmetricFootprint, _symmetricExtrusion, out placement);
        }

        /// <summary>Draws the footprint, translucent volume, outline, and dimensions.</summary>
        public void Draw(float hoverMarkerSize = 0.0f)
        {
            if (Stage == CSGBoxDrawStage.Hover)
            {
                var point = _plane.ToWorld(_anchor);
                float size = hoverMarkerSize > Mathf.Epsilon ? hoverMarkerSize : Mathf.Clamp(_plane.Spacing * 0.15f, 0.75f, 7.5f);
                var hoverColor = new Color(1.0f, 0.82f, 0.12f, 1.0f);
                var cube = new OrientedBoundingBox(new Vector3(-size * 0.5f), new Vector3(size * 0.5f))
                {
                    Transformation = new Transform(point, Quaternion.LookRotation(-_plane.Bitangent, _plane.Normal)),
                };
                DebugDraw.DrawBox(cube, hoverColor, 0.0f, false);
                DebugDraw.DrawWireBox(cube, new Color(0.55f, 0.38f, 0.02f, 1.0f), 0.0f, false);
                return;
            }
            if (!IsInteracting)
                return;

            GetFootprintBounds(out var minimum, out var maximum);
            var p0 = _plane.ToWorld(minimum);
            var p1 = _plane.ToWorld(new Float2(maximum.X, minimum.Y));
            var p2 = _plane.ToWorld(maximum);
            var p3 = _plane.ToWorld(new Float2(minimum.X, maximum.Y));
            bool validFootprint = HasValidFootprint();
            var color = validFootprint ? new Color(0.15f, 0.85f, 1.0f, 0.95f) : new Color(1.0f, 0.18f, 0.08f, 0.95f);
            DebugDraw.DrawLine(p0, p1, color, 0.0f, false);
            DebugDraw.DrawLine(p1, p2, color, 0.0f, false);
            DebugDraw.DrawLine(p2, p3, color, 0.0f, false);
            DebugDraw.DrawLine(p3, p0, color, 0.0f, false);

            if (TryGetPlacement(out var placement))
            {
                var box = placement.ToOrientedBox();
                var fill = color;
                fill.A = 0.12f;
                DebugDraw.DrawBox(box, fill, 0.0f, true);
                DebugDraw.DrawWireBox(box, color, 0.0f, false);
                DebugDraw.DrawText(StatusText, placement.Center, Color.White, 20, 0.0f, 0.75f);
            }
        }

        /// <summary>Clears all transient placement state.</summary>
        public void Reset()
        {
            Stage = CSGBoxDrawStage.Hover;
            NumericDimension = CSGBoxNumericDimension.Width;
            _numericText = string.Empty;
            _widthOverride = null;
            _depthOverride = null;
            _heightOverride = null;
        }

        private bool ApplyNumericText()
        {
            return float.TryParse(_numericText, NumberStyles.Float, CultureInfo.InvariantCulture, out float value) && SetNumericOverride(NumericDimension, value);
        }

        private bool HasValidFootprint()
        {
            GetFootprintBounds(out var minimum, out var maximum);
            return maximum.X - minimum.X > CSGBoxPlacementSolver.MinimumDimension &&
                   maximum.Y - minimum.Y > CSGBoxPlacementSolver.MinimumDimension;
        }

        private void GetSolvedDimensions(out float width, out float depth, out float height)
        {
            GetFootprintBounds(out var minimum, out var maximum);
            width = maximum.X - minimum.X;
            depth = maximum.Y - minimum.Y;
            height = (_heightOverride ?? _height) * (_symmetricExtrusion ? 2.0f : 1.0f);
        }

        private void GetFootprintBounds(out Float2 minimum, out Float2 maximum)
        {
            var pointer = ApplyFootprintOverrides();
            CSGBoxPlacementSolver.SolveFootprint(_anchor, pointer, _square, _symmetricFootprint, out minimum, out maximum);
        }

        private Float2 ApplyFootprintOverrides()
        {
            var delta = _pointer - _anchor;
            if (_widthOverride.HasValue)
            {
                float sign = delta.X < 0.0f ? -1.0f : 1.0f;
                delta.X = sign * (_symmetricFootprint ? _widthOverride.Value * 0.5f : _widthOverride.Value);
            }
            if (_depthOverride.HasValue)
            {
                float sign = delta.Y < 0.0f ? -1.0f : 1.0f;
                delta.Y = sign * (_symmetricFootprint ? _depthOverride.Value * 0.5f : _depthOverride.Value);
            }
            return _anchor + delta;
        }

        private static bool TryGetDigit(KeyboardKeys key, out char digit)
        {
            switch (key)
            {
            case KeyboardKeys.Alpha0:
            case KeyboardKeys.Numpad0: digit = '0'; return true;
            case KeyboardKeys.Alpha1:
            case KeyboardKeys.Numpad1: digit = '1'; return true;
            case KeyboardKeys.Alpha2:
            case KeyboardKeys.Numpad2: digit = '2'; return true;
            case KeyboardKeys.Alpha3:
            case KeyboardKeys.Numpad3: digit = '3'; return true;
            case KeyboardKeys.Alpha4:
            case KeyboardKeys.Numpad4: digit = '4'; return true;
            case KeyboardKeys.Alpha5:
            case KeyboardKeys.Numpad5: digit = '5'; return true;
            case KeyboardKeys.Alpha6:
            case KeyboardKeys.Numpad6: digit = '6'; return true;
            case KeyboardKeys.Alpha7:
            case KeyboardKeys.Numpad7: digit = '7'; return true;
            case KeyboardKeys.Alpha8:
            case KeyboardKeys.Numpad8: digit = '8'; return true;
            case KeyboardKeys.Alpha9:
            case KeyboardKeys.Numpad9: digit = '9'; return true;
            default: digit = default; return false;
            }
        }
    }
}
