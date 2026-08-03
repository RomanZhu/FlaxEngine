// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.Assertions;
using FlaxEngine.GUI;

namespace FlaxEditor.CustomEditors.GUI
{
    /// <summary>
    /// Custom property name label that contains a checkbox used to enable/disable a property.
    /// </summary>
    /// <seealso cref="FlaxEditor.CustomEditors.GUI.PropertyNameLabel" />
    [HideInEditor]
    public class CheckablePropertyNameLabel : PropertyNameLabel
    {
        /// <summary>
        /// The check box.
        /// </summary>
        public readonly CheckBox CheckBox;

        /// <summary>
        /// Gets or sets a value indicating whether child controls remain interactive but draw as disabled when unchecked.
        /// </summary>
        public bool UseAnticipatingChildInput { get; set; }

        /// <summary>
        /// Event fired when 'checked' state gets changed.
        /// </summary>
        public event Action<CheckablePropertyNameLabel> CheckChanged;

        /// <inheritdoc />
        public CheckablePropertyNameLabel(string name)
        : base(name)
        {
            CheckBox = new CheckBox(2, 2)
            {
                Checked = true,
                Size = new Float2(14),
                Parent = this
            };
            CheckBox.StateChanged += OnCheckChanged;
            Margin = new Margin(CheckBox.Width + 6, 4, 0, 0);
        }

        private void OnCheckChanged(CheckBox box)
        {
            CheckChanged?.Invoke(this);
            UpdateStyle();
        }

        /// <summary>
        /// Updates the label style.
        /// </summary>
        public virtual void UpdateStyle()
        {
            var style = Style.Current;
            bool check = CheckBox.Checked;
            bool anticipateChildInput = UseAnticipatingChildInput && !check;
            bool enableChildControls = check || UseAnticipatingChildInput;

            // Update label text color
            TextColor = check ? style.Foreground : style.ForegroundGrey;

            // Update child controls enabled state
            if (FirstChildControlIndex >= 0 && Parent is PropertiesList propertiesList)
            {
                if (FirstChildControlContainer != null)
                    propertiesList = FirstChildControlContainer;
                var controls = propertiesList.Children;
                var labels = propertiesList.Element.Labels;
                var thisIndex = labels.IndexOf(this);
                Assert.AreNotEqual(-1, thisIndex, "Invalid label linkage.");
                int childControlsCount = 0;
                if (thisIndex + 1 < labels.Count)
                    childControlsCount = labels[thisIndex + 1].FirstChildControlIndex - FirstChildControlIndex - 1;
                else
                    childControlsCount = controls.Count;
                int lastControl = Mathf.Min(FirstChildControlIndex + childControlsCount, controls.Count);
                for (int i = FirstChildControlIndex; i < lastControl; i++)
                {
                    controls[i].Enabled = enableChildControls;
                    controls[i].DrawAsDisabled = anticipateChildInput;
                }
            }
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            base.PerformLayoutBeforeChildren();

            // Place the checkbox directly to the left of the right-aligned label text.
            var font = Font?.GetFont();
            var text = Text?.ToString() ?? string.Empty;
            var textWidth = font != null ? font.MeasureText(text).X : 0.0f;
            var textX = Width - Margin.Right - textWidth;
            CheckBox.X = Mathf.Max(2.0f, textX - CheckBox.Width - 4.0f);
            CheckBox.Y = (Height - CheckBox.Height) / 2;
        }
    }
}
