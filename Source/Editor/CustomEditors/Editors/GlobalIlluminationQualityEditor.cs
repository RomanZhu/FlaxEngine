// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Content.Settings;
using FlaxEngine;

namespace FlaxEditor.CustomEditors.Editors
{
    /// <summary>
    /// Custom editor for graphics quality options that can create expensive global illumination workloads.
    /// </summary>
    public sealed class GlobalIlluminationQualityEditor : EnumEditor
    {
        /// <inheritdoc />
        protected override void OnValueChanged()
        {
            var value = (Quality)element.ComboBox.EnumTypeValue;
            var previousValue = (Quality)Values[0];
            if (value == Quality.Ultra && previousValue != value && ParentEditor.Values[0] is GraphicsSettings settings)
            {
                bool isGIQuality = Values.Info.Name == nameof(GraphicsSettings.GIQuality);
                bool isGlobalSDFQuality = Values.Info.Name == nameof(GraphicsSettings.GlobalSDFQuality);
                if (isGIQuality || (isGlobalSDFQuality && settings.GIQuality == Quality.Ultra))
                {
                    var giQuality = isGIQuality ? value : settings.GIQuality;
                    var globalSDFQuality = isGlobalSDFQuality ? value : settings.GlobalSDFQuality;
                    var result = MessageBox.Show(
                        $"This combination can create a very large software ray-tracing workload in a single frame and may trigger the operating system GPU timeout, which terminates the editor.\n\n" +
                        $"GI quality: {giQuality}\nGlobal SDF quality: {globalSDFQuality}\nProbe spacing: {settings.GIProbesSpacing}\n\n" +
                        "High GI quality or probe spacing in the 200-500 range is recommended. Apply Ultra anyway?",
                        "High global illumination workload", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning);
                    if (result != DialogResult.OK)
                    {
                        // Restore the selection without changing the settings object or triggering auto-save.
                        element.ComboBox.EnumTypeValue = previousValue;
                        return;
                    }
                }
            }

            base.OnValueChanged();
        }
    }
}
