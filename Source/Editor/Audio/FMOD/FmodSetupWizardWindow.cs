// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using FlaxEditor.Content.Settings;
using FlaxEditor.Windows;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.FMOD
{
    /// <summary>Guided, state-derived FMOD project linking and setup workflow.</summary>
    internal sealed class FmodSetupWizardWindow : EditorWindow
    {
        private static readonly string[] StepNames =
        {
            "Welcome", "Updating", "Linking", "Bank Output", "Metadata", "Listener", "Audio Setup", "Sources", "Source Control", "Validation", "Finish"
        };

        private readonly Button[] _steps = new Button[StepNames.Length];
        private readonly Label _title;
        private readonly Label _description;
        private readonly Label _details;
        private readonly Label _result;
        private readonly Button _primary;
        private readonly Button _secondary;
        private readonly Button _back;
        private readonly Button _next;
        private int _step;
        private ValidationPhase _validationPhase;
        private float _validationTimer;
        private float _validation2DPeak;
        private float _validation3DNearPeak;
        private float _validation3DFarPeak;
        private AudioEventHandle _validationHandle;
        private AudioEmitter[] _suspendedEmitters;

        private enum ValidationPhase
        {
            Idle,
            WaitingForPlay,
            Probe2D,
            Probe3DNear,
            Probe3DFar,
        }

        public FmodSetupWizardWindow(Editor editor)
            : base(editor, true, ScrollBars.Vertical)
        {
            Title = "FMOD Setup Wizard";

            new Label(24, 22, 190, 54)
            {
                Parent = this,
                Text = "FMOD / FLAX",
                Font = new FontReference(Style.Current.FontTitle),
                TextColor = Color.White,
                HorizontalAlignment = TextAlignment.Center,
                VerticalAlignment = TextAlignment.Center,
            };

            for (var i = 0; i < _steps.Length; i++)
            {
                var captured = i;
                _steps[i] = new Button(20, 90 + i * 36, 200, 30) { Parent = this, Text = StepNames[i] };
                _steps[i].Clicked += () => ShowStep(captured);
            }

            _title = new Label(260, 30, 610, 42)
            {
                Parent = this,
                Font = new FontReference(Style.Current.FontTitle),
                TextColor = Color.White,
                HorizontalAlignment = TextAlignment.Near,
            };
            _description = new Label(260, 88, 610, 82)
            {
                Parent = this,
                Font = new FontReference(Style.Current.FontLarge),
                Wrapping = TextWrapping.WrapWords,
                AutoHeight = true,
            };
            _details = new Label(260, 200, 610, 235)
            {
                Parent = this,
                Wrapping = TextWrapping.WrapWords,
                AutoHeight = false,
                TextColor = Style.Current.ForegroundGrey,
                HorizontalAlignment = TextAlignment.Near,
                VerticalAlignment = TextAlignment.Near,
            };
            _result = new Label(260, 445, 610, 150)
            {
                Parent = this,
                Wrapping = TextWrapping.WrapWords,
                AutoHeight = false,
                TextColor = Color.Wheat,
                HorizontalAlignment = TextAlignment.Near,
                VerticalAlignment = TextAlignment.Near,
            };

            _primary = new Button(260, 610, 250, 38) { Parent = this };
            _secondary = new Button(525, 610, 250, 38) { Parent = this };
            _back = new Button(260, 670, 120, 32) { Parent = this, Text = "Back" };
            _next = new Button(750, 670, 120, 32) { Parent = this, Text = "Next" };
            _primary.Clicked += RunPrimary;
            _secondary.Clicked += RunSecondary;
            _back.Clicked += () => ShowStep(Math.Max(0, _step - 1));
            _next.Clicked += () => ShowStep(Math.Min(StepNames.Length - 1, _step + 1));
            ShowStep(0);
        }

        private void ShowStep(int step)
        {
            _step = Math.Max(0, Math.Min(StepNames.Length - 1, step));
            _result.Text = string.Empty;
            _back.Enabled = _step > 0;
            _next.Enabled = _step < StepNames.Length - 1;
            RefreshNavigation();

            switch (_step)
            {
            case 0:
                SetPage("Welcome to FMOD for Flax",
                    "Connect an FMOD Studio project, import its built banks, generate typed Flax assets, and verify runtime audio.",
                    "The .fspro and build-output paths are per-user and are never committed. Generated metadata, typed references, and banks live under Content/Audio and can be shared with the project.",
                    "Start setup", "Refresh status");
                break;
            case 1:
                SetPage("Update existing FMOD references",
                    "Scan loaded scenes for obsolete string event paths and migrate unambiguous matches to stable typed AudioEvent assets.",
                    $"PASS  Engine integration layout: maintained by the Flax engine (no project plugin files to reorganize).\n{ReferenceInventory()}",
                    "Scan references", "Repair safe references");
                break;
            case 2:
                SetPage("Link the FMOD Studio project",
                    "Select the .fspro that authors this game's banks, or use a compiled single/multiple-platform bank folder. Flax uses the exact linked project for Open Project and Build Banks.",
                    "Current local project:\n" + ValueOrMissing(FmodEditorSettings.StudioProjectPath) +
                    "\n\n" + FmodStudioLocator.GetInstallationSummary(),
                    "Select .fspro...", "Open linked project");
                break;
            case 3:
                SetPage("Choose built-bank output",
                    "Select the folder containing Master.bank and Master.strings.bank. It is normally detected from the linked Studio project.",
                    "Current local bank source:\n" + ValueOrMissing(FmodEditorSettings.BankOutputPath) +
                    "\n\nProject destination:\n" + Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks") +
                    "\n\nBuilder:\n" + ValueOrMissing(FmodStudioLocator.FindCommandLineExecutable()),
                    "Select bank folder...", "Build banks in FMOD Studio");
                break;
            case 4:
                SetPage("Import banks and generate metadata",
                    "Read the compiled FMOD banks and create stable, typed AudioBank, AudioEvent, AudioSnapshot, AudioBus, and AudioVCA assets.",
                    "Files already inside Content/Audio/Banks are never copied onto themselves. External updates are unloaded, copied, validated, synchronized, and reloaded.",
                    "Import + synchronize", "Build banks first");
                break;
            case 5:
                var listeners = Level.GetActors<AudioListener>();
                SetPage("Verify FMOD listeners",
                    "Every 3D event needs an active AudioListener. The listener index and mask are validated by the runtime backend.",
                    listeners.Length == 0
                        ? "FAIL  No AudioListener is present in the loaded scene. Add one to the player camera."
                        : $"PASS  {listeners.Length} listener(s) found:\n" + string.Join("\n", Array.ConvertAll(listeners, x => $"  {x.Name}  index={x.ListenerIndex}  actor={x.ID}")),
                    "Refresh listener scan", "Select first listener");
                break;
            case 6:
                var audioSettings = GameSettings.Load<AudioSettings>();
                SetPage("Apply runtime audio settings",
                    "Configure FMOD as the sole event-output owner, apply startup banks, and choose whether FMOD Studio can attach for live profiling. Flax native Audio is not a competing Unity-style output device.",
                    FmodSetupWizard.Validate(false) +
                    $"\nProfiler connection: {(audioSettings.EnableLiveUpdate ? "ENABLED" : "DISABLED")} at localhost:{audioSettings.LiveUpdatePort}\nChanges to Live Update take effect the next time Play starts.",
                    "Apply settings", audioSettings.EnableLiveUpdate ? "Disable Live Update" : "Enable Live Update");
                break;
            case 7:
                var sources = Level.GetActors<AudioSource>();
                var emitters = Level.GetActors<AudioEmitter>();
                SetPage("Review audio sources and emitters",
                    "Inventory authored sources and migrate obsolete FMOD string fallbacks to typed AudioEvent references.",
                    $"AudioEmitter actors: {emitters.Length}\nFlax AudioSource actors: {sources.Length}\n\n{ReferenceInventory()}",
                    "Scan sources", "Repair safe references");
                break;
            case 8:
                SetPage("Source-control hygiene",
                    "Keep generated typed assets and compiled banks shared; keep each developer's local .fspro and bank-output paths out of project settings.",
                    "PASS  Local Studio link and bank source are stored under the editor user cache.\nPASS  Content/Audio typed assets and banks are project data.\nPASS  No Unity plugin DLL, Cache, StreamingAssets, .meta, or .gitattributes rules are required by the Flax integration.",
                    "Refresh source-control check", "Copy guidance");
                break;
            case 9:
                SetPage("Validate the complete setup",
                    "Verify libraries, linking, bank metadata, typed references, listeners, emitters, runtime configuration, and source-control hygiene.",
                    FmodSetupWizard.Validate(false), "Run validation", "Refresh status");
                break;
            default:
                SetPage("FMOD setup complete",
                    GetStepState(9) ? "Flax has a validated, typed view of the linked FMOD project." : "Unfinished checks remain. Return to Validation and resolve every TODO before shipping.",
                    FmodSetupWizard.Validate(false), "Open FMOD Studio", "Validate again");
                break;
            }
        }

        public void RefreshState()
        {
            ShowStep(_step);
        }

        private void SetPage(string title, string description, string details, string primary, string secondary)
        {
            _title.Text = title;
            _description.Text = description;
            _details.Text = details;
            _primary.Text = primary;
            _secondary.Text = secondary;
            if (_step == 9 || _step == 10)
            {
                var issue = FmodSetupWizard.GetIncompleteSummary();
                _result.Text = issue;
                _result.TextColor = issue.StartsWith("PASS", StringComparison.Ordinal) ? Color.LightGreen : Color.Orange;
            }
        }

        private bool GetStepState(int step)
        {
            switch (step)
            {
            case 0: return true;
            case 1: return CountLegacyEmitters() == 0;
            case 2: return File.Exists(FmodEditorSettings.StudioProjectPath);
            case 3: return Directory.Exists(FmodEditorSettings.BankOutputPath) && File.Exists(Path.Combine(FmodEditorSettings.BankOutputPath, "Master.bank"));
            case 4: return File.Exists(Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks", "fmod-metadata.json"));
            case 5: return Level.GetActors<AudioListener>().Length > 0;
            case 6: return FmodSetupWizard.Validate(false).IndexOf("TODO 4.", StringComparison.Ordinal) < 0;
            case 7: return CountLegacyEmitters() == 0;
            case 8: return true;
            case 9: return FmodSetupWizard.Validate(false).IndexOf("TODO", StringComparison.Ordinal) < 0;
            case 10: return GetStepState(9);
            default: return false;
            }
        }

        private static string ValueOrMissing(string value) => string.IsNullOrWhiteSpace(value) ? "NOT SELECTED" : value;

        private static int CountLegacyEmitters() => Level.GetActors<AudioEmitter>().Count(x => !x.Event && !string.IsNullOrWhiteSpace(x.EventPath));

        private static string ReferenceInventory()
        {
            var emitters = Level.GetActors<AudioEmitter>();
            var legacy = CountLegacyEmitters();
            var missing = emitters.Count(x => !x.Event && string.IsNullOrWhiteSpace(x.EventPath));
            return $"{(legacy == 0 && missing == 0 ? "PASS" : "TODO")}  {emitters.Length} emitter(s); {legacy} obsolete string fallback(s); {missing} missing event reference(s).";
        }

        private void RunPrimary()
        {
            switch (_step)
            {
            case 0: ShowStep(1); return;
            case 1: RunAndRefresh(ReferenceInventory()); return;
            case 2: SelectProject(); return;
            case 3: SelectBanks(); return;
            case 4: ImportAndSynchronize(); return;
            case 5: RunAndRefresh(ListenerInventory()); return;
            case 6: RunAndRefresh(FmodSetupWizard.ApplyDiscoveredSettings()); return;
            case 7: RunAndRefresh(ReferenceInventory()); return;
            case 8: RunAndRefresh("PASS - local authoring paths are per-user; generated Content/Audio data is shared."); return;
            case 9:
                StartRuntimeValidation();
                return;
            case 10: RunAndRefresh(FmodStudioLocator.OpenProject() ? "Opened linked FMOD Studio project." : "No valid Studio project or executable is linked."); return;
            }
        }

        private void RunSecondary()
        {
            switch (_step)
            {
            case 0:
            case 9: ShowStep(_step); return;
            case 1: RepairReferences(); return;
            case 2: RunAndRefresh(FmodStudioLocator.OpenProject() ? "Opened linked FMOD Studio project." : "No valid Studio project or executable is linked."); return;
            case 3:
            case 4: BuildBanks(); return;
            case 5: SelectFirstListener(); return;
            case 6: RunAndRefresh(FmodSetupWizard.ToggleLiveUpdate()); return;
            case 7: RepairReferences(); return;
            case 8:
                Clipboard.Text = "Keep Content/Audio banks and generated typed assets. Local .fspro and bank-output links are stored in the editor user cache and must not be committed.";
                RunAndRefresh("Source-control guidance copied.");
                return;
            case 10: StartRuntimeValidation(); return;
            }
        }

        private static string ListenerInventory()
        {
            var listeners = Level.GetActors<AudioListener>();
            return listeners.Length == 0 ? "FAIL - no AudioListener found." : $"PASS - {listeners.Length} AudioListener actor(s) found.";
        }

        private void SelectFirstListener()
        {
            var listener = Level.GetActors<AudioListener>().FirstOrDefault();
            if (listener == null)
            {
                RunAndRefresh("FAIL - no listener exists. Add an AudioListener actor as a child of the player camera.");
                return;
            }
            Editor.SceneEditing.Select(listener);
            RunAndRefresh($"Selected listener '{listener.Name}' ({listener.ID}).");
        }

        private void RepairReferences()
        {
            if (Editor.IsPlayMode)
            {
                RunAndRefresh("Stop Play mode before persisting reference repairs.");
                return;
            }
            try
            {
                CliAudioCommands.ReferencesRepair(false);
                RunAndRefresh(ReferenceInventory());
            }
            catch (Exception ex)
            {
                RunAndRefresh("FAIL - " + ex.Message);
                _result.TextColor = Color.OrangeRed;
            }
        }

        private void SelectProject()
        {
            if (FileSystem.ShowOpenFileDialog(Editor.Windows.MainWindow, null, "FMOD Studio projects (*.fspro)\0*.fspro\0All files (*.*)\0*.*\0", false, "Select FMOD Studio project", out var files) || files == null || files.Length == 0)
                return;
            try
            {
                var detected = FmodProjectLinker.LinkProject(files[0]);
                ShowStep(string.IsNullOrEmpty(detected) ? 3 : 4);
                _result.Text = string.IsNullOrEmpty(detected) ? "Project linked. Select its built-bank folder next." : "Project linked; bank output detected:\n" + detected;
            }
            catch (Exception ex)
            {
                _result.Text = ex.Message;
            }
        }

        private void SelectBanks()
        {
            if (FileSystem.ShowBrowseFolderDialog(Editor.Windows.MainWindow, FmodEditorSettings.BankOutputPath, "Select FMOD built-bank folder", out var folder) || string.IsNullOrWhiteSpace(folder))
                return;
            FmodEditorSettings.BankOutputPath = folder;
            ShowStep(4);
            _result.Text = "Built-bank folder linked.";
        }

        private void BuildBanks()
        {
            var build = FmodStudioLocator.BuildBanksDetailed();
            var message = build.ToDisplayString();
            var detected = FmodProjectLinker.DetectBankOutput();
            if (!string.IsNullOrEmpty(detected))
                FmodEditorSettings.BankOutputPath = detected;
            RunAndRefresh(message);
            _result.TextColor = build.Success ? Color.LightGreen : Color.OrangeRed;
        }

        private void ImportAndSynchronize()
        {
            var ok = FmodProjectLinker.ImportAndSynchronize(out var message);
            if (ok)
                ShowStep(5);
            else
                RefreshNavigation();
            _result.Text = (ok ? "PASS\n" : "FAIL\n") + message;
        }

        private void RunAndRefresh(string result)
        {
            ShowStep(_step);
            _result.Text = result;
        }

        private void RefreshNavigation()
        {
            for (var i = 0; i < _steps.Length; i++)
            {
                var pass = GetStepState(i);
                _steps[i].Text = (pass ? "PASS  " : "TODO  ") + StepNames[i];
                _steps[i].TextColor = pass ? Color.LightGreen : Color.Orange;
            }
        }

        internal void StartRuntimeValidation()
        {
            StopValidationHandle();
            _validation2DPeak = 0;
            _validation3DNearPeak = 0;
            _validation3DFarPeak = 0;
            _validationTimer = 0;
            _validationPhase = Editor.IsPlayMode ? ValidationPhase.Probe2D : ValidationPhase.WaitingForPlay;
            _primary.Enabled = false;
            _result.TextColor = Color.Yellow;
            _result.Text = Editor.IsPlayMode
                ? "RUNNING - measuring the 2D output probe..."
                : "RUNNING - starting Play mode, then measuring 2D and 3D output probes...";
            if (!Editor.IsPlayMode)
                Editor.Simulation.RequestStartPlayScenes();
            else
                Begin2DProbe();
        }

        private void Begin2DProbe()
        {
            _validationTimer = 0;
            _validationPhase = ValidationPhase.Probe2D;
            // Isolate the probes from authored scene voices so master output can be
            // compared at near/far distance without unrelated station ambience.
            SuspendSceneEmitters();
            AudioEventSystem.StopAll(AudioStopMode.Immediate);
            _validationHandle = AudioEventSystem.CreatePreviewInstance(new Guid("92479e97-94c6-4564-9e33-efbeec59b34a"), "event:/UI/Okay", new AudioEventCreateOptions());
            if (_validationHandle.Generation == 0 || !AudioEventSystem.PlayPreview(_validationHandle))
            {
                FinishRuntimeValidation(false, "2D probe event:/UI/Okay could not be created or played. " + LastFmodError());
                return;
            }
            _result.Text = "RUNNING 1/3 - playing event:/UI/Okay and measuring master output...";
        }

        private void Begin3DProbe(bool far)
        {
            StopValidationHandle();
            _validationTimer = 0;
            var distanceMeters = far ? 35.0f : 1.0f;
            var listener = Level.GetActors<AudioListener>().FirstOrDefault();
            var listenerPosition = listener?.AttenuationActor?.Position ?? listener?.Position ?? Vector3.Zero;
            var options = new AudioEventCreateOptions
            {
                Attributes = new Audio3DAttributes
                {
                    Position = listenerPosition + new Vector3(distanceMeters * 100.0f, 0, 0),
                    Velocity = Vector3.Zero,
                    Forward = Vector3.Forward,
                    Up = Vector3.Up,
                }
            };
            var id = new Guid("0c8363b4-23af-4f9c-af4b-0951bfd37d84");
            _validationHandle = AudioEventSystem.CreatePreviewInstance(id, "event:/Vehicles/Car Engine", options);
            if (_validationHandle.Generation == 0)
            {
                FinishRuntimeValidation(false, "3D probe event:/Vehicles/Car Engine could not be created. " + LastFmodError());
                return;
            }
            if (AudioEventSystem.GetEventParameters(id, "event:/Vehicles/Car Engine", out var parameters))
            {
                foreach (var parameter in parameters)
                    AudioEventSystem.SetParameter(_validationHandle, parameter.Id, (parameter.Minimum + parameter.Maximum) * 0.5f, false);
            }
            if (!AudioEventSystem.PlayPreview(_validationHandle))
            {
                FinishRuntimeValidation(false, "3D probe event:/Vehicles/Car Engine could not be played. " + LastFmodError());
                return;
            }
            _validationPhase = far ? ValidationPhase.Probe3DFar : ValidationPhase.Probe3DNear;
            _result.Text = far
                ? "RUNNING 3/3 - measuring the 3D event beyond its 20 m authored range..."
                : "RUNNING 2/3 - measuring the 3D event at its 1 m minimum distance...";
        }

        public override void Update(float deltaTime)
        {
            base.Update(deltaTime);
            if (_validationPhase == ValidationPhase.Idle)
                return;
            _validationTimer += deltaTime;
            if (_validationPhase == ValidationPhase.WaitingForPlay)
            {
                if (Editor.IsPlayMode && _validationTimer > 0.5f)
                    Begin2DProbe();
                else if (_validationTimer > 20.0f)
                    FinishRuntimeValidation(false, "Play mode did not start within 20 seconds.");
                return;
            }
            AudioEventSystem.CaptureDiagnostics(out var diagnostics);
            switch (_validationPhase)
            {
            case ValidationPhase.Probe2D:
                if (_validationTimer >= 0.2f)
                    _validation2DPeak = Math.Max(_validation2DPeak, diagnostics.CombinedOutputRms);
                if (_validationTimer >= 1.25f)
                    Begin3DProbe(false);
                break;
            case ValidationPhase.Probe3DNear:
                if (_validationTimer >= 0.35f)
                    _validation3DNearPeak = Math.Max(_validation3DNearPeak, diagnostics.CombinedOutputRms);
                if (_validationTimer >= 1.5f)
                    Begin3DProbe(true);
                break;
            case ValidationPhase.Probe3DFar:
                if (_validationTimer >= 0.35f)
                    _validation3DFarPeak = Math.Max(_validation3DFarPeak, diagnostics.CombinedOutputRms);
                if (_validationTimer >= 1.25f)
                {
                    var outputPassed = _validation2DPeak > 0.00001f && _validation3DNearPeak > 0.00001f;
                    var attenuationPassed = _validation3DFarPeak <= _validation3DNearPeak * 0.9f || _validation3DFarPeak < 0.00001f;
                    var report = $"2D master RMS {_validation2DPeak:0.000000}; isolated 3D near RMS {_validation3DNearPeak:0.000000}; isolated 3D far RMS {_validation3DFarPeak:0.000000}; attenuation {(attenuationPassed ? "PASS" : "FAIL")}";
                    FinishRuntimeValidation(outputPassed && attenuationPassed, report);
                }
                break;
            }
        }

        private void FinishRuntimeValidation(bool passed, string report)
        {
            StopValidationHandle();
            RestoreSceneEmitters();
            _validationPhase = ValidationPhase.Idle;
            FmodSetupWizard.RecordRuntimeValidation(passed, report);
            _primary.Enabled = true;
            ShowStep(9);
            _result.Text = (passed ? "PASS\n" : "FAIL\n") + report;
            _result.TextColor = passed ? Color.LightGreen : Color.OrangeRed;
        }

        private void StopValidationHandle()
        {
            if (_validationHandle.Generation != 0)
                AudioEventSystem.StopAndRelease(_validationHandle, AudioStopMode.Immediate);
            _validationHandle = default;
        }

        private void SuspendSceneEmitters()
        {
            if (_suspendedEmitters != null)
                return;
            _suspendedEmitters = Level.GetActors<AudioEmitter>()
                .Where(x => x != null && x.IsActive)
                .ToArray();
            foreach (var emitter in _suspendedEmitters)
                emitter.IsActive = false;
        }

        private void RestoreSceneEmitters()
        {
            if (_suspendedEmitters == null)
                return;
            foreach (var emitter in _suspendedEmitters)
                if (emitter != null)
                    emitter.IsActive = true;
            _suspendedEmitters = null;
        }

        private static string LastFmodError()
        {
            AudioEventSystem.CaptureDiagnostics(out var diagnostics);
            return diagnostics.LastErrorCode == 0 ? "No backend error was reported." : $"FMOD {diagnostics.LastErrorCode}: {diagnostics.LastError}";
        }

        public override void OnDestroy()
        {
            StopValidationHandle();
            RestoreSceneEmitters();
            base.OnDestroy();
        }
    }
}
