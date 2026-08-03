// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEngine.GUI
{
    /// <summary>
    /// GUI control that contains two child panels and the splitter between them.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    [HideInEditor]
    public class SplitPanel : ContainerControl
    {
        /// <summary>
        /// The splitter size (in pixels).
        /// </summary>
        public const int SplitterSize = 4;

        /// <summary>
        /// The splitter half size (in pixels).
        /// </summary>
        private const int SplitterSizeHalf = SplitterSize / 2;

        private Orientation _orientation;
        private float _splitterValue;
        private float _minimumPanelSize;
        private Rectangle _splitterRect;
        private bool _splitterClicked, _mouseOverSplitter;
        private bool _cursorChanged;
        private DragBranchState _dragPanel1State, _dragPanel2State;
        private DragBranchState _dragAncestorStates;
        private DragBranchState _capturedDragState;
        // Overflow maxima remain sticky for the whole drag; pushed lengths account for ancestor relayout.
        private float _dragPositiveOverflow, _dragNegativeOverflow;
        private float _dragPositivePushed, _dragNegativePushed;

        private sealed class DragBranchState
        {
            public SplitPanel Splitter;
            public bool PreservePanel1;
            public float PreservedLength;
            public DragBranchState Next;
            public DragBranchState RootNext;
        }

        /// <summary>
        /// The first panel (left or upper based on Orientation).
        /// </summary>
        public readonly Panel Panel1;

        /// <summary>
        /// The second panel.
        /// </summary>
        public readonly Panel Panel2;

        /// <summary>
        /// Gets or sets the panel orientation.
        /// </summary>
        /// <value>
        /// The orientation.
        /// </value>
        public Orientation Orientation
        {
            get => _orientation;
            set
            {
                if (_orientation != value)
                {
                    _orientation = value;
                    UpdateSplitRect();
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets the splitter value (always in range [0; 1]).
        /// </summary>
        /// <value>
        /// The splitter value (always in range [0; 1]).
        /// </value>
        public float SplitterValue
        {
            get => _splitterValue;
            set
            {
                value = Mathf.Saturate(value);
                if (!Mathf.NearEqual(_splitterValue, value))
                {
                    // Set new value
                    _splitterValue = value;

                    // Calculate rectangle and update panels
                    UpdateSplitRect();
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets the minimum size of each panel along the split axis.
        /// </summary>
        /// <value>
        /// The minimum panel size. A value of zero disables minimum size enforcement.
        /// </value>
        public float MinimumPanelSize
        {
            get => _minimumPanelSize;
            set
            {
                value = Mathf.Max(value, 0.0f);
                if (!Mathf.NearEqual(_minimumPanelSize, value))
                {
                    _minimumPanelSize = value;
                    UpdateSplitRect();
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets whether to preserve this split ratio when resized from the leading edge by a neighboring splitter.
        /// </summary>
        /// <value>
        /// <c>true</c> to preserve this split ratio when resized from the leading edge; otherwise, <c>false</c>.
        /// </value>
        public bool PreserveRatioWhenResizedFromLeadingEdge { get; set; }

        /// <summary>
        /// Initializes a new instance of the <see cref="SplitPanel"/> class.
        /// </summary>
        /// <param name="orientation">The orientation.</param>
        /// <param name="panel1Scroll">The panel1 scroll bars.</param>
        /// <param name="panel2Scroll">The panel2 scroll bars.</param>
        public SplitPanel(Orientation orientation = Orientation.Horizontal, ScrollBars panel1Scroll = ScrollBars.Both, ScrollBars panel2Scroll = ScrollBars.Both)
        {
            AutoFocus = false;

            _orientation = orientation;
            _splitterValue = 0.5f;

            Panel1 = new Panel(panel1Scroll);
            Panel2 = new Panel(panel2Scroll);

            Panel1.Parent = this;
            Panel2.Parent = this;

            UpdateSplitRect();
        }

        private float GetMinimumLength(Orientation orientation)
        {
            var panel1Minimum = GetPanelMinimumLength(Panel1, orientation);
            var panel2Minimum = GetPanelMinimumLength(Panel2, orientation);
            if (panel1Minimum <= 0.0f && panel2Minimum <= 0.0f)
                return 0.0f;
            return orientation == _orientation ? panel1Minimum + panel2Minimum + SplitterSize : Mathf.Max(panel1Minimum, panel2Minimum);
        }

        private float GetPanelMinimumLength(Control panel, Orientation orientation)
        {
            var result = _minimumPanelSize;
            if (panel is SplitPanel splitPanel)
            {
                result = Mathf.Max(result, splitPanel.GetMinimumLength(orientation));
            }
            else if (panel is ContainerControl container)
            {
                for (int i = 0; i < container.Children.Count; i++)
                    result = Mathf.Max(result, GetNestedMinimumLength(container.Children[i], orientation));
            }
            return result;
        }

        private static float GetNestedMinimumLength(Control control, Orientation orientation)
        {
            if (control is SplitPanel splitPanel)
                return splitPanel.GetMinimumLength(orientation);

            if (control is ContainerControl container)
            {
                var result = 0.0f;
                for (int i = 0; i < container.Children.Count; i++)
                    result = Mathf.Max(result, GetNestedMinimumLength(container.Children[i], orientation));
                return result;
            }

            return 0.0f;
        }

        private Control GetEdgeChild(ContainerControl container, bool trailing)
        {
            Control result = null;
            var edge = trailing ? float.MinValue : float.MaxValue;
            for (int i = 0; i < container.Children.Count; i++)
            {
                var child = container.Children[i];
                if (!child.Visible || child is ScrollBar)
                    continue;
                var childEdge = _orientation == Orientation.Horizontal
                    ? (trailing ? child.Right : child.Left)
                    : (trailing ? child.Bottom : child.Top);
                if (result == null || (trailing ? childEdge >= edge : childEdge <= edge))
                {
                    result = child;
                    edge = childEdge;
                }
            }
            return result;
        }

        private SplitPanel FindEdgeSplit(Control branch, bool trailing)
        {
            var control = branch;
            while (control != null && !(control is SplitPanel))
            {
                if (!(control is ContainerControl container))
                    return null;
                control = GetEdgeChild(container, trailing);
            }
            return control as SplitPanel;
        }

        private DragBranchState CaptureDragBranch(Control branch, bool trailing)
        {
            var splitter = FindEdgeSplit(branch, trailing);
            if (splitter == null || splitter._orientation != _orientation)
                return null;
            if (splitter.PreserveRatioWhenResizedFromLeadingEdge && !trailing)
                return null;
            if (splitter._capturedDragState != null)
                return splitter._capturedDragState;

            var preservePanel1 = trailing;
            var preservedPanel = preservePanel1 ? splitter.Panel1 : splitter.Panel2;
            var state = new DragBranchState
            {
                Splitter = splitter,
                PreservePanel1 = preservePanel1,
                PreservedLength = GetPanelLength(preservedPanel, splitter._orientation),
            };
            splitter._capturedDragState = state;

            var adjacentPanel = preservePanel1 ? splitter.Panel2 : splitter.Panel1;
            state.Next = CaptureDragBranch(adjacentPanel, !preservePanel1);
            return state;
        }

        private void CaptureAncestorDragBranch(Control branch, bool trailing)
        {
            var splitter = FindEdgeSplit(branch, trailing);
            if (splitter == null || splitter._orientation != _orientation || splitter._capturedDragState != null)
                return;

            var state = CaptureDragBranch(branch, trailing);
            if (state != null)
            {
                state.RootNext = _dragAncestorStates;
                _dragAncestorStates = state;
            }
        }

        private static void ClearCapturedDragBranch(DragBranchState state)
        {
            while (state != null)
            {
                var next = state.Next;
                state.Splitter._capturedDragState = null;
                state.Next = null;
                state = next;
            }
        }

        private static float GetCapturedBranchMinimum(DragBranchState state)
        {
            var splitter = state.Splitter;
            var farPanel = state.PreservePanel1 ? splitter.Panel1 : splitter.Panel2;
            var adjacentPanel = state.PreservePanel1 ? splitter.Panel2 : splitter.Panel1;
            var farMinimum = splitter.GetPanelMinimumLength(farPanel, splitter._orientation);
            var adjacentMinimum = state.Next != null
                ? GetCapturedBranchMinimum(state.Next)
                : splitter.GetPanelMinimumLength(adjacentPanel, splitter._orientation);
            return farMinimum + SplitterSize + adjacentMinimum;
        }

        private static float GetPanelLength(Control panel, Orientation orientation)
        {
            return orientation == Orientation.Horizontal ? panel.Width : panel.Height;
        }

        private static int GetCapturedSplit(DragBranchState state, float length)
        {
            return Mathf.RoundToInt(state.PreservePanel1
                ? state.PreservedLength + SplitterSizeHalf
                : length - state.PreservedLength - SplitterSizeHalf);
        }

        private int GetEffectiveSplit()
        {
            var length = Mathf.Max(_orientation == Orientation.Horizontal ? Width : Height, 0.0f);
            var split = Mathf.RoundToInt(_splitterValue * length);
            var panel1Minimum = GetPanelMinimumLength(Panel1, _orientation);
            var panel2Minimum = GetPanelMinimumLength(Panel2, _orientation);
            var capturedState = _capturedDragState;
            if (_splitterClicked)
            {
                if (_dragPanel1State != null)
                    panel1Minimum = Mathf.Max(panel1Minimum, GetCapturedBranchMinimum(_dragPanel1State));
                if (_dragPanel2State != null)
                    panel2Minimum = Mathf.Max(panel2Minimum, GetCapturedBranchMinimum(_dragPanel2State));
            }
            else if (capturedState != null)
            {
                var farPanel = capturedState.PreservePanel1 ? Panel1 : Panel2;
                var adjacentPanel = capturedState.PreservePanel1 ? Panel2 : Panel1;
                var farMinimum = GetPanelMinimumLength(farPanel, _orientation);
                var adjacentMinimum = capturedState.Next != null
                    ? GetCapturedBranchMinimum(capturedState.Next)
                    : GetPanelMinimumLength(adjacentPanel, _orientation);
                if (capturedState.PreservePanel1)
                {
                    panel1Minimum = Mathf.Max(panel1Minimum, farMinimum);
                    panel2Minimum = Mathf.Max(panel2Minimum, adjacentMinimum);
                }
                else
                {
                    panel1Minimum = Mathf.Max(panel1Minimum, adjacentMinimum);
                    panel2Minimum = Mathf.Max(panel2Minimum, farMinimum);
                }
                split = GetCapturedSplit(capturedState, length);
            }

            var minimumLength = panel1Minimum + panel2Minimum;
            if (minimumLength > 0.0f)
            {
                var availableLength = Mathf.Max(length - SplitterSize, 0.0f);
                if (availableLength >= minimumLength)
                {
                    var minimumSplit = Mathf.CeilToInt(panel1Minimum + SplitterSizeHalf);
                    var maximumSplit = Mathf.FloorToInt(length - panel2Minimum - SplitterSizeHalf);
                    if (minimumSplit <= maximumSplit)
                        split = Mathf.Clamp(split, minimumSplit, maximumSplit);
                    else
                    {
                        var panel1Length = availableLength * panel1Minimum / minimumLength;
                        split = Mathf.RoundToInt(panel1Length + SplitterSizeHalf);
                    }
                }
                else
                {
                    var panel1Length = availableLength * panel1Minimum / minimumLength;
                    split = Mathf.RoundToInt(panel1Length + SplitterSizeHalf);
                }
            }

            return Mathf.Clamp(split, 0, Mathf.RoundToInt(length));
        }

        private void UpdateSplitRect()
        {
            UpdateSplitRect(GetEffectiveSplit());
        }

        private void UpdateSplitRect(int split)
        {
            var width = Mathf.Max(Width, 0.0f);
            var height = Mathf.Max(Height, 0.0f);
            if (_orientation == Orientation.Horizontal)
            {
                var splitterStart = Mathf.Clamp(split - (float)SplitterSizeHalf, 0.0f, width);
                _splitterRect = new Rectangle(splitterStart, 0.0f, SplitterSize, height);
            }
            else
            {
                var splitterStart = Mathf.Clamp(split - (float)SplitterSizeHalf, 0.0f, height);
                _splitterRect = new Rectangle(0.0f, splitterStart, width, SplitterSize);
            }
        }

        private static float ApplyAncestorSplitOverflow(SplitPanel splitter, float overflow, bool positive)
        {
            var length = splitter._orientation == Orientation.Horizontal ? splitter.Width : splitter.Height;
            if (length <= 0.0f)
                return overflow;

            var split = splitter.GetEffectiveSplit();
            var requestedSplit = positive ? split + overflow : split - overflow;
            splitter.SplitterValue = requestedSplit / length;

            var effectiveSplit = splitter.GetEffectiveSplit();
            if (length > 0.0f)
                splitter._splitterValue = effectiveSplit / length;
            var moved = positive ? effectiveSplit - split : split - effectiveSplit;
            return moved > 0.0f ? Mathf.Max(overflow - moved, 0.0f) : overflow;
        }

        private float ApplyAncestorOverflow(float overflow, bool positive)
        {
            Control branch = this;
            while (overflow > 0.0f && branch.Parent != null)
            {
                var parent = branch.Parent;
                if (parent is SplitPanel splitter && splitter._orientation == _orientation)
                {
                    var branchInPanel1 = branch == splitter.Panel1;
                    var branchInPanel2 = branch == splitter.Panel2;
                    if (positive ? branchInPanel1 : branchInPanel2)
                    {
                        CaptureAncestorDragBranch(positive ? splitter.Panel2 : splitter.Panel1, !positive);
                        overflow = ApplyAncestorSplitOverflow(splitter, overflow, positive);
                    }
                }
                branch = parent;
            }
            return overflow;
        }

        private void ApplyDragOverflow(float requestedSplit, int effectiveSplit)
        {
            var overflow = requestedSplit - effectiveSplit;
            if (overflow > 0.0f)
            {
                var totalOverflow = overflow + _dragPositivePushed;
                if (totalOverflow > _dragPositiveOverflow)
                {
                    var additionalOverflow = totalOverflow - _dragPositiveOverflow;
                    _dragPositiveOverflow = totalOverflow;
                    var remaining = ApplyAncestorOverflow(additionalOverflow, true);
                    _dragPositivePushed += additionalOverflow - remaining;
                }
            }
            else if (overflow < 0.0f)
            {
                overflow = -overflow;
                var totalOverflow = overflow + _dragNegativePushed;
                if (totalOverflow > _dragNegativeOverflow)
                {
                    var additionalOverflow = totalOverflow - _dragNegativeOverflow;
                    _dragNegativeOverflow = totalOverflow;
                    var remaining = ApplyAncestorOverflow(additionalOverflow, false);
                    _dragNegativePushed += additionalOverflow - remaining;
                }
            }
        }

        private void StartTracking()
        {
            // Start move
            _dragPositiveOverflow = 0.0f;
            _dragNegativeOverflow = 0.0f;
            _dragPositivePushed = 0.0f;
            _dragNegativePushed = 0.0f;
            _splitterClicked = true;
            _dragPanel1State = CaptureDragBranch(Panel1, true);
            _dragPanel2State = CaptureDragBranch(Panel2, false);

            // Start capturing mouse
            StartMouseCapture();
        }

        private void EndTracking()
        {
            if (_splitterClicked)
            {
                // Clear flag and captured branch state
                _splitterClicked = false;
                ClearCapturedDragBranch(_dragPanel1State);
                ClearCapturedDragBranch(_dragPanel2State);
                var ancestorState = _dragAncestorStates;
                while (ancestorState != null)
                {
                    var next = ancestorState.RootNext;
                    ClearCapturedDragBranch(ancestorState);
                    ancestorState.RootNext = null;
                    ancestorState = next;
                }
                _dragAncestorStates = null;
                _dragPanel1State = null;
                _dragPanel2State = null;
                _dragPositiveOverflow = 0.0f;
                _dragNegativeOverflow = 0.0f;
                _dragPositivePushed = 0.0f;
                _dragNegativePushed = 0.0f;
                PerformLayout();

                // End capturing mouse
                EndMouseCapture();
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            // Draw splitter
            var style = Style.Current;
            Render2D.FillRectangle(_splitterRect, _splitterClicked ? style.BackgroundSelected : _mouseOverSplitter ? style.BackgroundHighlighted : style.SecondaryBackground);
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            EndTracking();

            base.OnLostFocus();
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            _mouseOverSplitter = _splitterRect.Contains(location);

            if (_splitterClicked)
            {
                var length = _orientation == Orientation.Horizontal ? Width : Height;
                if (length > 0.0f)
                {
                    var requestedSplit = _orientation == Orientation.Horizontal ? location.X : location.Y;
                    SplitterValue = requestedSplit / length;
                    ApplyDragOverflow(requestedSplit, GetEffectiveSplit());

                    var updatedLength = _orientation == Orientation.Horizontal ? Width : Height;
                    if (updatedLength > 0.0f && !Mathf.NearEqual(updatedLength, length))
                    {
                        _splitterValue = Mathf.Saturate(requestedSplit / updatedLength);
                        PerformLayout();
                    }
                }
                Cursor = _orientation == Orientation.Horizontal ? CursorType.SizeWE : CursorType.SizeNS;
                _cursorChanged = true;
            }
            else if (_mouseOverSplitter)
            {
                Cursor = _orientation == Orientation.Horizontal ? CursorType.SizeWE : CursorType.SizeNS;
                _cursorChanged = true;
            }
            else if (_cursorChanged)
            {
                Cursor = CursorType.Default;
                _cursorChanged = false;
            }

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && _splitterRect.Contains(location))
            {
                // Start moving splitter
                StartTracking();
                Focus();
                return true;
            }

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (_splitterClicked)
            {
                EndTracking();
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            // Clear flag
            _mouseOverSplitter = false;
            if (_cursorChanged)
            {
                Cursor = CursorType.Default;
                _cursorChanged = false;
            }

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            EndTracking();
        }

        /// <inheritdoc />
        protected override void OnSizeChanged()
        {
            base.OnSizeChanged();

            UpdateSplitRect();
            PerformLayout();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            base.PerformLayoutBeforeChildren();

            var width = Mathf.Max(Width, 0.0f);
            var height = Mathf.Max(Height, 0.0f);
            var length = _orientation == Orientation.Horizontal ? width : height;
            var capturedState = _capturedDragState;
            var capturedSplit = capturedState != null && !_splitterClicked
                ? GetCapturedSplit(capturedState, length)
                : 0;
            var split = GetEffectiveSplit();
            if (_splitterClicked || _capturedDragState != null)
            {
                if (length > 0.0f)
                    _splitterValue = Mathf.Saturate(split / length);
            }
            UpdateSplitRect(split);
            if (_orientation == Orientation.Horizontal)
            {
                var panel1Width = Mathf.Max(split - SplitterSizeHalf, 0.0f);
                var panel2X = Mathf.Min(split + SplitterSizeHalf, width);
                var panel2Width = Mathf.Max(width - panel2X, 0.0f);
                Panel1.Bounds = new Rectangle(0, 0, panel1Width, height);
                Panel2.Bounds = new Rectangle(panel2X, 0, panel2Width, height);
            }
            else
            {
                var panel1Height = Mathf.Max(split - SplitterSizeHalf, 0.0f);
                var panel2Y = Mathf.Min(split + SplitterSizeHalf, height);
                var panel2Height = Mathf.Max(height - panel2Y, 0.0f);
                Panel1.Bounds = new Rectangle(0, 0, width, panel1Height);
                Panel2.Bounds = new Rectangle(0, panel2Y, width, panel2Height);
            }

            if (capturedState != null && !_splitterClicked && split != capturedSplit)
            {
                var farPanel = capturedState.PreservePanel1 ? Panel1 : Panel2;
                var farLength = GetPanelLength(farPanel, _orientation);
                if (!Mathf.NearEqual(farLength, capturedState.PreservedLength))
                    capturedState.PreservedLength = farLength;
            }
        }
    }
}
