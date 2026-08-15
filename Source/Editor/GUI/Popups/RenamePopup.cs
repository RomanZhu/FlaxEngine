// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.GUI.ContextMenu;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Popup menu useful for renaming objects via UI. Displays text box for renaming.
    /// </summary>
    /// <seealso cref="ContextMenuBase" />
    public class RenamePopup : ContextMenuBase
    {
        private string _startValue;
        private string _text;
        private TextBox _inputField;
        private bool _renamed;

        /// <summary>
        /// Occurs when renaming is done.
        /// </summary>
        public event Action<RenamePopup> Renamed;

        /// <summary>
        /// Occurs when popup is closing (after renaming done or not).
        /// </summary>
        public event Action<RenamePopup> Closed;

        /// <summary>
        /// Input value validation delegate.
        /// </summary>
        /// <param name="popup">The popup reference.</param>
        /// <param name="value">The input text value.</param>
        /// <returns>True if text is valid, otherwise false.</returns>
        public delegate bool ValidateDelegate(RenamePopup popup, string value);

        /// <summary>
        /// Occurs when input text validation should be performed.
        /// </summary>
        public ValidateDelegate Validate;

        /// <summary>
        /// Gets or sets the initial value.
        /// </summary>
        public string InitialValue
        {
            get => _startValue;
            set => _startValue = value;
        }

        /// <summary>
        /// Gets or sets the input field text.
        /// </summary>
        public string Text
        {
            get => ResolveText(_inputField, _text);
            set
            {
                _text = value;
                if (_inputField != null)
                    _inputField.Text = value;
            }
        }

        /// <summary>
        /// Gets the text input field control.
        /// </summary>
        public TextBox InputField => _inputField;

        /// <summary>
        /// Initializes a new instance of the <see cref="RenamePopup"/> class.
        /// </summary>
        /// <param name="value">The value.</param>
        /// <param name="size">The size.</param>
        /// <param name="isMultiline">Enable/disable multiline text input support</param>
        public RenamePopup(string value, Float2 size, bool isMultiline)
        {
            if (!isMultiline)
                size.Y = TextBox.DefaultHeight;
            Size = size;

            _startValue = value;
            _text = value;

            _inputField = new TextBox(isMultiline, 0, 0, size.Y);
            _inputField.TextChanged += OnTextChanged;
            _inputField.AnchorPreset = AnchorPresets.StretchAll;
            _inputField.Offsets = Margin.Zero;
            _inputField.Text = _startValue;
            _inputField.Parent = this;
        }

        private bool IsInputValid
        {
            get
            {
                var text = Text;
                return !string.IsNullOrWhiteSpace(text) && (text == _startValue || Validate == null || Validate(this, text));
            }
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            // ContextMenuBase can leave a hidden popup in the current GUI update snapshot for
            // the remainder of the frame. OnDestroy has already released the input field then.
            if (!ShouldProcessUpdate(IsDisposing, _inputField != null))
                return;

            var mouseLocation = Root.MousePosition;
            if (!ContainsPoint(ref mouseLocation) && RootWindow.ContainsFocus && Text != _startValue)
            {
                // rename item before closing if left mouse button in clicked
                if (FlaxEngine.Input.GetMouseButtonDown(MouseButton.Left))
                    OnEnd();
            }

            base.Update(deltaTime);
        }

        private void OnTextChanged()
        {
            _text = _inputField.Text;
            var valid = IsInputValid;
            LogContentRename("rename.text-changed", $"text='{ContentMutationDiagnostics.Sanitize(_inputField.Text)}'; valid={valid}");
            if (Validate == null)
                return;

            var style = Style.Current;
            if (valid)
            {
                _inputField.BorderColor = Color.Transparent;
                _inputField.BorderSelectedColor = style.BorderSelected;
            }
            else
            {
                var color = new Color(1.0f, 0.0f, 0.02745f, 1.0f);
                _inputField.BorderColor = Color.Lerp(color, style.TextBoxBackground, 0.6f);
                _inputField.BorderSelectedColor = color;
            }
        }

        /// <summary>
        /// Shows the rename popup.
        /// </summary>
        /// <param name="control">The target control.</param>
        /// <param name="area">The target control area to cover.</param>
        /// <param name="value">The initial value.</param>
        /// <param name="isMultiline">Enable/disable multiline text input support</param>
        /// <returns>Created popup.</returns>
        public static RenamePopup Show(Control control, Rectangle area, string value, bool isMultiline)
        {
            // hardcoded flushing layout for tree controls
            if (control is Tree.TreeNode treeNode && treeNode.ParentTree != null)
                treeNode.ParentTree.FlushPendingPerformLayout();

            // Calculate the control size in the window space to handle scaled controls
            var upperLeft = control.PointToWindow(area.UpperLeft);
            var bottomRight = control.PointToWindow(area.BottomRight);
            var size = bottomRight - upperLeft;

            var rename = new RenamePopup(value, size, isMultiline);
            rename.Show(control, area.Location + new Float2(0, (size.Y - rename.Height) * 0.5f));
            return rename;
        }

        private void TryRename()
        {
            var text = Text;
            if (!_renamed && text != _startValue && IsInputValid)
            {
                LogContentRename("rename.commit", $"old='{ContentMutationDiagnostics.Sanitize(_startValue)}'; new='{ContentMutationDiagnostics.Sanitize(text)}'");
                _renamed = true;
                Renamed?.Invoke(this);
            }
            else
            {
                LogContentRename("rename.commit-skipped", $"changed={text != _startValue}; alreadyCommitted={_renamed}; valid={IsInputValid}; text='{ContentMutationDiagnostics.Sanitize(text)}'");
            }
        }

        private void OnEnd()
        {
            TryRename();

            Hide();
        }

        /// <inheritdoc />
        public override void Hide()
        {
            // An outside click can hide the popup before Update gets a chance to process it.
            if (FlaxEngine.Input.GetMouseButtonDown(MouseButton.Left))
                TryRename();

            LogContentRename("rename.hide", $"committed={_renamed}; text='{ContentMutationDiagnostics.Sanitize(Text)}'");
            base.Hide();
        }

        /// <inheritdoc />
        protected override bool UseAutomaticDirectionFix => false;

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            LogContentRename("input.rename.key-down", $"key={key}; text='{ContentMutationDiagnostics.Sanitize(Text)}'");
            // Enter
            if (key == KeyboardKeys.Return)
            {
                OnEnd();
                return true;
            }
            // Esc
            if (key == KeyboardKeys.Escape)
            {
                Hide();
                return true;
            }

            // Base
            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        protected override void OnShow()
        {
            _inputField.EndEditOnClick = false; // Ending edit is handled through popup
            _inputField.Focus();
            _inputField.SelectAll();

            base.OnShow();
        }

        /// <inheritdoc />
        protected override void OnHide()
        {
            LogContentRename("rename.closed", $"committed={_renamed}; text='{ContentMutationDiagnostics.Sanitize(Text)}'");
            Closed?.Invoke(this);
            Closed = null;

            base.OnHide();

            // Remove itself
            Dispose();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            Renamed = null;
            Closed = null;
            Validate = null;
            _text = ResolveText(_inputField, _text);
            _inputField = null;

            base.OnDestroy();
        }

        private void LogContentRename(string eventName, string details)
        {
            if (Tag is Content.ContentItem item)
                ContentMutationDiagnostics.Log(eventName, $"item='{item.Path}'; {details}");
        }

        internal static string ResolveText(TextBox inputField, string cachedText)
        {
            return inputField?.Text ?? cachedText;
        }

        internal static bool ShouldProcessUpdate(bool isDisposing, bool hasInputField)
        {
            return !isDisposing && hasInputField;
        }
    }
}
