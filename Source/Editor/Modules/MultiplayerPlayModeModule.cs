// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Manages synchronized editor instances for local multiplayer testing.
    /// </summary>
    [HideInEditor]
    public sealed class MultiplayerPlayModeModule : EditorModule
    {
        /// <summary>
        /// The maximum number of read-only replicas controlled by the primary editor.
        /// </summary>
        public const int MaxReplicaCount = 3;

        private sealed class ReplicaConnection : IDisposable
        {
            public readonly int Index;
            public readonly TcpClient Client;
            public readonly StreamWriter Writer;
            public volatile string SceneState;
            public volatile string ReloadToken;

            public ReplicaConnection(int index, TcpClient client, StreamWriter writer)
            {
                Index = index;
                Client = client;
                Writer = writer;
            }

            public bool Send(string message)
            {
                try
                {
                    lock (Writer)
                    {
                        Writer.WriteLine(message);
                        Writer.Flush();
                    }
                    return true;
                }
                catch
                {
                    return false;
                }
            }

            public void Dispose()
            {
                Writer.Dispose();
                Client.Dispose();
            }
        }

        private const string SessionArgument = "-mppmSession";
        private const string PortArgument = "-mppmPort";
        private const string IndexArgument = "-mppmIndex";
        private const string CountArgument = "-mppmCount";
        private const string TagsArgument = "-mppmTags";

        private readonly object _connectionsLock = new object();
        private readonly Dictionary<int, ReplicaConnection> _connections = new Dictionary<int, ReplicaConnection>();
        private readonly ConcurrentQueue<string> _commands = new ConcurrentQueue<string>();
        private readonly ConcurrentQueue<ReplicaConnection> _newConnections = new ConcurrentQueue<ReplicaConnection>();
        private readonly string _replicaSession;
        private readonly int _replicaPort;
        private volatile TcpListener _listener;
        private volatile TcpClient _primaryConnection;
        private volatile StreamWriter _primaryWriter;
        private string[] _availableTags = Array.Empty<string>();
        private string[] _primaryTags = Array.Empty<string>();
        private bool[] _replicaEnabled = new bool[MaxReplicaCount];
        private string[][] _replicaTags = new string[MaxReplicaCount][];
        private string _session;
        private string _scriptsReloadToken;
        private string _lastSceneState;
        private Guid[] _requestedScenes;
        private bool? _requestedPlayState;
        private bool? _requestedPauseState;
        private bool _enabled;
        private bool _initialized;
        private bool _restartRequested;
        private bool _scriptsReloadRequested;
        private bool _scriptsReloadInProgress;
        private bool _sceneSyncPending;
        private volatile bool _connectionsChanged;
        private volatile bool _stopping;
        private bool _exitRequested;
        private bool _lastPauseState;

        /// <summary>
        /// Gets a value indicating whether this editor is a read-only multiplayer replica.
        /// </summary>
        public bool IsReplica { get; }

        /// <summary>
        /// Gets a value indicating whether multiplayer play mode is active in this editor.
        /// </summary>
        public bool IsActive => IsReplica || _enabled;

        /// <summary>
        /// Gets the zero-based multiplayer instance index.
        /// </summary>
        public int InstanceIndex { get; }

        /// <summary>
        /// Gets the multiplayer instance tag.
        /// </summary>
        public string InstanceTag { get; private set; }

        /// <summary>
        /// Gets the multiplayer instance tags.
        /// </summary>
        public string[] InstanceTags { get; private set; } = Array.Empty<string>();

        /// <summary>
        /// Gets the configured number of multiplayer instances.
        /// </summary>
        public int InstanceCount { get; private set; }

        private int ExpectedReplicaCount
        {
            get
            {
                var count = 0;
                for (int i = 0; i < _replicaEnabled.Length; i++)
                    count += _replicaEnabled[i] ? 1 : 0;
                return count;
            }
        }

        /// <summary>
        /// Gets the number of connected read-only replicas.
        /// </summary>
        public int ConnectedReplicaCount
        {
            get
            {
                lock (_connectionsLock)
                    return _connections.Count;
            }
        }

        /// <summary>
        /// Gets the number of connected replicas that have loaded the primary editor's scenes.
        /// </summary>
        public int ReadyReplicaCount
        {
            get
            {
                var sceneState = GetSceneState();
                var count = 0;
                lock (_connectionsLock)
                {
                    foreach (var connection in _connections.Values)
                    {
                        if (connection.ReloadToken == null && string.Equals(connection.SceneState, sceneState, StringComparison.Ordinal))
                            count++;
                    }
                }
                return count;
            }
        }

        /// <summary>
        /// Gets a value indicating whether all configured multiplayer replicas are connected.
        /// </summary>
        public bool AreReplicasReady
        {
            get
            {
                if (IsReplica || !_enabled)
                    return true;
                var sceneState = GetSceneState();
                lock (_connectionsLock)
                {
                    if (_connections.Count != ExpectedReplicaCount)
                        return false;
                    for (int i = 0; i < _replicaEnabled.Length; i++)
                    {
                        if (!_replicaEnabled[i])
                            continue;
                        var index = i + 1;
                        if (!_connections.ContainsKey(index))
                            return false;
                        if (_connections[index].ReloadToken != null)
                            return false;
                        if (!string.Equals(_connections[index].SceneState, sceneState, StringComparison.Ordinal))
                            return false;
                    }
                }
                return true;
            }
        }

        internal MultiplayerPlayModeModule(Editor editor)
        : base(editor)
        {
            InitOrder = -950000;

            var arguments = Environment.GetCommandLineArgs();
            _replicaSession = GetArgument(arguments, SessionArgument);
            IsReplica = !string.IsNullOrEmpty(_replicaSession);
            if (IsReplica)
            {
                int.TryParse(GetArgument(arguments, PortArgument), NumberStyles.Integer, CultureInfo.InvariantCulture, out _replicaPort);
                int.TryParse(GetArgument(arguments, IndexArgument), NumberStyles.Integer, CultureInfo.InvariantCulture, out var instanceIndex);
                int.TryParse(GetArgument(arguments, CountArgument), NumberStyles.Integer, CultureInfo.InvariantCulture, out var instanceCount);
                InstanceIndex = instanceIndex;
                InstanceCount = Math.Max(1, instanceCount);
                InstanceTags = DecodeTags(GetArgument(arguments, TagsArgument));
                InstanceTag = InstanceTags.Length != 0 ? InstanceTags[0] : string.Empty;
            }
            else
            {
                InstanceIndex = 0;
                InstanceCount = 1;
                InstanceTag = string.Empty;
            }
            FlaxEngine.MultiplayerPlayMode.Configure(IsReplica, IsReplica, InstanceIndex, InstanceCount, InstanceTags);
        }

        private static string GetArgument(string[] arguments, string name)
        {
            for (int i = 0; i + 1 < arguments.Length; i++)
            {
                if (string.Equals(arguments[i], name, StringComparison.OrdinalIgnoreCase))
                    return arguments[i + 1];
            }
            return null;
        }

        private static string EncodeTags(string[] tags)
        {
            return Convert.ToBase64String(Encoding.UTF8.GetBytes(string.Join("\n", tags ?? Array.Empty<string>())));
        }

        private static string[] DecodeTags(string tags)
        {
            if (string.IsNullOrEmpty(tags))
                return Array.Empty<string>();
            try
            {
                var value = Encoding.UTF8.GetString(Convert.FromBase64String(tags));
                return string.IsNullOrEmpty(value) ? Array.Empty<string>() : value.Split('\n');
            }
            catch
            {
                return Array.Empty<string>();
            }
        }

        private void ApplyOptions()
        {
            var options = Editor.Options.Options.General;
            var availableTags = new List<string>();
            var configuredTags = options.MultiplayerPlayModeTags;
            if (configuredTags != null)
            {
                for (int i = 0; i < configuredTags.Length; i++)
                {
                    var tag = configuredTags[i]?.Trim();
                    if (!string.IsNullOrEmpty(tag) && !availableTags.Contains(tag))
                        availableTags.Add(tag);
                }
            }
            _availableTags = availableTags.ToArray();

            var primaryTags = new List<string>();
            var configuredPrimaryTags = options.MultiplayerPlayModePrimaryTags;
            if (configuredPrimaryTags != null)
            {
                for (int i = 0; i < configuredPrimaryTags.Length; i++)
                {
                    if (Array.IndexOf(_availableTags, configuredPrimaryTags[i]) != -1 && !primaryTags.Contains(configuredPrimaryTags[i]))
                        primaryTags.Add(configuredPrimaryTags[i]);
                }
            }
            _primaryTags = primaryTags.ToArray();

            var enabled = options.MultiplayerPlayModeReplicasEnabled;
            var assignedTags = options.MultiplayerPlayModeReplicaTags;
            _replicaEnabled = new bool[MaxReplicaCount];
            _replicaTags = new string[MaxReplicaCount][];
            for (int i = 0; i < MaxReplicaCount; i++)
            {
                _replicaEnabled[i] = enabled != null && i < enabled.Length && enabled[i];
                var tags = new List<string>();
                var source = assignedTags != null && i < assignedTags.Length ? assignedTags[i] : null;
                if (source != null)
                {
                    for (int j = 0; j < source.Length; j++)
                    {
                        if (Array.IndexOf(_availableTags, source[j]) != -1 && !tags.Contains(source[j]))
                            tags.Add(source[j]);
                    }
                }
                _replicaTags[i] = tags.ToArray();
            }

            _enabled = ExpectedReplicaCount != 0;
            InstanceCount = 1 + ExpectedReplicaCount;
            InstanceTags = (string[])_primaryTags.Clone();
            InstanceTag = InstanceTags.Length != 0 ? InstanceTags[0] : string.Empty;
            FlaxEngine.MultiplayerPlayMode.Configure(_enabled, false, 0, InstanceCount, InstanceTags);
        }

        /// <summary>
        /// Gets the available tags configured in Editor Options.
        /// </summary>
        public IReadOnlyList<string> AvailableTags => _availableTags;

        /// <summary>
        /// Gets whether a tag is assigned to the primary instance.
        /// </summary>
        public bool PrimaryHasTag(string tag)
        {
            return Array.IndexOf(_primaryTags, tag) != -1;
        }

        /// <summary>
        /// Gets whether a replica slot is enabled.
        /// </summary>
        public bool IsReplicaEnabled(int replicaIndex)
        {
            return replicaIndex >= 0 && replicaIndex < _replicaEnabled.Length && _replicaEnabled[replicaIndex];
        }

        /// <summary>
        /// Gets whether a tag is assigned to a replica slot.
        /// </summary>
        public bool ReplicaHasTag(int replicaIndex, string tag)
        {
            return replicaIndex >= 0 && replicaIndex < _replicaTags.Length && Array.IndexOf(_replicaTags[replicaIndex], tag) != -1;
        }

        internal void SetPrimaryTag(string tag, bool enabled)
        {
            if (IsReplica || Array.IndexOf(_availableTags, tag) == -1)
                return;
            var tags = new List<string>(_primaryTags);
            if (enabled)
            {
                if (!tags.Contains(tag))
                    tags.Add(tag);
            }
            else
            {
                tags.Remove(tag);
            }
            _primaryTags = tags.ToArray();
            SaveToolbarOptions();
        }

        internal void SetReplicaEnabled(int replicaIndex, bool enabled)
        {
            if (IsReplica || replicaIndex < 0 || replicaIndex >= MaxReplicaCount || _replicaEnabled[replicaIndex] == enabled)
                return;
            _replicaEnabled[replicaIndex] = enabled;
            SaveToolbarOptions();
        }

        internal void SetReplicaTag(int replicaIndex, string tag, bool enabled)
        {
            if (IsReplica || replicaIndex < 0 || replicaIndex >= MaxReplicaCount || Array.IndexOf(_availableTags, tag) == -1)
                return;
            var tags = new List<string>(_replicaTags[replicaIndex]);
            if (enabled)
            {
                if (!tags.Contains(tag))
                    tags.Add(tag);
            }
            else
            {
                tags.Remove(tag);
            }
            _replicaTags[replicaIndex] = tags.ToArray();
            SaveToolbarOptions();
        }

        private void SaveToolbarOptions()
        {
            var options = Editor.Options.Options.General;
            options.MultiplayerPlayModePrimaryTags = (string[])_primaryTags.Clone();
            options.MultiplayerPlayModeReplicasEnabled = (bool[])_replicaEnabled.Clone();
            options.MultiplayerPlayModeReplicaTags = new string[MaxReplicaCount][];
            for (int i = 0; i < MaxReplicaCount; i++)
                options.MultiplayerPlayModeReplicaTags[i] = (string[])_replicaTags[i].Clone();
            Editor.Options.SaveOptions();
            ApplyOptions();
            _restartRequested = _initialized;
        }

        private void OnOptionsChanged(Options.EditorOptions options)
        {
            if (IsReplica)
                return;
            ApplyOptions();
            _restartRequested = _initialized;
        }

        private void OnScriptsReloadBegin()
        {
            if (_listener == null)
                return;
            _scriptsReloadToken = Guid.NewGuid().ToString("N");
            lock (_connectionsLock)
            {
                foreach (var connection in _connections.Values)
                {
                    connection.SceneState = null;
                    connection.ReloadToken = _scriptsReloadToken;
                }
            }
            _connectionsChanged = true;
            Broadcast("RELOAD|" + _scriptsReloadToken);
        }

        private void OnScriptsReloadEnd()
        {
            if (_listener == null)
                return;
            _lastSceneState = null;
            _connectionsChanged = true;
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            if (!IsReplica)
            {
                ApplyOptions();
                Editor.Options.OptionsChanged += OnOptionsChanged;
                ScriptsBuilder.ScriptsReloadBegin += OnScriptsReloadBegin;
                ScriptsBuilder.ScriptsReloadEnd += OnScriptsReloadEnd;
            }
        }

        /// <inheritdoc />
        public override void OnEndInit()
        {
            _initialized = true;
            if (IsReplica)
            {
                Editor.Windows.ContentWin.Enabled = false;
                Editor.Windows.ToolboxWin.Enabled = false;
                for (int i = 0; i < Editor.Windows.Windows.Count; i++)
                    DisableAssetEditing(Editor.Windows.Windows[i]);
                Editor.Windows.WindowAdded += DisableAssetEditing;
                StartReplicaClient();
            }
            else if (_enabled)
                StartPrimarySession();
            Editor.Windows.UpdateWindowTitle();
        }

        private void StartPrimarySession()
        {
            if (_listener != null)
                return;

            _stopping = false;
            _session = Guid.NewGuid().ToString("N");
            _listener = new TcpListener(IPAddress.Loopback, 0);
            _listener.Start();
            var listener = _listener;
            var session = _session;
            var enabledReplicas = (bool[])_replicaEnabled.Clone();
            var port = ((IPEndPoint)_listener.LocalEndpoint).Port;
            Task.Run(() => AcceptReplicas(listener, session, enabledReplicas));

            for (int i = 0; i < _replicaEnabled.Length; i++)
            {
                if (!_replicaEnabled[i])
                    continue;
                var instanceIndex = i + 1;
                var processSettings = new CreateProcessSettings
                {
                    FileName = Platform.ExecutableFilePath,
                    Arguments = $"-project \"{Globals.ProjectFolder}\" -skipcompile {SessionArgument} {_session} {PortArgument} {port} {IndexArgument} {instanceIndex} {CountArgument} {InstanceCount} {TagsArgument} {EncodeTags(_replicaTags[i])}",
                    WorkingDirectory = Globals.ProjectFolder,
                    ShellExecute = true,
                    WaitForEnd = false,
                    HiddenWindow = false,
                    LogOutput = false,
                };
                if (Platform.CreateProcess(ref processSettings) != 0)
                    Editor.LogError($"Failed to launch multiplayer replica {instanceIndex}.");
            }

            Editor.Log($"[Multiplayer Play Mode] Waiting for {ExpectedReplicaCount} replica(s).");
            _connectionsChanged = true;
        }

        private void DisableAssetEditing(EditorWindow window)
        {
            if (window is AssetEditorWindow)
                window.Enabled = false;
        }

        private void AcceptReplicas(TcpListener listener, string session, bool[] enabledReplicas)
        {
            while (ReferenceEquals(_listener, listener))
            {
                TcpClient client;
                try
                {
                    client = listener.AcceptTcpClient();
                }
                catch
                {
                    return;
                }
                Task.Run(() => HandleReplica(client, session, enabledReplicas));
            }
        }

        private void HandleReplica(TcpClient client, string session, bool[] enabledReplicas)
        {
            ReplicaConnection connection = null;
            try
            {
                var reader = new StreamReader(client.GetStream(), Encoding.UTF8, false, 1024, true);
                var writer = new StreamWriter(client.GetStream(), Encoding.UTF8, 1024, true);
                var hello = reader.ReadLine()?.Split('|');
                if (hello == null || hello.Length != 3 || hello[0] != "HELLO" || hello[1] != session ||
                    !int.TryParse(hello[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out var index) ||
                    index <= 0 || index > enabledReplicas.Length || !enabledReplicas[index - 1])
                {
                    client.Dispose();
                    return;
                }

                connection = new ReplicaConnection(index, client, writer);
                connection.ReloadToken = _scriptsReloadToken;
                lock (_connectionsLock)
                {
                    if (_connections.TryGetValue(index, out var previous))
                        previous.Dispose();
                    _connections[index] = connection;
                }
                _connectionsChanged = true;
                _newConnections.Enqueue(connection);

                string message;
                while (!_stopping && (message = reader.ReadLine()) != null)
                {
                    if (message.StartsWith("READY|", StringComparison.Ordinal))
                    {
                        if (connection.ReloadToken == null)
                        {
                            connection.SceneState = message.Substring(6);
                            _connectionsChanged = true;
                        }
                    }
                    else if (message.StartsWith("RELOADED|", StringComparison.Ordinal))
                    {
                        var payload = message.Substring(9);
                        var separator = payload.IndexOf('|');
                        if (separator != -1)
                        {
                            var token = payload.Substring(0, separator);
                            if (string.Equals(connection.ReloadToken, token, StringComparison.Ordinal))
                            {
                                connection.ReloadToken = null;
                                connection.SceneState = payload.Substring(separator + 1);
                                _connectionsChanged = true;
                            }
                        }
                    }
                }
            }
            catch
            {
            }
            finally
            {
                if (connection != null)
                {
                    lock (_connectionsLock)
                    {
                        if (_connections.TryGetValue(connection.Index, out var current) && ReferenceEquals(current, connection))
                            _connections.Remove(connection.Index);
                    }
                    connection.Dispose();
                    _connectionsChanged = true;
                }
                else
                {
                    client.Dispose();
                }
            }
        }

        private void StartReplicaClient()
        {
            if (_replicaPort <= 0)
            {
                Editor.LogError("Invalid multiplayer play mode connection settings.");
                return;
            }
            _stopping = false;
            Task.Run(ConnectToPrimary);
        }

        private void ConnectToPrimary()
        {
            while (!_stopping)
            {
                try
                {
                    var client = new TcpClient();
                    client.Connect(IPAddress.Loopback, _replicaPort);
                    _primaryConnection = client;
                    var reader = new StreamReader(client.GetStream(), Encoding.UTF8, false, 1024, true);
                    var writer = new StreamWriter(client.GetStream(), Encoding.UTF8, 1024, true);
                    _primaryWriter = writer;
                    writer.WriteLine($"HELLO|{_replicaSession}|{InstanceIndex}");
                    writer.Flush();

                    string command;
                    while (!_stopping && (command = reader.ReadLine()) != null)
                        _commands.Enqueue(command);
                    if (!_stopping)
                        _commands.Enqueue("CLOSE");
                    _primaryWriter = null;
                    return;
                }
                catch
                {
                    _primaryConnection?.Dispose();
                    _primaryConnection = null;
                    _primaryWriter = null;
                    if (!_stopping)
                        System.Threading.Thread.Sleep(250);
                }
            }
        }

        private void SendCurrentState(ReplicaConnection connection)
        {
            if (!string.IsNullOrEmpty(connection.ReloadToken))
                connection.Send("RELOAD|" + connection.ReloadToken);
            connection.Send("SCENES|" + GetSceneState());
            connection.Send(Editor.StateMachine.IsPlayMode ? "PLAY|START" : "PLAY|STOP");
            if (Editor.StateMachine.IsPlayMode)
                connection.Send(Editor.StateMachine.PlayingState.IsPaused ? "PAUSE|1" : "PAUSE|0");
        }

        private void SendToPrimary(string message)
        {
            var writer = _primaryWriter;
            if (writer == null)
                return;
            try
            {
                lock (writer)
                {
                    writer.WriteLine(message);
                    writer.Flush();
                }
            }
            catch
            {
            }
        }

        private string GetSceneState()
        {
            var builder = new StringBuilder();
            for (int i = 0; i < Level.ScenesCount; i++)
            {
                if (i != 0)
                    builder.Append(',');
                builder.Append(Level.GetScene(i).ID.ToString("N"));
            }
            return builder.ToString();
        }

        private void Broadcast(string command)
        {
            lock (_connectionsLock)
            {
                foreach (var connection in _connections.Values)
                    connection.Send(command);
            }
        }

        private void SendSceneState(bool force = false)
        {
            var sceneState = GetSceneState();
            var changed = !string.Equals(sceneState, _lastSceneState, StringComparison.Ordinal);
            if (!force && !changed)
                return;
            _lastSceneState = sceneState;
            Broadcast("SCENES|" + sceneState);
            if (changed)
                _connectionsChanged = true;
        }

        private void StopPrimarySession()
        {
            _stopping = true;
            Broadcast("CLOSE");
            _listener?.Stop();
            _listener = null;
            lock (_connectionsLock)
            {
                foreach (var connection in _connections.Values)
                    connection.Dispose();
                _connections.Clear();
            }
            _session = null;
            _lastSceneState = null;
            _connectionsChanged = true;
        }

        private void ProcessCommand(string command)
        {
            var separator = command.IndexOf('|');
            var name = separator < 0 ? command : command.Substring(0, separator);
            var value = separator < 0 ? string.Empty : command.Substring(separator + 1);
            switch (name)
            {
            case "SCENES":
                if (string.IsNullOrEmpty(value))
                {
                    _requestedScenes = Array.Empty<Guid>();
                }
                else
                {
                    var values = value.Split(',');
                    var scenes = new List<Guid>(values.Length);
                    for (int i = 0; i < values.Length; i++)
                    {
                        if (Guid.TryParse(values[i], out var id))
                            scenes.Add(id);
                    }
                    _requestedScenes = scenes.ToArray();
                }
                _sceneSyncPending = true;
                break;
            case "PLAY":
                _requestedPlayState = value == "START";
                break;
            case "PAUSE":
                _requestedPauseState = value == "1";
                break;
            case "RELOAD":
                _scriptsReloadRequested = true;
                _scriptsReloadToken = value;
                break;
            case "CLOSE":
                _exitRequested = true;
                break;
            }
        }

        private bool ScenesMatchRequest()
        {
            if (_requestedScenes == null || Level.ScenesCount != _requestedScenes.Length)
                return false;
            for (int i = 0; i < _requestedScenes.Length; i++)
            {
                if (Level.GetScene(i).ID != _requestedScenes[i])
                    return false;
            }
            return true;
        }

        private void UpdateReplica()
        {
            while (_commands.TryDequeue(out var command))
                ProcessCommand(command);

            if (_exitRequested)
            {
                Engine.RequestExit(0);
                return;
            }

            if (_scriptsReloadRequested && Editor.StateMachine.IsPlayMode)
            {
                _requestedPlayState = null;
                Editor.Simulation.RequestStopPlayFromMultiplayer();
                return;
            }

            if (_scriptsReloadRequested && !_scriptsReloadInProgress && Editor.StateMachine.IsEditMode && !Level.IsAnyActionPending)
            {
                _scriptsReloadRequested = false;
                _scriptsReloadInProgress = true;
                FlaxEngine.Scripting.Reload();
            }

            if (_scriptsReloadInProgress && Editor.StateMachine.IsEditMode && !Level.IsAnyActionPending)
            {
                _scriptsReloadInProgress = false;
                var token = _scriptsReloadToken;
                _scriptsReloadToken = null;
                SendToPrimary(!string.IsNullOrEmpty(token) ? "RELOADED|" + token + "|" + GetSceneState() : "READY|" + GetSceneState());
            }

            if (_scriptsReloadRequested || _scriptsReloadInProgress)
                return;

            if (_sceneSyncPending && Editor.StateMachine.IsEditMode && !Level.IsAnyActionPending)
            {
                if (!ScenesMatchRequest())
                {
                    Editor.Scene.ClearRefsToSceneObjects();
                    Level.UnloadAllScenes();
                    for (int i = 0; i < _requestedScenes.Length; i++)
                        Level.LoadScene(_requestedScenes[i]);
                }
                _sceneSyncPending = false;
                SendToPrimary("READY|" + GetSceneState());
            }

            if (_requestedPlayState.HasValue)
            {
                if (_requestedPlayState.Value)
                {
                    if (!_sceneSyncPending && ScenesMatchRequest() && Editor.StateMachine.IsEditMode && !Level.IsAnyActionPending && Level.IsAnySceneLoaded)
                    {
                        _requestedPlayState = null;
                        Editor.Simulation.RequestStartPlayScenesFromMultiplayer();
                    }
                }
                else if (Editor.StateMachine.IsPlayMode)
                {
                    _requestedPlayState = null;
                    Editor.Simulation.RequestStopPlayFromMultiplayer();
                }
                else
                {
                    _requestedPlayState = null;
                }
            }

            if (_requestedPauseState.HasValue && Editor.StateMachine.IsPlayMode)
            {
                Editor.Simulation.SetPausedFromMultiplayer(_requestedPauseState.Value);
                _requestedPauseState = null;
            }
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            if (IsReplica)
            {
                UpdateReplica();
                return;
            }

            if (_restartRequested && Editor.StateMachine.IsEditMode)
            {
                _restartRequested = false;
                StopPrimarySession();
                if (_enabled)
                    StartPrimarySession();
                Editor.Windows.UpdateWindowTitle();
            }

            while (_newConnections.TryDequeue(out var connection))
                SendCurrentState(connection);

            if (_listener != null && Editor.StateMachine.IsEditMode && !Level.IsAnyActionPending)
                SendSceneState();

            if (_listener != null && Editor.StateMachine.IsPlayMode)
            {
                var paused = Editor.StateMachine.PlayingState.IsPaused;
                if (paused != _lastPauseState)
                {
                    _lastPauseState = paused;
                    Broadcast(paused ? "PAUSE|1" : "PAUSE|0");
                }
            }

            if (_scriptsReloadToken != null && AreReplicasReady)
                _scriptsReloadToken = null;

            if (_connectionsChanged)
            {
                _connectionsChanged = false;
                Editor.UI.UpdateToolstrip();
                Editor.Log($"[Multiplayer Play Mode] {ReadyReplicaCount}/{ExpectedReplicaCount} replica(s) ready.");
            }
        }

        /// <inheritdoc />
        public override void OnPlayBeginning()
        {
            if (_listener == null)
                return;
            SendSceneState(true);
            Broadcast("PLAY|START");
            _lastPauseState = false;
        }

        /// <inheritdoc />
        public override void OnPlayEnding()
        {
            if (_listener != null)
                Broadcast("PLAY|STOP");
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            if (IsReplica)
                Editor.Windows.WindowAdded -= DisableAssetEditing;
            if (!IsReplica)
            {
                Editor.Options.OptionsChanged -= OnOptionsChanged;
                ScriptsBuilder.ScriptsReloadBegin -= OnScriptsReloadBegin;
                ScriptsBuilder.ScriptsReloadEnd -= OnScriptsReloadEnd;
            }
            _stopping = true;
            if (_listener != null)
                StopPrimarySession();
            _primaryConnection?.Dispose();
            _primaryConnection = null;
        }
    }
}
