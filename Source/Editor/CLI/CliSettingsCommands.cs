// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using FlaxEditor.Content.Settings;
using FlaxEngine;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>
    /// Typed access to the project settings assets owned by Flax. Updates are partial JSON patches:
    /// fields omitted from the patch retain their current value.
    /// </summary>
    internal static class CliSettingsCommands
    {
        private sealed class SettingsGroup
        {
            public readonly string Name;
            public readonly Type Type;

            public SettingsGroup(string name, Type type)
            {
                Name = name;
                Type = type;
            }
        }

        private static readonly SettingsGroup[] Groups = CreateGroups();

        private static SettingsGroup[] CreateGroups()
        {
            var groups = new List<SettingsGroup>
            {
                new SettingsGroup("game", typeof(GameSettings)),
                new SettingsGroup("time", typeof(TimeSettings)),
                new SettingsGroup("audio", typeof(AudioSettings)),
                new SettingsGroup("layers", typeof(LayersAndTagsSettings)),
                new SettingsGroup("physics", typeof(PhysicsSettings)),
                new SettingsGroup("input", typeof(InputSettings)),
                new SettingsGroup("graphics", typeof(GraphicsSettings)),
                new SettingsGroup("network", typeof(NetworkSettings)),
                new SettingsGroup("navigation", typeof(NavigationSettings)),
                new SettingsGroup("localization", typeof(LocalizationSettings)),
                new SettingsGroup("build", typeof(BuildSettings)),
                new SettingsGroup("streaming", typeof(StreamingSettings)),
            };

            // Platform settings are optional in Flax builds. Discover concrete API classes at
            // runtime so a CLI built for one platform can still inspect projects targeting
            // another platform when that package is present, without making the core command
            // registry fail to load on minimal installations.
            AddOptionalGroup(groups, "platform.windows", "WindowsPlatformSettings");
            AddOptionalGroup(groups, "platform.uwp", "UWPPlatformSettings");
            AddOptionalGroup(groups, "platform.linux", "LinuxPlatformSettings");
            AddOptionalGroup(groups, "platform.android", "AndroidPlatformSettings");
            AddOptionalGroup(groups, "platform.web", "WebPlatformSettings");
            AddOptionalGroup(groups, "platform.mac", "MacPlatformSettings");
            AddOptionalGroup(groups, "platform.ios", "iOSPlatformSettings");
            AddOptionalGroup(groups, "platform.gdk", "GDKPlatformSettings");
            return groups.ToArray();
        }

        private static void AddOptionalGroup(List<SettingsGroup> groups, string name, string typeName)
        {
            var type = typeof(GameSettings).Assembly.GetType("FlaxEditor.Content.Settings." + typeName, false);
            if (type != null && typeof(FlaxEditor.Content.Settings.SettingsBase).IsAssignableFrom(type))
                groups.Add(new SettingsGroup(name, type));
        }

        [CliCommand("settings.list", Description = "List the stable Flax project settings groups.", Access = CliCommandAccess.ReadOnly)]
        public static object List()
        {
            return Groups.Select(group =>
            {
                JsonAsset asset = null;
                try
                {
                    asset = GameSettings.LoadAsset(group.Type);
                }
                catch
                {
                    // A platform or optional settings asset can be unavailable in a minimal project.
                }
                return new
                {
                    name = group.Name,
                    type = group.Type.FullName,
                    assetPath = asset?.Path,
                    exists = asset != null,
                };
            }).ToArray();
        }

        [CliCommand("settings.get", Description = "Read one Flax project settings group as JSON.", Access = CliCommandAccess.ReadOnly)]
        public static object Get([CliOption("group", Description = "Settings group name.", Required = true)] string group)
        {
            var descriptor = ResolveGroup(group);
            var value = Load(descriptor.Type);
            return new
            {
                group = descriptor.Name,
                type = descriptor.Type.FullName,
                assetPath = GameSettings.LoadAsset(descriptor.Type)?.Path,
                values = ToJson(value),
            };
        }

        [CliCommand("settings.schema", Description = "Describe editable top-level fields for one settings group.", Access = CliCommandAccess.ReadOnly)]
        public static object Schema([CliOption("group", Description = "Settings group name.", Required = true)] string group)
        {
            var descriptor = ResolveGroup(group);
            var value = Load(descriptor.Type);
            var members = GetMembers(descriptor.Type).Select(member =>
            {
                var memberValue = GetMemberValue(value, member);
                return new
                {
                    name = GetJsonName(member),
                    type = GetMemberType(member).FullName,
                    writable = IsWritable(member),
                    value = memberValue == null ? null : ToJsonToken(memberValue),
                };
            }).ToArray();
            return new { group = descriptor.Name, type = descriptor.Type.FullName, members };
        }

        [CliCommand("settings.diff", Description = "Preview a partial settings patch without saving it.", Access = CliCommandAccess.ReadOnly)]
        public static object Diff([CliOption("group", Description = "Settings group name.", Required = true)] string group, [CliOption("values", Description = "JSON object containing fields to compare.", Required = true)] JObject values)
        {
            if (values == null)
                throw new ArgumentNullException(nameof(values));
            var descriptor = ResolveGroup(group);
            var current = Load(descriptor.Type);
            ValidatePatch(current, values);
            var before = ToJson(current);
            var proposed = (JObject)before.DeepClone();
            proposed.Merge(values, new JsonMergeSettings { MergeArrayHandling = MergeArrayHandling.Replace });
            var changes = new List<object>();
            CollectChanges(before, proposed, string.Empty, changes);
            return new { group = descriptor.Name, changed = changes.Count != 0, changes = changes.ToArray(), before, after = proposed };
        }

        [CliCommand("settings.set", Description = "Apply and persist a partial JSON patch to one Flax settings group.", Access = CliCommandAccess.MutatesProject)]
        public static object Set([CliOption("group", Description = "Settings group name.", Required = true)] string group, [CliOption("values", Description = "JSON object containing fields to update.", Required = true)] JObject values, [CliOption("dry-run", Description = "Validate and preview the update without saving.")] bool dryRun = false)
        {
            if (values == null)
                throw new ArgumentNullException(nameof(values));
            var descriptor = ResolveGroup(group);
            var current = Load(descriptor.Type);
            ValidatePatch(current, values);
            var before = ToJson(current);
            var after = (JObject)before.DeepClone();
            after.Merge(values, new JsonMergeSettings { MergeArrayHandling = MergeArrayHandling.Replace });
            var changes = new List<object>();
            CollectChanges(before, after, string.Empty, changes);
            if (dryRun || changes.Count == 0)
                return new { group = descriptor.Name, dryRun = true, changed = changes.Count != 0, saved = false, changes = changes.ToArray(), before, after };

            var serializer = CreateSerializer();
            using (var reader = values.CreateReader())
                serializer.Populate(reader, current);
            if (Save(descriptor.Type, current))
            {
                // Restore the in-memory object if persistence fails, so a failed command is atomic.
                using var restoreReader = before.CreateReader();
                serializer.Populate(restoreReader, current);
                throw new InvalidOperationException($"Failed to save settings group '{descriptor.Name}'. See the Editor log for details.");
            }
            return new { group = descriptor.Name, dryRun = false, changed = true, saved = true, assetPath = GameSettings.LoadAsset(descriptor.Type)?.Path, changes = changes.ToArray(), before, after };
        }

        private static SettingsGroup ResolveGroup(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
                throw new ArgumentException("A settings group is required.", nameof(value));
            var result = Groups.FirstOrDefault(x => string.Equals(x.Name, value, StringComparison.OrdinalIgnoreCase));
            return result ?? throw new KeyNotFoundException($"Unknown settings group '{value}'. Use settings.list to discover supported groups.");
        }

        private static FlaxEditor.Content.Settings.SettingsBase Load(Type type)
        {
            var method = typeof(GameSettings).GetMethods(BindingFlags.Public | BindingFlags.Static)
                .Single(x => x.Name == "Load" && x.IsGenericMethodDefinition && x.GetParameters().Length == 0);
            var value = method.MakeGenericMethod(type).Invoke(null, null) as FlaxEditor.Content.Settings.SettingsBase;
            return value ?? throw new InvalidOperationException($"Failed to load settings group '{type.FullName}'.");
        }

        private static bool Save(Type type, FlaxEditor.Content.Settings.SettingsBase value)
        {
            var method = typeof(GameSettings).GetMethods(BindingFlags.Public | BindingFlags.Static)
                .Single(x => x.Name == "Save" && x.IsGenericMethodDefinition && x.GetParameters().Length == 1);
            return (bool)method.MakeGenericMethod(type).Invoke(null, new object[] { value });
        }

        private static JObject ToJson(object value)
        {
            return JObject.FromObject(value, CreateSerializer());
        }

        private static JToken ToJsonToken(object value)
        {
            return value == null ? JValue.CreateNull() : JToken.FromObject(value, CreateSerializer());
        }

        private static JsonSerializer CreateSerializer()
        {
            return JsonSerializer.Create(FlaxJsonSerializer.Settings);
        }

        private static IEnumerable<MemberInfo> GetMembers(Type type)
        {
            return type.GetMembers(BindingFlags.Instance | BindingFlags.Public)
                .Where(x => (x is FieldInfo || x is PropertyInfo) && x.GetCustomAttribute<JsonIgnoreAttribute>() == null)
                .Where(x => x is FieldInfo || ((PropertyInfo)x).GetIndexParameters().Length == 0)
                .Where(IsReadable)
                .OrderBy(x => x.MetadataToken);
        }

        private static bool IsReadable(MemberInfo member)
        {
            return member is FieldInfo || ((PropertyInfo)member).CanRead;
        }

        private static bool IsWritable(MemberInfo member)
        {
            return member is FieldInfo field ? !field.IsInitOnly : ((PropertyInfo)member).CanWrite;
        }

        private static Type GetMemberType(MemberInfo member)
        {
            return member is FieldInfo field ? field.FieldType : ((PropertyInfo)member).PropertyType;
        }

        private static object GetMemberValue(object target, MemberInfo member)
        {
            return member is FieldInfo field ? field.GetValue(target) : ((PropertyInfo)member).GetValue(target);
        }

        private static string GetJsonName(MemberInfo member)
        {
            return member.GetCustomAttribute<JsonPropertyAttribute>()?.PropertyName ?? member.Name;
        }

        private static void ValidatePatch(FlaxEditor.Content.Settings.SettingsBase target, JObject patch)
        {
            var members = GetMembers(target.GetType()).ToDictionary(GetJsonName, StringComparer.OrdinalIgnoreCase);
            var serializer = CreateSerializer();
            foreach (var property in patch.Properties())
            {
                if (!members.TryGetValue(property.Name, out var member))
                    throw new ArgumentException($"Settings group '{target.GetType().Name}' has no public field or property '{property.Name}'.");
                if (!IsWritable(member))
                    throw new ArgumentException($"Settings member '{property.Name}' is read-only.");
                try
                {
                    property.Value.ToObject(GetMemberType(member), serializer);
                }
                catch (Exception ex)
                {
                    throw new ArgumentException($"Settings member '{property.Name}' cannot be converted to '{GetMemberType(member).FullName}': {ex.Message}");
                }
            }
        }

        private static void CollectChanges(JToken before, JToken after, string path, List<object> changes)
        {
            if (JToken.DeepEquals(before, after))
                return;
            if (before is JObject leftObject && after is JObject rightObject)
            {
                var names = leftObject.Properties().Select(x => x.Name).Concat(rightObject.Properties().Select(x => x.Name)).Distinct(StringComparer.OrdinalIgnoreCase);
                foreach (var name in names)
                    CollectChanges(leftObject[name], rightObject[name], string.IsNullOrEmpty(path) ? name : path + "." + name, changes);
                return;
            }
            changes.Add(new { path, before, after });
        }
    }
}
