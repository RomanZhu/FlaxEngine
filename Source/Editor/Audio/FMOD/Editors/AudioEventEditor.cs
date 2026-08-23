// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Globalization;
using System.IO;
using System.Linq;
using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Editors;
using FlaxEngine;
using FlaxEngine.GUI;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.FMOD
{
    /// <summary>Editor controls for previewing an FMOD event asset outside Play mode.</summary>
    [CustomEditor(typeof(AudioEvent)), DefaultEditor]
    public sealed class AudioEventEditor : GenericEditor
    {
        private AudioEvent _event;
        private AudioEventHandle _handle;
        private Label _state;

        public override void Initialize(LayoutElementsContainer layout)
        {
            base.Initialize(layout);
            _event = Values.Count > 0 ? Values[0] as AudioEvent : null;
            var parametersGroup = layout.Group("FMOD Parameters");
            AddParameterDescriptions(parametersGroup);
            var group = layout.Group("FMOD Preview");
            _state = group.Label("Stopped").Label;
            var buttons = group.UniformGrid();
            buttons.CustomControl.Height = Button.DefaultHeight;
            buttons.CustomControl.SlotsHorizontally = 4;
            buttons.CustomControl.SlotsVertically = 1;
            buttons.Button("Play").Button.Clicked += Play;
            buttons.Button("Pause").Button.Clicked += Pause;
            buttons.Button("Stop").Button.Clicked += Stop;
            buttons.Button("Release").Button.Clicked += Release;
        }

        private void Play()
        {
            Release();
            if (_event == null)
                return;
            _handle = AudioEventSystem.CreatePreviewInstance(_event.BackendId, _event.Path, new AudioEventCreateOptions());
            if (IsHandleValid(_handle))
                AudioEventSystem.PlayPreview(_handle);
            Refresh();
        }

        private void Pause()
        {
            if (IsHandleValid(_handle))
                AudioEventSystem.Pause(_handle);
            Refresh();
        }

        private void Stop()
        {
            if (IsHandleValid(_handle))
                AudioEventSystem.Stop(_handle, AudioStopMode.AllowFadeOut);
            Refresh();
        }

        private void Release()
        {
            ReleaseHandle();
            Refresh();
        }

        private void ReleaseHandle()
        {
            if (IsHandleValid(_handle))
                AudioEventSystem.ReleaseInstance(_handle);
            _handle = new AudioEventHandle();
        }

        private void AddParameterDescriptions(LayoutElementsContainer group)
        {
            var descriptions = ReadParameterDescriptions();
            if (descriptions == null || descriptions.Count == 0)
            {
                group.Label("No authored parameters.");
                return;
            }

            try
            {
                foreach (var description in descriptions.OfType<JObject>())
                {
                    var id = description[nameof(AudioParameterDescription.Id)] as JObject;
                    var name = (string)id?[nameof(AudioParameterId.Name)];
                    if (string.IsNullOrWhiteSpace(name))
                        name = "Unnamed";
                    var minimum = (float?)description[nameof(AudioParameterDescription.Minimum)] ?? 0.0f;
                    var maximum = (float?)description[nameof(AudioParameterDescription.Maximum)] ?? 1.0f;
                    var defaultValue = (float?)description[nameof(AudioParameterDescription.DefaultValue)] ?? 0.0f;
                    var type = (int?)description[nameof(AudioParameterDescription.Type)] ?? 0;
                    var flags = (uint?)description[nameof(AudioParameterDescription.Flags)] ?? 0;
                    group.Label($"{name}: {Format(minimum)} to {Format(maximum)}, default {Format(defaultValue)} (type {type}, flags {flags})");

                    var labels = ((string)description[nameof(AudioParameterDescription.Labels)] ?? string.Empty)
                                 .Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
                    if (labels.Length > 0)
                        group.Label("Values: " + string.Join(", ", labels));
                }
            }
            catch (Exception ex)
            {
                group.Label("Parameter metadata is invalid. Rebuild and synchronize the FMOD project.");
                Editor.LogWarning($"Failed to display FMOD parameter metadata for '{_event.Path}'. Exception: {ex.Message}");
            }
        }

        private JArray ReadParameterDescriptions()
        {
            if (_event == null)
                return null;
            var directory = Path.Combine(Globals.ProjectContentFolder, "Audio", "Events");
            if (!Directory.Exists(directory))
                return null;

            foreach (var path in Directory.GetFiles(directory, "*.json", SearchOption.AllDirectories))
            {
                try
                {
                    var data = JObject.Parse(File.ReadAllText(path))["Data"] as JObject;
                    if (data == null || !Guid.TryParse((string)data[nameof(AudioEvent.BackendId)], out var backendId) || backendId != _event.BackendId)
                        continue;
                    return data["ParameterDescriptions"] as JArray;
                }
                catch
                {
                    // Ignore unrelated or partially written JSON files in the generated folder.
                }
            }
            return null;
        }

        private static string Format(float value) => value.ToString("0.###", CultureInfo.InvariantCulture);

        public override void Refresh()
        {
            base.Refresh();
            if (_state == null)
                return;
            AudioEventInstanceState state = default;
            if (IsHandleValid(_handle) && AudioEventSystem.QueryInstance(_handle, out state))
                _state.Text = $"{state.PlaybackState} @ {state.TimelinePosition} ms";
            else
                _state.Text = "Stopped";
        }

        protected override void Deinitialize()
        {
            // The backing JsonAsset can be replaced during FMOD synchronization.
            // Do not refresh a presenter that is currently dismantling its editor tree.
            ReleaseHandle();
            _event = null;
            _state = null;
            base.Deinitialize();
        }

        private static bool IsHandleValid(AudioEventHandle handle) => handle.Generation != 0;
    }
}
