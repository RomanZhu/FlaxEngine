// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using FlaxEngine;
using FlaxEngine.Utilities;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;

namespace FlaxEditor
{
    /// <summary>
    /// Describes the access level of a typed Flax CLI command.
    /// </summary>
    public enum CliCommandAccess
    {
        /// <summary>
        /// The command only observes project or runtime state.
        /// </summary>
        ReadOnly,

        /// <summary>
        /// The command can change project state.
        /// </summary>
        MutatesProject,

        /// <summary>
        /// The command can perform destructive or difficult-to-recover changes.
        /// </summary>
        Destructive,
    }

    /// <summary>
    /// Controls how a project-owned CLI generator persists successful changes.
    /// </summary>
    public enum CliGeneratorSaveMode
    {
        /// <summary>
        /// The generator owns all persistence.
        /// </summary>
        None,

        /// <summary>
        /// Save all loaded scenes after the generator succeeds.
        /// </summary>
        Scenes,

        /// <summary>
        /// Save all scenes and content after the generator succeeds.
        /// </summary>
        All,
    }

    /// <summary>
    /// Registers a public static method as a typed Flax CLI command.
    /// </summary>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
    public class CliCommandAttribute : Attribute
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="CliCommandAttribute"/> class.
        /// </summary>
        /// <param name="name">The unique lowercase dotted command name.</param>
        public CliCommandAttribute(string name)
        {
            Name = name;
        }

        /// <summary>
        /// Gets the unique command name.
        /// </summary>
        public string Name { get; }

        /// <summary>
        /// Gets or sets the command description.
        /// </summary>
        public string Description { get; set; }

        /// <summary>
        /// Gets or sets the command version.
        /// </summary>
        public string Version { get; set; } = "1.0";

        /// <summary>
        /// Gets or sets the command access level.
        /// </summary>
        public CliCommandAccess Access { get; set; } = CliCommandAccess.ReadOnly;

        /// <summary>
        /// Gets or sets whether the command must execute on the main thread.
        /// </summary>
        public bool RequiresMainThread { get; set; } = true;

        /// <summary>
        /// Gets or sets whether the command requires at least one loaded scene.
        /// </summary>
        public bool RequiresScene { get; set; }

