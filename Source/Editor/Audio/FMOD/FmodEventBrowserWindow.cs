// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor.GUI.Input;
using FlaxEditor.GUI.Tree;
using FlaxEditor.Windows;
using FlaxEngine;
using FlaxEngine.GUI;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.FMOD
{
    /// <summary>FMOD-style hierarchy, metadata, parameter, spatial-preview, and output browser.</summary>
    internal sealed class FmodEventBrowserWindow : EditorWindow
    {
        private sealed class BrowserItem
        {
            public string Type;
            public string Path;
            public Guid BackendId;
            public Guid AssetId;
            public string AssetFile;
            public bool Is3D;
            public bool IsOneShot;
            public float MinDistance;
            public float MaxDistance;
            public float Length;
            public string[] ParameterNames = Array.Empty<string>();
            public string[] Dependencies = Array.Empty<string>();
        }

        private static readonly (string Type, string Label)[] Groups =
        {
            ("Event", "Events"),
            ("Snapshot", "Snapshots"),
            ("Bank", "Banks"),
            ("Bus", "Buses"),
            ("VCA", "VCAs"),
        };

        private readonly SearchBox _search;
        private readonly Button _refresh;
        private readonly Button _close;
        private readonly SplitPanel _split;
        private readonly Tree _tree;
        private readonly Panel _detailContent;
        private readonly List<BrowserItem> _items = new();
        private BrowserItem _selected;
        private AudioEventHandle _previewHandle;
        private Label _previewState;
        private Label _meterText;
        private Label _runtimeInfo;
        private ProgressBar _meter;
        private float _previewDistance = 1.0f;

        public FmodEventBrowserWindow(Editor editor)
            : base(editor, true, ScrollBars.None)
        {
            Title = "FMOD Event Browser";
            _search = new SearchBox
            {
                Parent = this,
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(6, 170, 6, 24),
            };
            _search.TextChanged += RebuildTree;
            _refresh = new Button(0, 5, 76, 26)
            {
                Parent = this,
                AnchorPreset = AnchorPresets.TopRight,
                Offsets = new Margin(-160, 84, 5, 26),
                Text = "Refresh",
                TooltipText = "Reload generated FMOD metadata from Content/Audio.",
            };
            _refresh.Clicked += () => { LoadItems(); RebuildTree(); ShowWelcome(); };
            _close = new Button(0, 5, 70, 26)
            {
                Parent = this,
                AnchorPreset = AnchorPresets.TopRight,
                Offsets = new Margin(-78, 70, 5, 26),
                Text = "Close",
                TooltipText = "Close this dockable window. Drag its tab to dock it anywhere in the editor.",
            };
            _close.Clicked += () => Close();

            _split = new SplitPanel(Orientation.Horizontal, ScrollBars.Vertical, ScrollBars.Vertical)
            {
                Parent = this,
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(4, 4, 34, 4),
                SplitterValue = 0.36f,
            };
            _tree = new Tree(false)
            {
                Parent = _split.Panel1,
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                IsScrollable = true,
            };
            _tree.SelectedChanged += OnSelectionChanged;
            _detailContent = new Panel
            {
                Parent = _split.Panel2,
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = Margin.Zero,
                Height = 720,
            };

            LoadItems();
            RebuildTree();
            ShowWelcome();
        }

        private void LoadItems()
        {
            _items.Clear();
            foreach (var file in Directory.GetFiles(Globals.ProjectContentFolder, "*.json", SearchOption.AllDirectories))
            {
                try
                {
                    var json = JObject.Parse(File.ReadAllText(file));
                    var fullType = (string)json["TypeName"] ?? string.Empty;
                    if (!fullType.StartsWith("FlaxEngine.Audio", StringComparison.Ordinal))
                        continue;
                    var type = fullType.Substring("FlaxEngine.Audio".Length);
                    if (!Groups.Any(x => x.Type == type))
                        continue;
                    var data = json["Data"] as JObject;
                    _items.Add(new BrowserItem
                    {
                        Type = type,
                        Path = (string)data?["Path"] ?? string.Empty,
                        BackendId = Guid.TryParse((string)data?["BackendId"], out var id) ? id : Guid.Empty,
                        AssetId = Guid.TryParse((string)json["ID"], out var assetId) ? assetId : Guid.Empty,
                        AssetFile = file,
                        Is3D = (bool?)data?["Is3D"] ?? false,
                        IsOneShot = (bool?)data?["IsOneShot"] ?? false,
                        MinDistance = (float?)data?["MinDistance"] ?? 0.0f,
                        MaxDistance = (float?)data?["MaxDistance"] ?? 0.0f,
                        Length = (float?)data?["Length"] ?? 0.0f,
                        ParameterNames = (data?["Parameters"] as JArray)?.Select(x => (string)x["Name"]).Where(x => !string.IsNullOrWhiteSpace(x)).ToArray() ?? Array.Empty<string>(),
                        Dependencies = (data?["BankDependencies"] as JArray)?.Select(x => (string)x).Where(x => !string.IsNullOrWhiteSpace(x)).ToArray() ?? Array.Empty<string>(),
                    });
                }
                catch (Exception ex)
                {
                    Editor.LogWarning($"FMOD Event Browser skipped invalid metadata '{file}': {ex.Message}");
                }
            }
            _items.Sort((a, b) => string.Compare(a.Path, b.Path, StringComparison.OrdinalIgnoreCase));
        }

        private void RebuildTree()
        {
            _tree.DisposeChildren();
            var query = _search.Text?.Trim() ?? string.Empty;
            var visible = _items.Where(x => query.Length == 0 || x.Path.IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0 || x.Type.IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0).ToArray();
            foreach (var group in Groups)
            {
                var groupItems = visible.Where(x => x.Type == group.Type).ToArray();
                if (groupItems.Length == 0)
                    continue;
                var root = new TreeNode(false)
                {
                    Parent = _tree,
                    Text = $"{group.Label}  ({groupItems.Length})",
                    IsSelectable = false,
                    TextColor = new Color(0.75f, 0.85f, 1.0f),
                    HeaderHeight = 22,
                };
                root.Expand(true);
                var folders = new Dictionary<string, TreeNode>(StringComparer.OrdinalIgnoreCase);
                foreach (var item in groupItems)
                {
                    var relative = StripPrefix(item.Path);
                    var parts = relative.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
                    TreeNode parent = root;
                    var folderKey = group.Type;
                    for (var i = 0; i < Math.Max(0, parts.Length - 1); i++)
                    {
                        folderKey += "/" + parts[i];
                        if (!folders.TryGetValue(folderKey, out var folder))
                        {
                            folder = new TreeNode(false) { Parent = parent, Text = parts[i], IsSelectable = false, HeaderHeight = 20 };
                            if (query.Length != 0)
                                folder.Expand(true);
                            folders.Add(folderKey, folder);
                        }
                        parent = folder;
                    }
                    var name = parts.Length == 0 ? item.Path : parts[parts.Length - 1];
                    if (item.Type == "Bank" && name.EndsWith(".bank", StringComparison.OrdinalIgnoreCase))
                        name = name.Substring(0, name.Length - 5);
                    _ = new TreeNode(false) { Parent = parent, Text = name, Tag = item, HeaderHeight = 20 };
                }
            }
        }

        private static string StripPrefix(string path)
        {
            var index = path.IndexOf(":/", StringComparison.Ordinal);
            return index >= 0 ? path.Substring(index + 2) : path.Replace('\\', '/');
        }

        private void OnSelectionChanged(List<TreeNode> before, List<TreeNode> after)
        {
            if (after != null && after.Count == 1 && after[0].Tag is BrowserItem item)
                ShowItem(item);
        }

        private Label AddLabel(string text, float y, float height = 22, Color? color = null, FontReference font = default)
        {
            var label = new Label(14, y, 620, height)
            {
                Parent = _detailContent,
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(14, 14, y, height),
                Text = text,
                TextColor = color ?? Style.Current.Foreground,
                Wrapping = TextWrapping.WrapWords,
                VerticalAlignment = TextAlignment.Near,
            };
            // FontReference is a class. Assigning its default (null) used to clear Label.Font
            // and crash Label.DrawSelf when ordinary detail rows were rendered.
            if (font != null)
                label.Font = font;
            return label;
        }

        private Button AddButton(string text, float x, float y, float width, Action action)
        {
            var button = new Button(x, y, width, 30) { Parent = _detailContent, Text = text };
            button.Clicked += action;
            return button;
        }

        private void ShowWelcome()
        {
            ReleasePreview();
            _detailContent.DisposeChildren();
            _previewState = null;
            _meter = null;
            _meterText = null;
            _runtimeInfo = null;
            _selected = null;
            AddLabel("FMOD / FLAX EVENT BROWSER", 18, 32, Color.White, new FontReference(Style.Current.FontTitle));
            AddLabel("Select an event, snapshot, bank, bus, or VCA from the hierarchy.", 62, 26, Style.Current.ForegroundGrey, new FontReference(Style.Current.FontLarge));
            AddLabel("Events expose typed metadata, authored parameter ranges, 3D preview distance, transport controls, and measured master output. Paths and GUIDs are metadata; the generated Flax asset remains the serialized reference.", 108, 70, Style.Current.ForegroundGrey);
        }

        private void ShowItem(BrowserItem item)
        {
            ReleasePreview();
            _detailContent.DisposeChildren();
            _previewState = null;
            _meter = null;
            _meterText = null;
            _runtimeInfo = null;
            _selected = item;
            var y = 16.0f;
            AddLabel(StripPrefix(item.Path), y, 34, Color.White, new FontReference(Style.Current.FontTitle));
            y += 42;
            AddLabel($"{item.Type}   {item.Path}", y, 24, new Color(0.55f, 0.78f, 1.0f));
            y += 30;
            AddLabel($"GUID   {item.BackendId}\nAsset   {Path.GetRelativePath(Globals.ProjectContentFolder, item.AssetFile).Replace('\\', '/')}" +
                     (item.Dependencies.Length == 0 ? string.Empty : $"\nBanks   {FormatDependencies(item.Dependencies)}"), y, item.Dependencies.Length == 0 ? 48 : 68, Style.Current.ForegroundGrey);
            y += item.Dependencies.Length == 0 ? 58 : 78;
            AddButton("Open Asset", 14, y, 120, OpenSelectedAsset);
            AddButton("Copy Path", 144, y, 110, () => Clipboard.Text = item.Path);
            AddButton("Copy GUID", 264, y, 110, () => Clipboard.Text = item.BackendId.ToString());
            y += 46;

            if (item.Type != "Event" && item.Type != "Snapshot")
            {
                AddLabel("This typed object is available for asset pickers, scene references, validation, and CLI lookup.", y, 48, Style.Current.ForegroundGrey);
                _detailContent.Height = y + 90;
                return;
            }

            AddLabel("PREVIEW", y, 24, Color.White, new FontReference(Style.Current.FontLarge));
            y += 32;
            AddLabel($"Panning: {(item.Is3D ? "3D" : "2D")}    One-shot: {item.IsOneShot}    Length: {(item.Length > 0 ? item.Length.ToString("0.###") + " s" : "N/A")}\nStreaming: not exported by FMOD bank metadata    Range: {(item.Is3D ? $"{item.MinDistance:0.##}-{item.MaxDistance:0.##} m" : "N/A")}", y, 44, Style.Current.ForegroundGrey);
            y += 52;
            AddButton("Play", 14, y, 82, PlayPreview);
            AddButton("Pause", 104, y, 82, PausePreview);
            AddButton("Stop", 194, y, 82, StopPreview);
            AddButton("Release", 284, y, 82, ReleasePreview);
            _previewState = AddLabel("Stopped", y + 4, 24, new Color(0.75f, 0.85f, 1.0f));
            _previewState.Offsets = new Margin(382, 14, y + 4, 24);
            y += 46;
            _meter = new ProgressBar(14, y, 350, 18) { Parent = _detailContent, Value = 0 };
            _meterText = AddLabel("Master output: -120.0 dBFS", y - 2, 22, Style.Current.ForegroundGrey);
            _meterText.Offsets = new Margin(380, 14, y - 2, 22);
            y += 36;
            _runtimeInfo = AddLabel("Runtime: start preview to inspect sample, voice, virtual, audibility, and signal-path state.", y, 44, Style.Current.ForegroundGrey);
            y += 52;

            if (item.Is3D)
            {
                var spatial = new SpatialPreview { Parent = _detailContent, Bounds = new Rectangle(14, y, 496, 150), MinDistance = item.MinDistance, MaxDistance = item.MaxDistance, Distance = _previewDistance };
                y += 160;
                AddLabel("Listener distance", y, 22, Style.Current.ForegroundGrey);
                var max = Mathf.Max(item.MaxDistance * 1.5f, 2.0f);
                _previewDistance = Mathf.Clamp(_previewDistance, 0.0f, max);
                var distance = new SliderControl(_previewDistance, 150, y, 360, 0.0f, max) { Parent = _detailContent };
                distance.ValueChanged += () => { _previewDistance = distance.Value; spatial.Distance = _previewDistance; spatial.PerformLayout(); ApplyPreviewPosition(); };
                y += 38;
            }

            AddLabel("PARAMETERS", y, 24, Color.White, new FontReference(Style.Current.FontLarge));
            y += 32;
            if (AudioEventSystem.GetEventParameters(item.BackendId, item.Path, out var parameters) && parameters.Length != 0)
            {
                foreach (var parameter in parameters)
                {
                    var captured = parameter;
                    AddLabel(captured.Id.Name, y, 22, Style.Current.ForegroundGrey);
                    var slider = new SliderControl(captured.DefaultValue, 150, y, 360, captured.Minimum, captured.Maximum) { Parent = _detailContent };
                    slider.ValueChanged += () => { if (IsPreviewValid()) AudioEventSystem.SetParameter(_previewHandle, captured.Id, slider.Value, false); };
                    y += 34;
                }
            }
            else if (item.ParameterNames.Length != 0)
            {
                AddLabel($"Runtime ranges become available after the FMOD backend initializes. Authored parameters: {string.Join(", ", item.ParameterNames)}", y, 46, Color.Orange);
                y += 54;
            }
            else
            {
                AddLabel("No authored parameters.", y, 24, Style.Current.ForegroundGrey);
                y += 32;
            }
            _detailContent.Height = y + 40;
        }

        private string FormatDependencies(string[] dependencies)
        {
            var names = new List<string>();
            foreach (var dependency in dependencies)
            {
                if (Guid.TryParse(dependency, out var id))
                {
                    var bank = _items.FirstOrDefault(x => x.Type == "Bank" && x.BackendId == id);
                    names.Add(bank != null ? StripPrefix(bank.Path) : dependency);
                }
                else
                {
                    names.Add(dependency);
                }
            }
            return string.Join(", ", names);
        }

        private sealed class SpatialPreview : Control
        {
            public float MinDistance;
            public float MaxDistance;
            public float Distance;

            public override void Draw()
            {
                var bounds = new Rectangle(0, 0, Width, Height);
                Render2D.FillRectangle(bounds, Style.Current.BackgroundSelected.AlphaMultiplied(0.35f));
                Render2D.DrawRectangle(bounds, Style.Current.BorderNormal);
                var center = new Float2(Width * 0.5f, Height * 0.5f);
                var radius = Math.Min(Width, Height) * 0.38f;
                DrawCircle(center, radius, new Color(0.35f, 0.5f, 0.7f));
                var scale = MaxDistance > 0 ? radius / MaxDistance : radius;
                DrawCircle(center, Math.Max(3.0f, MinDistance * scale), Color.LightGreen);
                var source = center + new Float2(Math.Min(radius, Distance * scale), 0);
                Render2D.FillRectangle(new Rectangle(center.X - 4, center.Y - 4, 8, 8), Color.White);
                Render2D.FillRectangle(new Rectangle(source.X - 5, source.Y - 5, 10, 10), Color.Orange);
                Render2D.DrawLine(center, source, Color.Orange, 2.0f);
                base.Draw();
            }

            private static void DrawCircle(Float2 center, float radius, Color color)
            {
                const int segments = 48;
                var previous = center + new Float2(radius, 0);
                for (var i = 1; i <= segments; i++)
                {
                    var angle = Mathf.TwoPi * i / segments;
                    var next = center + new Float2(Mathf.Cos(angle) * radius, Mathf.Sin(angle) * radius);
                    Render2D.DrawLine(previous, next, color);
                    previous = next;
                }
            }
        }

        private void OpenSelectedAsset()
        {
            if (_selected != null && Editor.ContentDatabase.Find(_selected.AssetFile) is FlaxEditor.Content.AssetItem item)
                Editor.ContentEditing.Open(item);
        }

        private void PlayPreview()
        {
            if (_selected == null)
                return;
            ReleasePreview();
            var options = new AudioEventCreateOptions();
            if (_selected.Is3D)
                options.Attributes = PreviewAttributes();
            LoadPreviewBanks();
            AudioEventSystem.SetPreviewListener(PreviewListenerAttributes());
            if (!AudioEventSystem.GetEventParameters(_selected.BackendId, _selected.Path, out _))
            {
                if (_previewState != null)
                    _previewState.Text = "Unavailable in the linked banks — rebuild/import metadata, then Refresh";
                return;
            }
            _previewHandle = AudioEventSystem.CreatePreviewInstance(_selected.BackendId, _selected.Path, options);
            if (!IsPreviewValid())
            {
                SetPreviewFailure("Create failed");
                return;
            }
            if (!AudioEventSystem.PlayPreview(_previewHandle))
                SetPreviewFailure("Play failed");
        }

        private void LoadPreviewBanks()
        {
            // Edit mode unloads runtime banks by design. The browser owns an
            // explicit preview session, so hydrate the generated typed bank set
            // in deterministic dependency-friendly order before creating a voice.
            var banks = _items.Where(x => x.Type == "Bank")
                              .OrderBy(x => x.Path.EndsWith(".strings.bank", StringComparison.OrdinalIgnoreCase) ? 0 :
                                            x.Path.EndsWith("Master.bank", StringComparison.OrdinalIgnoreCase) ? 1 : 2);
            foreach (var item in banks)
            {
                var databaseItem = Editor.ContentDatabase.Find(item.AssetFile) as FlaxEditor.Content.AssetItem;
                if (databaseItem == null)
                    continue;
                var asset = FlaxEngine.Content.LoadAsync<JsonAsset>(databaseItem.ID);
                var bank = asset?.GetInstance<AudioBank>();
                if (bank != null && !AudioEventSystem.IsBankLoaded(bank.BackendId))
                    AudioEventSystem.LoadBank(bank.BackendId, bank.Path, bank.NonBlocking);
            }
        }

        private Audio3DAttributes PreviewAttributes()
        {
            var listenerAttributes = PreviewListenerAttributes();
            return new Audio3DAttributes
            {
                Position = listenerAttributes.Position + listenerAttributes.Forward * (_previewDistance * 100.0f),
                Velocity = Vector3.Zero,
                Forward = listenerAttributes.Forward,
                Up = listenerAttributes.Up,
            };
        }

        private static Audio3DAttributes PreviewListenerAttributes()
        {
            var listener = Level.GetActors<AudioListener>().FirstOrDefault(x => x.IsActiveInHierarchy);
            var actor = listener?.AttenuationActor ?? listener;
            return new Audio3DAttributes
            {
                Position = actor?.Position ?? Vector3.Zero,
                Velocity = Vector3.Zero,
                Forward = Vector3.Forward,
                Up = Vector3.Up,
            };
        }

        private void SetPreviewFailure(string operation)
        {
            AudioEventSystem.CaptureDiagnostics(out var diagnostics);
            if (_previewState != null)
                _previewState.Text = diagnostics.LastErrorCode == 0
                    ? $"{operation}: backend did not return a usable instance"
                    : $"{operation}: FMOD {diagnostics.LastErrorCode} {diagnostics.LastError}";
        }

        private void PausePreview()
        {
            if (IsPreviewValid())
                AudioEventSystem.Pause(_previewHandle);
        }

        private void StopPreview()
        {
            if (IsPreviewValid())
                AudioEventSystem.Stop(_previewHandle, AudioStopMode.AllowFadeOut);
        }

        private void ApplyPreviewPosition()
        {
            if (IsPreviewValid() && _selected != null && _selected.Is3D)
                AudioEventSystem.Set3DAttributes(_previewHandle, PreviewAttributes());
        }

        private void ReleasePreview()
        {
            if (IsPreviewValid())
                AudioEventSystem.StopAndRelease(_previewHandle, AudioStopMode.AllowFadeOut);
            _previewHandle = default;
            if (_previewState != null)
                _previewState.Text = "Stopped";
        }

        private bool IsPreviewValid() => _previewHandle.Generation != 0;

        public override void Update(float deltaTime)
        {
            base.Update(deltaTime);
            if (_meter != null && _meterText != null)
            {
                AudioEventSystem.CaptureDiagnostics(out var diagnostics);
                _meter.Value = Mathf.Saturate((diagnostics.CombinedOutputDbfs + 60.0f) / 60.0f) * 100.0f;
                _meterText.Text = $"Master output: {diagnostics.CombinedOutputDbfs:0.0} dBFS";
                if (_runtimeInfo != null)
                {
                    var runtime = default(AudioEventRuntimeInfo);
                    var found = false;
                    if (diagnostics.Events != null)
                    {
                        foreach (var candidate in diagnostics.Events)
                        {
                            if (candidate.Handle.Index != _previewHandle.Index || candidate.Handle.Generation != _previewHandle.Generation)
                                continue;
                            runtime = candidate;
                            found = true;
                            break;
                        }
                    }
                    _runtimeInfo.Text = !found
                        ? $"Runtime: backend={diagnostics.BackendName}; loaded banks={diagnostics.LoadedBanks}; active instances={diagnostics.ActiveInstances}"
                        : $"Runtime: sample={runtime.SampleLoadingState}; real/virtual voices={runtime.RealVoices}/{runtime.VirtualVoices}; virtual={runtime.IsVirtual}; audible={runtime.Audible}; reaching output={runtime.ReachingOutput}\nGain={runtime.FinalVolume:0.###}; audibility={runtime.Audibility:0.###}; listener mask=0x{runtime.ListenerMask:X}; silence={runtime.SilenceCause}";
                }
            }
            if (_previewState != null)
            {
                if (IsPreviewValid() && AudioEventSystem.QueryInstance(_previewHandle, out var state))
                    _previewState.Text = $"{state.PlaybackState}  {state.TimelinePosition} ms";
                else
                    _previewState.Text = "Stopped";
            }
        }

        public override void OnDestroy()
        {
            ReleasePreview();
            base.OnDestroy();
        }
    }
}
