// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>
    /// 
    /// </summary>
    public class FlaxVersionConverter : JsonConverter
    {
        // Original implementation is based on Newtonsoft.Json VersionConverter
        /// <summary>
        /// Writes the JSON representation of the object.
        /// </summary>
        /// <param name="writer">The <see cref="JsonWriter"/> to write to.</param>
        /// <param name="value">The value.</param>
        /// <param name="serializer">The calling serializer.</param>
        public override void WriteJson(JsonWriter writer, object value, JsonSerializer serializer)
        {
            if (value == null)
                writer.WriteNull();
            else if (value is Version)
                writer.WriteValue(value.ToString());
            else
                throw new JsonSerializationException("Expected Version object value");
        }

        /// <summary>
        /// Reads the JSON representation of the object.
        /// </summary>
        /// <param name="reader">The <see cref="JsonReader"/> to read from.</param>
        /// <param name="objectType">Type of the object.</param>
        /// <param name="existingValue">The existing property value of the JSON that is being converted.</param>
        /// <param name="serializer">The calling serializer.</param>
        /// <returns>The object value.</returns>
        public override object ReadJson(JsonReader reader, Type objectType, object existingValue, JsonSerializer serializer)
        {
            if (reader.TokenType == JsonToken.Null)
                return null;

            if (reader.TokenType == JsonToken.StartObject)
            {
                try
                {
                    reader.Read();
                    var values = new Dictionary<string, int>();
                    while (reader.TokenType == JsonToken.PropertyName)
                    {
                        var key = reader.Value as string;
                        reader.Read();
                        var val = (long)reader.Value;
                        reader.Read();
                        values.Add(key, (int)val);
                    }

                    values.TryGetValue("Major", out var major);
                    values.TryGetValue("Minor", out var minor);
                    if (!values.TryGetValue("Build", out var build))
                        build = -1;
                    if (!values.TryGetValue("Revision", out var revision))
                        revision = -1;

                    if (build <= 0)
                        return new Version(major, minor);
                    if (revision <= 0)
                        return new Version(major, minor, build);
                    return new Version(major, minor, build, revision);
                }
                catch (Exception ex)
                {
                    throw new Exception(String.Format("Error parsing version string: {0}", reader.Value), ex);
                }
            }
            if (reader.TokenType == JsonToken.String)
            {
                try
                {
                    return new Version((string)reader.Value!);
                }
                catch (Exception ex)
                {
                    throw new Exception(String.Format("Error parsing version string: {0}", reader.Value), ex);
                }
            }
            throw new Exception(String.Format("Unexpected token or value when parsing version. Token: {0}, Value: {1}", reader.TokenType, reader.Value));
        }

        /// <summary>
        /// Determines whether this instance can convert the specified object type.
        /// </summary>
        /// <param name="objectType">Type of the object.</param>
        /// <returns><c>true</c> if this instance can convert the specified object type; otherwise, <c>false</c>.</returns>
        public override bool CanConvert(Type objectType)
        {
            return objectType == typeof(Version);
        }
    }

    /// <summary>
    /// Contains information about Flax project.
    /// </summary>
    public sealed class ProjectInfo
    {
        /// <summary>Asset-system format supported by this editor.</summary>
        public const int CurrentAssetSystemVersion = 3;

        /// <summary>Artifact layout supported by this editor.</summary>
        public const int CurrentArtifactLayoutVersion = 2;

        /// <summary>Authored source-document format supported by this editor.</summary>
        public const int CurrentSourceDocumentVersion = 1;

        private static List<ProjectInfo> _projectsCache;

        /// <summary>
        /// The project reference.
        /// </summary>
        public class Reference
        {
            /// <summary>
            /// The referenced project name.
            /// </summary>
            public string Name;

            /// <summary>
            /// The referenced project.
            /// </summary>
            [NonSerialized]
            public ProjectInfo Project;

            /// <inheritdoc />
            public override string ToString()
            {
                return Name;
            }
        }

        /// <summary>
        /// The project name.
        /// </summary>
        public string Name;

        /// <summary>
        /// The project file path.
        /// </summary>
        [NonSerialized]
        public string ProjectPath;

        /// <summary>
        /// The project root folder path.
        /// </summary>
        [NonSerialized]
        public string ProjectFolderPath;

        /// <summary>Committed one-way asset-system format marker. Zero means a pre-marker project.</summary>
        public int AssetSystemVersion;

        /// <summary>Stable bootstrap identity of an asset-system v3 project.</summary>
        public System.Guid ProjectId;

        /// <summary>The single writable source-root declaration.</summary>
        public string SourceRoot;

        /// <summary>The persistent source-object identity model.</summary>
        public string IdentityModel;

        /// <summary>The immutable artifact store layout version.</summary>
        public int ArtifactLayoutVersion;

        /// <summary>The authored source-document format version.</summary>
        public int SourceDocumentVersion;

        /// <summary>Whether this editor must not write this project's asset-system state.</summary>
        [NonSerialized]
        public bool AssetSystemReadOnly;

        /// <summary>
        /// The project version.
        /// </summary>
        public Version Version;

        /// <summary>
        /// The project publisher company.
        /// </summary>
        public string Company = string.Empty;

        /// <summary>
        /// The project copyright note.
        /// </summary>
        public string Copyright = string.Empty;

        /// <summary>
        /// The name of the build target to use for the game building (final, cooked game code).
        /// </summary>
        public string GameTarget;

        /// <summary>
        /// The name of the build target to use for the game in editor building (editor game code).
        /// </summary>
        public string EditorTarget;

        /// <summary>
        /// The project references.
        /// </summary>
        public Reference[] References = new Reference[0];

        /// <summary>
        /// The default scene asset identifier to open on project startup.
        /// </summary>
        public string DefaultScene;

        /// <summary>
        /// The default scene spawn point (position and view direction).
        /// </summary>
        public Ray DefaultSceneSpawn;

        /// <summary>
        /// The minimum version supported by this project.
        /// </summary>
        public Version MinEngineVersion;

        /// <summary>
        /// The user-friendly nickname of the engine installation to use when opening the project. Can be used to open game project with a custom engine distributed for team members. This value must be the same in engine and game projects to be paired.
        /// </summary>
        public string EngineNickname;

        /// <summary>
        /// Gets all projects including this project, it's references and their references (any deep level of references).
        /// </summary>
        /// <returns>The collection of projects.</returns>
        public HashSet<ProjectInfo> GetAllProjects()
        {
            var result = new HashSet<ProjectInfo>();
            GetAllProjects(result);
            return result;
        }

        private void GetAllProjects(HashSet<ProjectInfo> result)
        {
            result.Add(this);
            foreach (var reference in References)
                reference.Project.GetAllProjects(result);
        }

        /// <summary>
        /// Saves the project file.
        /// </summary>
        public void Save()
        {
            if (AssetSystemReadOnly)
                throw new InvalidOperationException($"Cannot save project descriptor with unsupported asset-system version {AssetSystemVersion}.");
            string contents;
            if (AssetSystemVersion == CurrentAssetSystemVersion)
            {
                SaveV3MutableSettings();
                var descriptor = new JObject
                {
                    ["ProjectId"] = ProjectId.ToString("N"),
                    ["AssetSystemVersion"] = AssetSystemVersion,
                    ["SourceRoot"] = SourceRoot,
                    ["IdentityModel"] = IdentityModel,
                    ["ArtifactLayoutVersion"] = ArtifactLayoutVersion,
                    ["SourceDocumentVersion"] = SourceDocumentVersion,
                    ["MinEngineVersion"] = MinEngineVersion?.ToString(),
                    ["References"] = JArray.FromObject(References.Select(x => new { x.Name })),
                };
                if (!string.IsNullOrEmpty(EngineNickname))
                    descriptor["EngineNickname"] = EngineNickname;
                contents = descriptor.ToString(Formatting.Indented);
            }
            else
            {
                contents = FlaxEngine.Json.JsonSerializer.Serialize(this);
            }
            File.WriteAllText(ProjectPath, contents);
        }

        private static JObject LoadSettingsData(string path)
        {
            var document = JObject.Parse(File.ReadAllText(path));
            return document["Data"] as JObject ?? throw new InvalidDataException($"Mandatory settings source has no Data object: {path}");
        }

        private static JToken MainAssetReference(string guid)
        {
            return Guid.TryParse(guid, out var id) && id != Guid.Empty
                ? new JObject
                {
                    ["guid"] = id.ToString("N"),
                    ["localId"] = 1,
                }
                : JValue.CreateNull();
        }

        private static string ReadMainAssetReference(JToken value)
        {
            if (!(value is JObject reference) || reference.Value<long?>("localId") != 1)
                return string.Empty;
            return Guid.TryParse(reference.Value<string>("guid"), out var id) && id != Guid.Empty ? id.ToString("N") : string.Empty;
        }

        private static void WriteSettingsData(string path, JObject data)
        {
            var document = JObject.Parse(File.ReadAllText(path));
            var typeName = document.Value<string>("TypeName");
            if (string.IsNullOrEmpty(typeName))
                throw new InvalidDataException($"Mandatory settings source has no TypeName: {path}");
            if (!Guid.TryParse(document.Value<string>("ID"), out var sourceId) || sourceId == Guid.Empty)
                throw new InvalidDataException($"Mandatory settings source has no valid ID: {path}");
            document["Data"] = data;
            var contents = Encoding.UTF8.GetBytes(document.ToString(Formatting.Indented));

            var contentDatabase = Editor.Instance?.ContentDatabase;
            contentDatabase?.BeginAssetSave(path);
            var failed = true;
            try
            {
                failed = AssetDatabaseFacade.SaveExistingJsonSourceBytes(path, contents, sourceId, typeName);
                if (failed)
                    throw new IOException($"Failed to save mandatory settings source: {path}");
            }
            finally
            {
                contentDatabase?.EndAssetSave(path, !failed);
            }
        }

        private void SaveV3MutableSettings()
        {
            var settings = Path.Combine(ProjectFolderPath, "Content", "Settings");
            var projectPath = Path.Combine(settings, "Project Settings.json");
            var buildPath = Path.Combine(settings, "Build Settings.json");
            var editorPath = Path.Combine(settings, "Editor Settings.json");
            var paths = new[] { projectPath, buildPath, editorPath };
            var callbackPaths = paths.Select(AssetDatabase.ToLogicalPathInternal).ToArray();
            var approved = new HashSet<string>(AssetPipelineCallbacks.WillSave(callbackPaths), StringComparer.OrdinalIgnoreCase);
            if (callbackPaths.Any(path => !approved.Contains(path)))
                throw new OperationCanceledException("Saving mandatory project settings was vetoed by an asset modification callback.");

            using (AssetPipelineCallbacks.BypassNativeDecision())
            {
            var project = LoadSettingsData(projectPath);
            project["ProductName"] = Name;
            project["Version"] = Version?.ToString() ?? "1.0";
            project["CompanyName"] = Company;
            project["CopyrightNotice"] = Copyright;
            project["FirstScene"] = MainAssetReference(DefaultScene);
            WriteSettingsData(projectPath, project);

            var build = LoadSettingsData(buildPath);
            build["GameTarget"] = GameTarget;
            build["EditorTarget"] = EditorTarget;
            WriteSettingsData(buildPath, build);

            var editor = LoadSettingsData(editorPath);
            editor["DefaultSceneSpawn"] = JToken.Parse(FlaxEngine.Json.JsonSerializer.Serialize(DefaultSceneSpawn));
            WriteSettingsData(editorPath, editor);
            }
        }

        private void LoadV3MutableSettings()
        {
            var settings = Path.Combine(ProjectFolderPath, "Content", "Settings");
            var project = LoadSettingsData(Path.Combine(settings, "Project Settings.json"));
            Name = project.Value<string>("ProductName");
            Version = new Version(project.Value<string>("Version") ?? "1.0");
            Company = project.Value<string>("CompanyName") ?? string.Empty;
            Copyright = project.Value<string>("CopyrightNotice") ?? string.Empty;
            DefaultScene = ReadMainAssetReference(project["FirstScene"]);

            var build = LoadSettingsData(Path.Combine(settings, "Build Settings.json"));
            GameTarget = build.Value<string>("GameTarget");
            EditorTarget = build.Value<string>("EditorTarget");

            var editor = LoadSettingsData(Path.Combine(settings, "Editor Settings.json"));
            if (editor.TryGetValue("DefaultSceneSpawn", out var spawn))
                DefaultSceneSpawn = FlaxEngine.Json.JsonSerializer.Deserialize<Ray>(spawn.ToString(Formatting.None));
        }

        /// <summary>Validates the committed asset-system marker.</summary>
        public bool ValidateAssetSystemMarker(out string error)
        {
            error = null;
            if (AssetSystemVersion == 0)
                return true;
            if (AssetSystemVersion < CurrentAssetSystemVersion)
            {
                error = $"Asset-system version {AssetSystemVersion} requires one-way migration to version {CurrentAssetSystemVersion}.";
                return false;
            }
            if (AssetSystemVersion > CurrentAssetSystemVersion)
            {
                error = $"Asset-system version {AssetSystemVersion} is newer than supported version {CurrentAssetSystemVersion}.";
                return false;
            }
            if (ProjectId == System.Guid.Empty)
            {
                error = "Asset-system v3 project identity is missing or invalid.";
                return false;
            }
            if (!string.Equals(SourceRoot, "Content", StringComparison.Ordinal))
            {
                error = "Asset-system source root must be exactly 'Content'.";
                return false;
            }
            if (!string.Equals(IdentityModel, "guid-local-id", StringComparison.Ordinal))
            {
                error = "Asset-system identity model must be exactly 'guid-local-id'.";
                return false;
            }
            if (ArtifactLayoutVersion != CurrentArtifactLayoutVersion)
            {
                error = $"Artifact layout version must be {CurrentArtifactLayoutVersion}.";
                return false;
            }
            if (SourceDocumentVersion != CurrentSourceDocumentVersion)
            {
                error = $"Source document version must be {CurrentSourceDocumentVersion}.";
                return false;
            }
            return true;
        }

        /// <summary>
        /// Loads the project from the specified file.
        /// </summary>
        /// <param name="path">The path.</param>
        /// <returns>The loaded project.</returns>
        public static ProjectInfo Load(string path)
        {
            // Try to reuse loaded file
            path = StringUtils.RemovePathRelativeParts(path);
            if (_projectsCache == null)
                _projectsCache = new List<ProjectInfo>();
            for (int i = 0; i < _projectsCache.Count; i++)
            {
                if (_projectsCache[i].ProjectPath == path)
                    return _projectsCache[i];
            }

            Profiler.BeginEvent(path);
            try
            {
                // Load
                var contents = File.ReadAllText(path);
                var project = JsonConvert.DeserializeObject<ProjectInfo>(contents, new JsonSerializerSettings() { Converters = new[] { new FlaxVersionConverter() } });
                project.ProjectPath = path;
                project.ProjectFolderPath = StringUtils.NormalizePath(Path.GetDirectoryName(path));
                project.AssetSystemReadOnly = project.AssetSystemVersion > CurrentAssetSystemVersion;
                if (project.AssetSystemVersion == CurrentAssetSystemVersion)
                    project.LoadV3MutableSettings();

                // Process project data
                if (string.IsNullOrEmpty(project.Name))
                    throw new Exception("Missing project name.");
                if (project.Version == null)
                    project.Version = new Version(1, 0);
                if (project.Version.Revision == 0)
                    project.Version = new Version(project.Version.Major, project.Version.Minor, project.Version.Build);
                if (project.Version.Build == 0 && project.Version.Revision == -1)
                    project.Version = new Version(project.Version.Major, project.Version.Minor);
                if (project.AssetSystemVersion >= CurrentAssetSystemVersion && !project.ValidateAssetSystemMarker(out var markerError))
                {
                    if (project.AssetSystemReadOnly)
                        Editor.LogError($"Project opened read-only. {markerError}");
                    else
                        throw new InvalidDataException(markerError);
                }
                foreach (var reference in project.References)
                {
                    string referencePath;
                    if (reference.Name.StartsWith("$(EnginePath)"))
                    {
                        // Relative to engine root
                        referencePath = Path.Combine(Globals.StartupFolder, reference.Name.Substring(14));
                    }
                    else if (reference.Name.StartsWith("$(ProjectPath)"))
                    {
                        // Relative to project root
                        referencePath = Path.Combine(project.ProjectFolderPath, reference.Name.Substring(15));
                    }
                    else if (Path.IsPathRooted(reference.Name))
                    {
                        // Relative to workspace
                        referencePath = Path.Combine(Environment.CurrentDirectory, reference.Name);
                    }
                    else
                    {
                        // Absolute
                        referencePath = reference.Name;
                    }

                    // Load referenced project
                    reference.Project = Load(referencePath);
                }

                // Project loaded
                Editor.Log($"Loaded project {project.Name}, version {project.Version}");
                _projectsCache.Add(project);
                return project;
            }
            catch
            {
                // Failed to load project
                Editor.LogError("Failed to load project \"" + path + "\".");
                throw;
            }
            finally
            {
                Profiler.EndEvent();
            }
        }

        /// <inheritdoc />
        public override string ToString()
        {
            return $"{Name} ({ProjectPath})";
        }
    }
}