        /// <summary>
        /// Gets or sets whether the command requires play mode.
        /// </summary>
        public bool RequiresPlayMode { get; set; }
    }

    /// <summary>
    /// Registers a public static method as a discoverable project-owned generator.
    /// </summary>
    /// <remarks>
    /// Generators contain project-specific authoring logic. Use <see cref="CliOptionAttribute"/>
    /// parameters for their typed inputs. When <see cref="SupportsDryRun"/> is enabled the method
    /// must expose an optional boolean <c>dry-run</c> option and must avoid mutations when it is true.
    /// </remarks>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
    public sealed class CliGeneratorAttribute : CliCommandAttribute
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="CliGeneratorAttribute"/> class.
        /// </summary>
        /// <param name="name">The unique lowercase dotted generator name.</param>
        public CliGeneratorAttribute(string name)
        : base(name)
        {
            Access = CliCommandAccess.MutatesProject;
        }

        /// <summary>
        /// Gets or sets whether the generator supports the conventional optional boolean <c>dry-run</c> option.
        /// </summary>
        public bool SupportsDryRun { get; set; }

        /// <summary>
        /// Gets or sets how successful non-dry-run changes are persisted.
        /// </summary>
        public CliGeneratorSaveMode SaveMode { get; set; } = CliGeneratorSaveMode.All;
    }

    /// <summary>
    /// Describes a parameter exposed by a typed Flax CLI command.
    /// </summary>
    [AttributeUsage(AttributeTargets.Parameter, AllowMultiple = false, Inherited = false)]
    public sealed class CliOptionAttribute : Attribute
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="CliOptionAttribute"/> class.
        /// </summary>
        /// <param name="name">The public option name.</param>
        public CliOptionAttribute(string name)
        {
            Name = name;
        }

        /// <summary>
        /// Gets the public option name.
        /// </summary>
        public string Name { get; }

        /// <summary>
        /// Gets or sets the option description.
        /// </summary>
        public string Description { get; set; }

        /// <summary>
        /// Gets or sets whether the option is required. If omitted, the method parameter default determines this value.
        /// </summary>
        public bool Required { get; set; }
    }

    /// <summary>
    /// A structured typed-command diagnostic.
    /// </summary>
    public sealed class CliCommandMessage
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="CliCommandMessage"/> class.
        /// </summary>
        public CliCommandMessage(string code, string message, object details = null)
        {
            Code = code;
            Message = message;
            Details = details;
        }

        /// <summary>
        /// Gets the stable diagnostic code.
        /// </summary>
        [JsonProperty("code")]
        public string Code { get; }

        /// <summary>
        /// Gets the diagnostic message.
        /// </summary>
        [JsonProperty("message")]
        public string Message { get; }

        /// <summary>
        /// Gets optional structured diagnostic details.
        /// </summary>
        [JsonProperty("details")]
        public object Details { get; }
    }

    /// <summary>
    /// Result returned by a typed Flax CLI command.
    /// </summary>
    public sealed class CliCommandResult
    {
        private CliCommandResult(bool succeeded, object data, CliCommandMessage[] errors, CliCommandMessage[] warnings)
        {
            Succeeded = succeeded;
            Data = data;
            Errors = errors ?? Array.Empty<CliCommandMessage>();
            Warnings = warnings ?? Array.Empty<CliCommandMessage>();
        }

        /// <summary>
        /// Gets whether the command succeeded.
        /// </summary>
        public bool Succeeded { get; }

        /// <summary>
        /// Gets the structured command result data.
        /// </summary>
        public object Data { get; }

        /// <summary>
        /// Gets command errors.
        /// </summary>
        public IReadOnlyList<CliCommandMessage> Errors { get; }

        /// <summary>
        /// Gets command warnings.
        /// </summary>
        public IReadOnlyList<CliCommandMessage> Warnings { get; }

        /// <summary>
        /// Creates a successful command result.
        /// </summary>
        public static CliCommandResult Success(object data = null, params CliCommandMessage[] warnings)
        {
            return new CliCommandResult(true, data, null, warnings);
        }

        /// <summary>
        /// Creates a failed command result.
        /// </summary>
        public static CliCommandResult Failure(string code, string message, object details = null)
        {
            return new CliCommandResult(false, null, new[] { new CliCommandMessage(code, message, details) }, null);
        }
    }

    /// <summary>
    /// A cooperative CLI command that performs its work in short slices on the Editor thread.
    /// </summary>
    /// <remarks>
    /// Long-running commands should return an instance of this type instead of doing all work
    /// inside the command method. The live CLI bridge calls <see cref="Update"/> once per frame
    /// with a small time budget. Implementations must return promptly when that budget expires.
    /// </remarks>
    public abstract class CliCommandOperation
    {
        /// <summary>
        /// Gets whether the operation has completed.
        /// </summary>
        public abstract bool IsCompleted { get; }

        /// <summary>
        /// Gets the completed result. This property is read only after <see cref="IsCompleted"/> becomes true.
        /// </summary>
        public abstract CliCommandResult Result { get; }

        /// <summary>
        /// Advances the operation on the Editor thread.
        /// </summary>
        /// <param name="timeBudget">Maximum amount of time the operation should spend in this update.</param>
        public abstract void Update(TimeSpan timeBudget);

        /// <summary>
        /// Cancels the operation and releases any partially acquired state.
        /// </summary>
        public virtual void Cancel()
        {
        }
    }

    /// <summary>
    /// Context supplied to a typed Flax CLI command.
    /// </summary>
    public sealed class CliCommandContext
    {
        private readonly Action<string, float> _reportProgress;
        private readonly Action<CliCommandMessage> _reportWarning;

        internal CliCommandContext(string requestId, string projectPath, CancellationToken cancellationToken, Action<string, float> reportProgress, Action<CliCommandMessage> reportWarning)
        {
            RequestId = requestId;
            ProjectPath = projectPath;
            CancellationToken = cancellationToken;
            _reportProgress = reportProgress;
            _reportWarning = reportWarning;
        }

        /// <summary>
        /// Gets the unique request ID.
        /// </summary>
        public string RequestId { get; }

        /// <summary>
        /// Gets the canonical project path.
        /// </summary>
        public string ProjectPath { get; }

        /// <summary>
        /// Gets the request cancellation token. One-shot commands currently receive a non-cancellable token; live bridge requests receive a session token.
        /// </summary>
        public CancellationToken CancellationToken { get; }

        /// <summary>
        /// Reports command progress in the range 0 through 1.
        /// </summary>
        public void ReportProgress(string message, float progress)
        {
            if (float.IsNaN(progress) || float.IsInfinity(progress))
                throw new ArgumentOutOfRangeException(nameof(progress));
            _reportProgress?.Invoke(message, Math.Max(0.0f, Math.Min(1.0f, progress)));
        }

        /// <summary>
        /// Reports a structured warning without failing the command.
        /// </summary>
        public void ReportWarning(string code, string message, object details = null)
        {
            _reportWarning?.Invoke(new CliCommandMessage(code, message, details));
        }
    }

    internal static class CliBuiltInCommands
    {
        /// <summary>
        /// Verifies typed command discovery and one-shot Editor execution.
        /// </summary>
        [CliCommand("cli.ping", Description = "Verify typed command discovery and one-shot Editor execution.", Access = CliCommandAccess.ReadOnly)]
        public static object Ping(CliCommandContext context)
        {
            return new
            {
                protocolVersion = 1,
                projectPath = context.ProjectPath,
                playMode = Editor.IsPlayMode,
                loadedScenes = Level.ScenesCount,
            };
        }
    }

    internal sealed class CliCommandOptions
    {
        [JsonProperty("action")]
        public string Action { get; set; }

        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("arguments")]
        public JObject Arguments { get; set; }

        [JsonProperty("confirm")]
        public bool Confirm { get; set; }
    }

    internal sealed class CliRegisteredCommand
    {
        public CliCommandAttribute Attribute;
        public MethodInfo Method;
        public CliRegisteredParameter[] Parameters;
    }

    internal sealed class CliRegisteredParameter
    {
        public ParameterInfo Parameter;
        public CliOptionAttribute Attribute;
        public string Name;
        public bool Required;
        public bool IsContext;
    }

    internal sealed class CliCommandInvocation
    {
        private readonly CliCommandOperation _operation;
        private readonly CliCommandResult _result;
        private readonly Action<CliCommandResult> _onCompleted;
        private bool _completionApplied;

        public CliCommandInvocation(CliCommandResult result, Action<CliCommandResult> onCompleted = null)
        {
            _result = result;
            _onCompleted = onCompleted;
        }

        public CliCommandInvocation(CliCommandOperation operation, Action<CliCommandResult> onCompleted = null)
        {
            _operation = operation ?? throw new ArgumentNullException(nameof(operation));
            _onCompleted = onCompleted;
        }

        public bool IsCompleted => _operation == null || _operation.IsCompleted;

        public CliCommandResult Result
        {
            get
            {
                var result = _operation == null ? _result : _operation.Result;
                if (IsCompleted && !_completionApplied)
                {
                    _completionApplied = true;
                    _onCompleted?.Invoke(result);
                }
                return result;
            }
        }

        public void Update(TimeSpan timeBudget)
        {
            _operation?.Update(timeBudget);
        }

        public void Cancel()
        {
            _operation?.Cancel();
        }
    }

    internal sealed class CliCommandProtocolException : Exception
    {
        public readonly string Code;

        public CliCommandProtocolException(string code, string message)
        : base(message)
        {
            Code = code;
        }
    }

    internal static class CliCommandRegistry
    {
        private static readonly object CacheLocker = new object();
        private static Assembly[] _cachedAssemblies;
        private static CliRegisteredCommand[] _cachedCommands;

        public static CliRegisteredCommand[] Discover()
        {
            var result = new Dictionary<string, CliRegisteredCommand>(StringComparer.OrdinalIgnoreCase);
            var assemblies = Utils.GetAssemblies().Where(x => !x.IsDynamic).OrderBy(x => x.GetName().Name, StringComparer.OrdinalIgnoreCase).ToArray();
            lock (CacheLocker)
            {
                if (_cachedAssemblies != null && _cachedAssemblies.SequenceEqual(assemblies))
                    return _cachedCommands;
            }
            foreach (var assembly in assemblies)
            {
                foreach (var type in GetLoadableTypes(assembly).OrderBy(x => x.FullName, StringComparer.Ordinal))
                {
                    var methods = type.GetMethods(BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly);
                    foreach (var method in methods)
                    {
                        var attribute = method.GetCustomAttribute<CliCommandAttribute>(false);
                        if (attribute == null)
                            continue;
                        var command = Validate(method, attribute);
                        CliRegisteredCommand duplicate;
                        if (result.TryGetValue(attribute.Name, out duplicate))
                            throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' is registered by both '{duplicate.Method.DeclaringType?.FullName}.{duplicate.Method.Name}' and '{method.DeclaringType?.FullName}.{method.Name}'.");
                        result.Add(attribute.Name, command);
                    }
                }
            }
            var commands = result.Values.OrderBy(x => x.Attribute.Name, StringComparer.OrdinalIgnoreCase).ToArray();
            lock (CacheLocker)
            {
                _cachedAssemblies = assemblies;
                _cachedCommands = commands;
            }
            return commands;
        }

        public static CliRegisteredCommand[] DiscoverCommands()
        {
            return Discover().Where(x => !(x.Attribute is CliGeneratorAttribute)).ToArray();
        }

        public static CliRegisteredCommand[] DiscoverGenerators()
        {
            return Discover().Where(x => x.Attribute is CliGeneratorAttribute).ToArray();
        }

        public static object Describe(CliRegisteredCommand command)
        {
            return new
            {
                name = command.Attribute.Name,
                kind = command.Attribute is CliGeneratorAttribute ? "generator" : "command",
                description = command.Attribute.Description,
                version = command.Attribute.Version,
                access = GetAccessName(command.Attribute.Access),
                requiresMainThread = command.Attribute.RequiresMainThread,
                requiresScene = command.Attribute.RequiresScene,
                requiresPlayMode = command.Attribute.RequiresPlayMode,
                owner = new
                {
                    assembly = command.Method.DeclaringType?.Assembly.GetName().Name,
                    type = command.Method.DeclaringType?.FullName,
                    method = command.Method.Name,
                },
                parameters = command.Parameters.Where(x => !x.IsContext).Select(DescribeParameter).ToArray(),
                returns = DescribeType(command.Method.ReturnType),
                generator = command.Attribute is CliGeneratorAttribute generator
                    ? new
                    {
                        supportsDryRun = generator.SupportsDryRun,
                        saveMode = GetSaveModeName(generator.SaveMode),
                    }
                    : null,
            };
        }

        public static CliRegisteredCommand Require(CliRegisteredCommand[] commands, string name)
        {
            if (string.IsNullOrWhiteSpace(name))
                throw new CliCommandProtocolException("FLX-COMMAND-NAME-0002", "A command name is required.");
            var command = commands.FirstOrDefault(x => string.Equals(x.Attribute.Name, name, StringComparison.OrdinalIgnoreCase));
            if (command == null)
                throw new CliCommandProtocolException("FLX-COMMAND-NOTFOUND-0004", $"CLI command '{name}' is not registered in the project.");
            return command;
        }

        public static CliRegisteredCommand RequireCommand(CliRegisteredCommand[] commands, string name)
        {
            var command = Require(commands, name);
            if (command.Attribute is CliGeneratorAttribute)
                throw new CliCommandProtocolException("FLX-COMMAND-NOTFOUND-0004", $"'{name}' is a generator. Invoke it through the generators surface.");
            return command;
        }

        public static CliRegisteredCommand RequireGenerator(CliRegisteredCommand[] commands, string name)
        {
            var command = Require(commands, name);
            if (!(command.Attribute is CliGeneratorAttribute))
                throw new CliCommandProtocolException("FLX-GENERATOR-NOTFOUND-0004", $"'{name}' is a command, not a project generator.");
            return command;
        }

        public static CliCommandResult Invoke(CliRegisteredCommand command, JObject arguments, bool confirm, CliCommandContext context)
        {
            var invocation = BeginInvoke(command, arguments, confirm, context);
            while (!invocation.IsCompleted)
            {
                context.CancellationToken.ThrowIfCancellationRequested();
                invocation.Update(TimeSpan.FromMilliseconds(10.0));
            }
            return invocation.Result;
        }

        public static CliCommandInvocation BeginInvoke(CliRegisteredCommand command, JObject arguments, bool confirm, CliCommandContext context)
        {
            if (command.Attribute.Access == CliCommandAccess.Destructive && !confirm)
                throw new CliCommandProtocolException("FLX-COMMAND-CONFIRM-0004", $"Command '{command.Attribute.Name}' is destructive and requires --yes.");
            if (command.Attribute.RequiresMainThread && !Platform.IsInMainThread)
                throw new CliCommandProtocolException("FLX-COMMAND-THREAD-0006", $"Command '{command.Attribute.Name}' requires the main thread.");
            if (command.Attribute.RequiresScene && !Level.IsAnySceneLoaded)
                throw new CliCommandProtocolException("FLX-COMMAND-SCENE-0004", $"Command '{command.Attribute.Name}' requires a loaded scene.");
            if (command.Attribute.RequiresPlayMode && !Editor.IsPlayMode)
                throw new CliCommandProtocolException("FLX-COMMAND-PLAY-0004", $"Command '{command.Attribute.Name}' requires play mode.");

            arguments = arguments ?? new JObject();
            var values = new object[command.Parameters.Length];
            var consumed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < command.Parameters.Length; i++)
            {
                var parameter = command.Parameters[i];
                if (parameter.IsContext)
                {
                    values[i] = context;
                    continue;
                }

                var property = FindArgument(arguments, parameter.Name);
                if (property != null)
                {
                    consumed.Add(property.Name);
                    var parameterType = parameter.Parameter.ParameterType;
                    var isBooleanParameter = parameterType == typeof(bool) || Nullable.GetUnderlyingType(parameterType) == typeof(bool);
                    if (property.Value.Type == JTokenType.Boolean && property.Value.Value<bool>() && !isBooleanParameter)
                    {
                        throw new CliCommandProtocolException(
                            "FLX-COMMAND-ARGUMENT-0002",
                            $"Argument '{property.Name}' requires a value. Valueless --{property.Name} syntax is only valid for Boolean parameters.");
                    }
                    try
                    {
                        // Terminal option parsing represents a single occurrence as a
                        // scalar and repeated occurrences as an array. Normalize the
                        // single-value form for array parameters so `--actor <id>` and
                        // `--actor <id> --actor <id>` have the same typed contract.
                        var argumentValue = parameterType.IsArray && property.Value.Type != JTokenType.Array
                            ? new JArray(property.Value.DeepClone())
                            : property.Value;
                        values[i] = JsonConvert.DeserializeObject(argumentValue.ToString(Formatting.None), parameterType, FlaxJsonSerializer.Settings);
                    }
                    catch (Exception ex)
                    {
                        throw new CliCommandProtocolException("FLX-COMMAND-ARGUMENT-0002", $"Argument '{property.Name}' cannot be converted to '{parameterType.FullName}': {ex.Message}");
                    }
                }
                else if (parameter.Parameter.HasDefaultValue)
                {
                    values[i] = parameter.Parameter.DefaultValue;
                }
                else if (!parameter.Required)
                {
                    values[i] = parameter.Parameter.ParameterType.IsValueType ? Activator.CreateInstance(parameter.Parameter.ParameterType) : null;
                }
                else
                {
                    throw new CliCommandProtocolException("FLX-COMMAND-ARGUMENT-0002", $"Required argument '{parameter.Name}' is missing.");
                }
            }

            var unknown = arguments.Properties().FirstOrDefault(x => !consumed.Contains(x.Name));
            if (unknown != null)
                throw new CliCommandProtocolException("FLX-COMMAND-ARGUMENT-0002", $"Command '{command.Attribute.Name}' does not define argument '{unknown.Name}'.");

            object value;
            try
            {
                value = command.Method.Invoke(null, values);
            }
            catch (TargetInvocationException ex) when (ex.InnerException != null)
            {
                throw new CliCommandProtocolException("FLX-COMMAND-0006", ex.InnerException.Message);
            }
            catch (Exception ex)
            {
                throw new CliCommandProtocolException("FLX-COMMAND-0006", ex.Message);
            }

            var completion = CreateCompletion(command, values);
            if (value is CliCommandOperation operation)
                return new CliCommandInvocation(operation, completion);
            return new CliCommandInvocation(value as CliCommandResult ?? CliCommandResult.Success(value), completion);
        }

        private static CliRegisteredCommand Validate(MethodInfo method, CliCommandAttribute attribute)
        {
            if (string.IsNullOrWhiteSpace(attribute.Name) || !IsValidCommandName(attribute.Name))
                throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"Method '{method.DeclaringType?.FullName}.{method.Name}' has invalid CLI command name '{attribute.Name}'. Names must be lowercase dotted identifiers.");
            if (method.ContainsGenericParameters)
                throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' cannot be generic.");
            if (typeof(Task).IsAssignableFrom(method.ReturnType))
                throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' cannot return Task because continuations may leave the Editor thread. Return CliCommandOperation for cooperative work.");
            if (method.ReturnType.IsByRef || method.ReturnType.IsPointer)
                throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' has an unsupported return type.");

            var parameters = method.GetParameters();
            var registered = new CliRegisteredParameter[parameters.Length];
            var optionNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var contextCount = 0;
            for (int i = 0; i < parameters.Length; i++)
            {
                var parameter = parameters[i];
                if (parameter.ParameterType == typeof(CliCommandContext))
                {
                    contextCount++;
                    registered[i] = new CliRegisteredParameter { Parameter = parameter, Name = parameter.Name, IsContext = true };
                    continue;
                }
                if (parameter.IsOut || parameter.ParameterType.IsByRef || parameter.ParameterType.IsPointer || parameter.ParameterType.ContainsGenericParameters)
                    throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' parameter '{parameter.Name}' has an unsupported type.");

                var option = parameter.GetCustomAttribute<CliOptionAttribute>(false);
                var name = option?.Name ?? parameter.Name;
                if (string.IsNullOrWhiteSpace(name))
                    throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' has an unnamed parameter.");
                var normalizedName = NormalizeName(name);
                if (!optionNames.Add(normalizedName))
                    throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' defines duplicate option '{name}'.");
                var required = HasRequiredOverride(parameter, out var requiredOverride) ? requiredOverride : !parameter.HasDefaultValue;
                registered[i] = new CliRegisteredParameter { Parameter = parameter, Attribute = option, Name = name, Required = required };
            }
            if (contextCount > 1)
                throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI command '{attribute.Name}' accepts more than one CliCommandContext parameter.");

            if (attribute is CliGeneratorAttribute generator)
            {
                if (generator.Access == CliCommandAccess.ReadOnly)
                    throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI generator '{attribute.Name}' must use mutates-project or destructive access.");
                if (generator.SupportsDryRun)
                {
                    var dryRun = registered.FirstOrDefault(x => !x.IsContext && NormalizeName(x.Name) == "dryrun");
                    if (dryRun == null || dryRun.Parameter.ParameterType != typeof(bool) || !dryRun.Parameter.HasDefaultValue || !Equals(dryRun.Parameter.DefaultValue, false))
                        throw new CliCommandProtocolException("FLX-COMMAND-CATALOG-0006", $"CLI generator '{attribute.Name}' declares dry-run support but does not expose an optional boolean 'dry-run' parameter defaulting to false.");
                }
            }

            return new CliRegisteredCommand { Attribute = attribute, Method = method, Parameters = registered };
        }

        private static Action<CliCommandResult> CreateCompletion(CliRegisteredCommand command, object[] values)
        {
            if (!(command.Attribute is CliGeneratorAttribute generator))
                return null;

            var dryRun = false;
            if (generator.SupportsDryRun)
            {
                for (var i = 0; i < command.Parameters.Length; i++)
                {
                    if (!command.Parameters[i].IsContext && NormalizeName(command.Parameters[i].Name) == "dryrun")
                    {
                        dryRun = (bool)values[i];
                        break;
                    }
                }
            }
            return result =>
            {
                if (result == null || !result.Succeeded || dryRun)
                    return;
                switch (generator.SaveMode)
                {
                case CliGeneratorSaveMode.None:
                    break;
                case CliGeneratorSaveMode.Scenes:
                    foreach (var scene in Level.Scenes)
                    {
                        if (Editor.Instance.Scene.IsEdited(scene) && !Editor.Instance.Scene.SaveSceneSynchronously(scene))
                            throw new InvalidOperationException($"Generator '{generator.Name}' succeeded but scene '{scene.Name}' failed to save.");
                    }
                    break;
                case CliGeneratorSaveMode.All:
                    Editor.Instance.SaveAll();
                    break;
                default:
                    throw new ArgumentOutOfRangeException();
                }
            };
        }

        private static object DescribeParameter(CliRegisteredParameter parameter)
        {
            return new
            {
                name = parameter.Name,
                description = parameter.Attribute?.Description,
                required = parameter.Required,
                defaultValue = parameter.Parameter.HasDefaultValue && parameter.Parameter.DefaultValue != DBNull.Value ? parameter.Parameter.DefaultValue : null,
                schema = DescribeType(parameter.Parameter.ParameterType),
            };
        }

        private static object DescribeType(Type type)
        {
            var nullableType = Nullable.GetUnderlyingType(type);
            if (nullableType != null)
                return new { type = GetJsonType(nullableType), nullable = true, dotnetType = type.FullName, values = nullableType.IsEnum ? Enum.GetNames(nullableType) : null, items = (object)null };
            if (type.IsArray)
                return new { type = "array", nullable = false, dotnetType = type.FullName, values = (string[])null, items = DescribeType(type.GetElementType()) };
            return new { type = GetJsonType(type), nullable = !type.IsValueType, dotnetType = type.FullName, values = type.IsEnum ? Enum.GetNames(type) : null, items = (object)null };
        }

        private static string GetJsonType(Type type)
        {
            if (type == typeof(void))
                return "null";
            if (type == typeof(bool))
                return "boolean";
            if (type == typeof(byte) || type == typeof(sbyte) || type == typeof(short) || type == typeof(ushort) || type == typeof(int) || type == typeof(uint) || type == typeof(long) || type == typeof(ulong))
                return "integer";
            if (type == typeof(float) || type == typeof(double) || type == typeof(decimal))
                return "number";
            if (type == typeof(string) || type == typeof(char) || type == typeof(Guid) || type == typeof(DateTime) || type.IsEnum)
                return "string";
            if (type.IsArray)
                return "array";
            return "object";
        }

        private static JProperty FindArgument(JObject arguments, string name)
        {
            var normalized = NormalizeName(name);
            return arguments.Properties().FirstOrDefault(x => NormalizeName(x.Name) == normalized);
        }

        private static string NormalizeName(string value)
        {
            return new string(value.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
        }

        private static bool HasRequiredOverride(ParameterInfo parameter, out bool value)
        {
            var attribute = parameter.CustomAttributes.FirstOrDefault(x => x.AttributeType == typeof(CliOptionAttribute));
            if (attribute != null)
            {
                foreach (var argument in attribute.NamedArguments)
                {
                    if (argument.MemberName == nameof(CliOptionAttribute.Required))
                    {
                        value = (bool)argument.TypedValue.Value;
                        return true;
                    }
                }
            }
            value = false;
            return false;
        }

        private static bool IsValidCommandName(string name)
        {
            if (!string.Equals(name, name.ToLowerInvariant(), StringComparison.Ordinal) || name.StartsWith(".", StringComparison.Ordinal) || name.EndsWith(".", StringComparison.Ordinal) || name.Contains(".."))
                return false;
            return name.All(x => char.IsLetterOrDigit(x) || x == '.' || x == '-' || x == '_');
        }

        private static string GetAccessName(CliCommandAccess access)
        {
            switch (access)
            {
            case CliCommandAccess.ReadOnly:
                return "readOnly";
            case CliCommandAccess.MutatesProject:
                return "mutatesProject";
            case CliCommandAccess.Destructive:
                return "destructive";
            default:
                throw new ArgumentOutOfRangeException(nameof(access));
            }
        }

        private static string GetSaveModeName(CliGeneratorSaveMode saveMode)
        {
            switch (saveMode)
            {
            case CliGeneratorSaveMode.None:
                return "none";
            case CliGeneratorSaveMode.Scenes:
                return "scenes";
            case CliGeneratorSaveMode.All:
                return "all";
            default:
                throw new ArgumentOutOfRangeException(nameof(saveMode));
            }
        }

        private static Type[] GetLoadableTypes(Assembly assembly)
        {
            try
            {
                return assembly.GetTypes();
            }
            catch (ReflectionTypeLoadException ex)
            {
                return ex.Types.Where(x => x != null).ToArray();
            }
        }
    }

    internal sealed partial class CliRequestService
    {
        private static readonly TimeSpan CommandStartupTimeout = TimeSpan.FromMinutes(2);

        private void ExecuteCommand()
        {
            var options = _request.Command ?? throw new InvalidOperationException("The command request payload is missing.");
            if (string.IsNullOrWhiteSpace(options.Action))
                throw new InvalidOperationException("The command action is missing.");

            TryWriteEvent(new { type = "started", requestId = _request.RequestId, operation = _request.Operation, action = options.Action, name = options.Name });
            if (!Editor.Instance.StateMachine.CurrentState.IsEditorReady)
            {
                DeferCommandUntilScriptsLoad(options);
                return;
            }
            ExecuteCommand(options);
        }

        private void DeferCommandUntilScriptsLoad(CliCommandOptions options)
        {
            var deadline = DateTime.UtcNow + CommandStartupTimeout;
            TryWriteEvent(new { type = "phase", requestId = _request.RequestId, name = "WaitingForProjectScripts" });
            Action update = null;
            update = () =>
            {
                var ready = EvaluateCommandStartup(Editor.Instance.StateMachine.CurrentState.IsEditorReady, DateTime.UtcNow, deadline, out var timedOut);
                if (ready)
                {
                    Editor.Instance.EditorUpdate -= update;
                    ExecuteCommand(options);
                }
                else if (timedOut)
                {
                    Editor.Instance.EditorUpdate -= update;
                    CompleteCommand(null, false,
                        new[] { new CliCommandMessage("FLX-COMMAND-STARTUP-0006", "Timed out waiting for project scripts to load.") },
                        Array.Empty<CliCommandMessage>());
                }
            };
            Editor.Instance.EditorUpdate += update;
        }

        internal static bool EvaluateCommandStartup(bool scriptsLoaded, DateTime utcNow, DateTime deadline, out bool timedOut)
        {
            timedOut = !scriptsLoaded && utcNow >= deadline;
            return scriptsLoaded;
        }

        private void ExecuteCommand(CliCommandOptions options)
        {
            try
            {
                var commands = CliCommandRegistry.Discover();
                switch (options.Action)
                {
                case "list":
                    CompleteCommand(commands.Where(x => !(x.Attribute is CliGeneratorAttribute)).Select(CliCommandRegistry.Describe).ToArray(), true, Array.Empty<CliCommandMessage>(), Array.Empty<CliCommandMessage>());
                    break;
                case "info":
                    CompleteCommand(CliCommandRegistry.Describe(CliCommandRegistry.RequireCommand(commands, options.Name)), true, Array.Empty<CliCommandMessage>(), Array.Empty<CliCommandMessage>());
                    break;
                case "invoke":
                    InvokeCommand(commands, options, false);
                    break;
                case "generator-list":
                    CompleteCommand(commands.Where(x => x.Attribute is CliGeneratorAttribute).Select(CliCommandRegistry.Describe).ToArray(), true, Array.Empty<CliCommandMessage>(), Array.Empty<CliCommandMessage>());
                    break;
                case "generator-info":
                    CompleteCommand(CliCommandRegistry.Describe(CliCommandRegistry.RequireGenerator(commands, options.Name)), true, Array.Empty<CliCommandMessage>(), Array.Empty<CliCommandMessage>());
                    break;
                case "generator-invoke":
                    InvokeCommand(commands, options, true);
                    break;
                default:
                    throw new CliCommandProtocolException("FLX-COMMAND-ACTION-0002", $"Unsupported command action '{options.Action}'.");
                }
            }
            catch (CliCommandProtocolException ex)
            {
                Editor.LogError(ex.ToString());
                CompleteCommand(null, false, new[] { new CliCommandMessage(ex.Code, ex.Message) }, Array.Empty<CliCommandMessage>());
            }
            catch (Exception ex)
            {
                Editor.LogError(ex.ToString());
                CompleteCommand(null, false, new[] { new CliCommandMessage("FLX-COMMAND-0006", ex.Message) }, Array.Empty<CliCommandMessage>());
            }
        }

        private void InvokeCommand(CliRegisteredCommand[] commands, CliCommandOptions options, bool generator)
        {
            var warnings = new List<CliCommandMessage>();
            var context = new CliCommandContext(
                _request.RequestId,
                Globals.ProjectFolder,
                CancellationToken.None,
                (message, progress) => TryWriteEvent(new { type = "progress", requestId = _request.RequestId, value = progress, message }),
                warning =>
                {
                    warnings.Add(warning);
                    TryWriteEvent(new { type = "diagnostic", requestId = _request.RequestId, severity = "warning", code = warning.Code, message = warning.Message, details = warning.Details });
                });
            var command = generator
                ? CliCommandRegistry.RequireGenerator(commands, options.Name)
                : CliCommandRegistry.RequireCommand(commands, options.Name);
            TryWriteEvent(new { type = "phase", requestId = _request.RequestId, name = command.Attribute.Name });
            var invocation = CliCommandRegistry.BeginInvoke(command, options.Arguments, options.Confirm, context);

            void CompleteInvocation(CliCommandResult result)
            {
                warnings.AddRange(result.Warnings);
                if (!result.Succeeded)
                {
                    foreach (var error in result.Errors)
                        TryWriteEvent(new { type = "diagnostic", requestId = _request.RequestId, severity = "error", code = error.Code, message = error.Message, details = error.Details });
                }
                CompleteCommand(result.Data, result.Succeeded, result.Errors, warnings);
            }

            if (invocation.IsCompleted)
            {
                CompleteInvocation(invocation.Result);
                return;
            }

            Action update = null;
            update = () =>
            {
                try
                {
                    invocation.Update(TimeSpan.FromMilliseconds(10.0));
                    if (!invocation.IsCompleted)
                        return;
                    Editor.Instance.EditorUpdate -= update;
                    CompleteInvocation(invocation.Result);
                }
                catch (Exception ex)
                {
                    Editor.Instance.EditorUpdate -= update;
                    CompleteCommand(null, false, new[] { new CliCommandMessage("FLX-COMMAND-0006", ex.Message) }, warnings);
                }
            };
            Editor.Instance.EditorUpdate += update;
        }

        private void CompleteCommand(object data, bool success, IEnumerable<CliCommandMessage> errors, IEnumerable<CliCommandMessage> warnings)
        {
            if (_completed)
                return;
            var errorArray = errors?.ToArray() ?? Array.Empty<CliCommandMessage>();
            var warningArray = warnings?.ToArray() ?? Array.Empty<CliCommandMessage>();
            WriteResult(new
            {
                schemaVersion = 1,
                requestId = _request.RequestId,
                success,
                exitCode = success ? 0 : 6,
                data,
                errors = errorArray,
                warnings = warningArray,
            });
            _completed = true;
            TryWriteEvent(new { type = "result", requestId = _request.RequestId, success, exitCode = success ? 0 : 6 });
            Engine.RequestExit(success ? 0 : 1);
        }
    }
}
