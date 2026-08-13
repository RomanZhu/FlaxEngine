// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Text;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Overrides the command namespace used by members declared in the annotated type.
    /// </summary>
    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
    public sealed class EditorCommandAliasAttribute : Attribute
    {
        /// <summary>
        /// The command namespace.
        /// </summary>
        public readonly string Alias;

        /// <summary>
        /// Initializes a new instance of the <see cref="EditorCommandAliasAttribute"/> class.
        /// </summary>
        /// <param name="alias">The command namespace.</param>
        public EditorCommandAliasAttribute(string alias)
        {
            Alias = (alias ?? string.Empty).Trim('.', ' ');
        }
    }

    /// <summary>
    /// Exposes a method, field, property, or delegate in the editor console.
    /// </summary>
    [AttributeUsage(AttributeTargets.Method | AttributeTargets.Field | AttributeTargets.Property)]
    public sealed class EditorCommandAttribute : Attribute
    {
        internal readonly string Alias;
        internal readonly string Command;

        /// <summary>
        /// The command description shown by autocomplete and help.
        /// </summary>
        public readonly string Description;

        /// <summary>
        /// Initializes a new instance of the <see cref="EditorCommandAttribute"/> class.
        /// The member name is used as the command name.
        /// </summary>
        public EditorCommandAttribute()
        : this(string.Empty)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="EditorCommandAttribute"/> class.
        /// </summary>
        /// <param name="command">Command name, optionally prefixed with a namespace.</param>
        /// <param name="description">Command description.</param>
        public EditorCommandAttribute(string command, string description = "")
        {
            command ??= string.Empty;
            int separator = command.LastIndexOf('.');
            if (separator >= 0)
            {
                Alias = command.Substring(0, separator).Trim('.', ' ');
                command = command.Substring(separator + 1);
            }
            else
            {
                Alias = string.Empty;
            }

            Command = command.Trim();
            Description = description ?? string.Empty;
        }
    }

    /// <summary>
    /// A single editor console autocomplete result.
    /// </summary>
    public sealed class EditorCommandSuggestion
    {
        /// <summary>
        /// The text inserted into the command line.
        /// </summary>
        public string Text { get; internal set; }

        /// <summary>
        /// The short display name.
        /// </summary>
        public string Display { get; internal set; }

        /// <summary>
        /// The command syntax and description.
        /// </summary>
        public string Detail { get; internal set; }

        internal bool IsCommand { get; set; }
    }

    internal readonly struct EditorConsoleToken
    {
        public readonly string Value;
        public readonly int Start;

        public EditorConsoleToken(string value, int start)
        {
            Value = value;
            Start = start;
        }
    }

    internal enum EditorConsoleMessageKind
    {
        Command,
        Result,
        Error,
    }

    internal static class EditorConsoleParser
    {
        public static bool Tokenize(string input, List<EditorConsoleToken> tokens, out string error, bool allowOpenQuote = false)
        {
            tokens.Clear();
            error = null;
            if (string.IsNullOrWhiteSpace(input))
                return true;

            int index = 0;
            while (index < input.Length)
            {
                while (index < input.Length && char.IsWhiteSpace(input[index]))
                    index++;
                if (index >= input.Length)
                    break;

                int start = index;
                char quote = '\0';
                var value = new StringBuilder();
                while (index < input.Length)
                {
                    char c = input[index];
                    if (quote == '\0' && char.IsWhiteSpace(c))
                        break;
                    if ((c == '"' || c == '\'') && quote == '\0')
                    {
                        quote = c;
                        index++;
                        continue;
                    }
                    if (c == quote)
                    {
                        quote = '\0';
                        index++;
                        continue;
                    }
                    if (c == '\\' && index + 1 < input.Length &&
                        (input[index + 1] == '\\' || input[index + 1] == quote))
                    {
                        value.Append(input[index + 1]);
                        index += 2;
                        continue;
                    }

                    value.Append(c);
                    index++;
                }

                if (quote != '\0' && !allowOpenQuote)
                {
                    error = "Unterminated quoted argument.";
                    return false;
                }

                tokens.Add(new EditorConsoleToken(value.ToString(), start));
            }

            return true;
        }

        public static bool TryParseArguments(ParameterInfo[] parameters, IReadOnlyList<EditorConsoleToken> tokens,
            int firstToken, out object[] values, out string error)
        {
            values = new object[parameters.Length];
            error = null;
            int tokenIndex = firstToken;

            for (int parameterIndex = 0; parameterIndex < parameters.Length; parameterIndex++)
            {
                ParameterInfo parameter = parameters[parameterIndex];
                bool isParams = parameter.GetCustomAttribute<ParamArrayAttribute>() != null;
                if (isParams)
                {
                    Type elementType = parameter.ParameterType.GetElementType();
                    var items = new List<object>();
                    while (tokenIndex < tokens.Count)
                    {
                        if (!TryConvert(elementType, tokens, ref tokenIndex, out object item, out error))
                            return false;
                        items.Add(item);
                    }

                    Array array = Array.CreateInstance(elementType, items.Count);
                    for (int i = 0; i < items.Count; i++)
                        array.SetValue(items[i], i);
                    values[parameterIndex] = array;
                    continue;
                }

                if (tokenIndex >= tokens.Count)
                {
                    if (parameter.HasDefaultValue)
                    {
                        values[parameterIndex] = parameter.DefaultValue;
                        continue;
                    }

                    error = $"Missing argument '{parameter.Name}'.";
                    return false;
                }

                if (!TryConvert(parameter.ParameterType, tokens, ref tokenIndex, out values[parameterIndex], out error))
                {
                    error = $"Argument '{parameter.Name}': {error}";
                    return false;
                }
            }

            if (tokenIndex != tokens.Count)
            {
                error = $"Too many arguments ({tokens.Count - tokenIndex} extra).";
                return false;
            }

            return true;
        }

        public static bool TryConvert(Type type, IReadOnlyList<EditorConsoleToken> tokens, ref int tokenIndex,
            out object value, out string error)
        {
            value = null;
            error = null;
            Type target = Nullable.GetUnderlyingType(type) ?? type;

            if (target == typeof(Float2))
            {
                if (!TryFloatComponents(tokens, ref tokenIndex, 2, out float[] parts, out error))
                    return false;
                value = new Float2(parts[0], parts[1]);
                return true;
            }
            if (target == typeof(Float3))
            {
                if (!TryFloatComponents(tokens, ref tokenIndex, 3, out float[] parts, out error))
                    return false;
                value = new Float3(parts[0], parts[1], parts[2]);
                return true;
            }
            if (target == typeof(Float4))
            {
                if (!TryFloatComponents(tokens, ref tokenIndex, 4, out float[] parts, out error))
                    return false;
                value = new Float4(parts[0], parts[1], parts[2], parts[3]);
                return true;
            }
            if (target == typeof(Quaternion))
            {
                if (!TryFloatComponents(tokens, ref tokenIndex, 4, out float[] parts, out error))
                    return false;
                value = new Quaternion(parts[0], parts[1], parts[2], parts[3]);
                return true;
            }

            if (tokenIndex >= tokens.Count)
            {
                error = "Not enough arguments.";
                return false;
            }

            string text = tokens[tokenIndex++].Value;
            if (target == typeof(string))
            {
                value = text;
                return true;
            }
            if (target == typeof(bool) && bool.TryParse(text, out bool boolValue))
                value = boolValue;
            else if (target == typeof(byte) && byte.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out byte byteValue))
                value = byteValue;
            else if (target == typeof(sbyte) && sbyte.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out sbyte sbyteValue))
                value = sbyteValue;
            else if (target == typeof(short) && short.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out short shortValue))
                value = shortValue;
            else if (target == typeof(ushort) && ushort.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out ushort ushortValue))
                value = ushortValue;
            else if (target == typeof(int) && int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out int intValue))
                value = intValue;
            else if (target == typeof(uint) && uint.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out uint uintValue))
                value = uintValue;
            else if (target == typeof(long) && long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out long longValue))
                value = longValue;
            else if (target == typeof(ulong) && ulong.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out ulong ulongValue))
                value = ulongValue;
            else if (target == typeof(float) && float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out float floatValue))
                value = floatValue;
            else if (target == typeof(double) && double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double doubleValue))
                value = doubleValue;
            else if (target == typeof(decimal) && decimal.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out decimal decimalValue))
                value = decimalValue;
            else if (target == typeof(char) && text.Length == 1)
                value = text[0];
            else if (target == typeof(Guid) && Guid.TryParse(text, out Guid guidValue))
                value = guidValue;
            else if (target.IsEnum && Enum.TryParse(target, text, true, out object enumValue))
                value = enumValue;
            else
            {
                error = $"Cannot parse '{text}' as {FriendlyTypeName(target)}.";
                return false;
            }

            return true;
        }

        private static bool TryFloatComponents(IReadOnlyList<EditorConsoleToken> tokens, ref int tokenIndex, int count,
            out float[] parts, out string error)
        {
            parts = new float[count];
            error = null;
            for (int i = 0; i < count; i++)
            {
                if (tokenIndex >= tokens.Count ||
                    !float.TryParse(tokens[tokenIndex++].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out parts[i]))
                {
                    error = $"Expected {count} numeric components.";
                    return false;
                }
            }
            return true;
        }

        public static string FriendlyTypeName(Type type)
        {
            Type target = Nullable.GetUnderlyingType(type) ?? type;
            if (target == typeof(string)) return "string";
            if (target == typeof(bool)) return "bool";
            if (target == typeof(byte)) return "byte";
            if (target == typeof(sbyte)) return "sbyte";
            if (target == typeof(short)) return "short";
            if (target == typeof(ushort)) return "ushort";
            if (target == typeof(int)) return "int";
            if (target == typeof(uint)) return "uint";
            if (target == typeof(long)) return "long";
            if (target == typeof(ulong)) return "ulong";
            if (target == typeof(float)) return "float";
            if (target == typeof(double)) return "double";
            if (target == typeof(decimal)) return "decimal";
            if (target == typeof(char)) return "char";
            if (target.IsArray) return FriendlyTypeName(target.GetElementType()) + "[]";
            return target.Name;
        }

        public static string FormatValue(object value)
        {
            if (value == null)
                return "null";
            if (value is string text)
                return text;
            if (value is IFormattable formattable)
                return formattable.ToString(null, CultureInfo.InvariantCulture);
            return value.ToString();
        }
    }

    internal sealed class EditorCommandBinding
    {
        public MemberInfo Member;
        public Type DeclaringType;
        public bool IsStatic;
        public string Syntax;
    }

    internal sealed class EditorCommandRecord
    {
        public string Name;
        public EditorCommandAttribute Attribute;
        public readonly List<EditorCommandBinding> Bindings = new List<EditorCommandBinding>();
    }

    internal sealed class EditorConsoleRegistry
    {
        private const BindingFlags CommandFlags = BindingFlags.Public | BindingFlags.NonPublic |
                                                  BindingFlags.Static | BindingFlags.Instance |
                                                  BindingFlags.DeclaredOnly;

        private readonly Dictionary<string, EditorCommandRecord> _commands = new Dictionary<string, EditorCommandRecord>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<Type, List<WeakReference<object>>> _instances = new Dictionary<Type, List<WeakReference<object>>>();
        private readonly HashSet<Type> _scannedTypes = new HashSet<Type>();
        private readonly Dictionary<string, Dictionary<int, EditorConsole.ArgumentAutoCompleteProvider>> _providers =
            new Dictionary<string, Dictionary<int, EditorConsole.ArgumentAutoCompleteProvider>>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, Dictionary<int, EditorConsole.ArgumentAutoCompleteProviderWithContext>> _contextProviders =
            new Dictionary<string, Dictionary<int, EditorConsole.ArgumentAutoCompleteProviderWithContext>>(StringComparer.OrdinalIgnoreCase);
        private readonly List<EditorConsoleToken> _tokens = new List<EditorConsoleToken>();
        private readonly List<string> _providerResults = new List<string>();

        public int CommandCount => _commands.Count;

        public IEnumerable<string> GetHelp(string filter)
        {
            filter ??= string.Empty;
            return _commands.Values
                .Where(command => filter.Length == 0 || command.Name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0)
                .OrderBy(command => command.Name, StringComparer.OrdinalIgnoreCase)
                .Select(command => string.IsNullOrWhiteSpace(command.Attribute.Description)
                    ? command.Bindings[0].Syntax
                    : command.Bindings[0].Syntax + " — " + command.Attribute.Description);
        }

        public void Clear()
        {
            _commands.Clear();
            _instances.Clear();
            _scannedTypes.Clear();
            _providers.Clear();
            _contextProviders.Clear();
        }

        public void ScanAssembly(Assembly assembly)
        {
            if (assembly == null || assembly.IsDynamic)
                return;

            Type[] types;
            try
            {
                types = assembly.GetTypes();
            }
            catch (ReflectionTypeLoadException exception)
            {
                types = exception.Types.Where(type => type != null).ToArray();
            }

            foreach (Type type in types)
                RegisterStaticType(type);
        }

        public void RegisterStaticType(Type type)
        {
            if (type == null || !_scannedTypes.Add(type))
                return;

            string typeAlias = type.GetCustomAttribute<EditorCommandAliasAttribute>()?.Alias ?? string.Empty;
            foreach (MemberInfo member in type.GetMembers(CommandFlags))
            {
                EditorCommandAttribute attribute = member.GetCustomAttribute<EditorCommandAttribute>();
                if (attribute == null)
                    continue;

                bool isStatic;
                switch (member)
                {
                case MethodInfo method:
                    isStatic = method.IsStatic;
                    break;
                case FieldInfo field:
                    isStatic = field.IsStatic;
                    break;
                case PropertyInfo property:
                    MethodInfo accessor = property.GetMethod ?? property.SetMethod;
                    if (accessor == null)
                        continue;
                    isStatic = accessor.IsStatic;
                    break;
                default:
                    continue;
                }

                string command = string.IsNullOrWhiteSpace(attribute.Command) ? member.Name : attribute.Command;
                string alias = string.IsNullOrWhiteSpace(attribute.Alias) ? typeAlias : attribute.Alias;
                string name = string.IsNullOrWhiteSpace(alias) ? command : alias + "." + command;
                if (!_commands.TryGetValue(name, out EditorCommandRecord record))
                {
                    record = new EditorCommandRecord
                    {
                        Name = name,
                        Attribute = attribute,
                    };
                    _commands.Add(name, record);
                }

                record.Bindings.Add(new EditorCommandBinding
                {
                    Member = member,
                    DeclaringType = type,
                    IsStatic = isStatic,
                    Syntax = BuildSyntax(name, member),
                });
            }
        }

        public void Register(object instance)
        {
            if (instance == null)
                return;
            Type type = instance.GetType();
            RegisterStaticType(type);
            if (!_instances.TryGetValue(type, out List<WeakReference<object>> instances))
            {
                instances = new List<WeakReference<object>>();
                _instances.Add(type, instances);
            }
            RemoveDead(instances);
            if (!instances.Any(reference => reference.TryGetTarget(out object target) && ReferenceEquals(target, instance)))
                instances.Add(new WeakReference<object>(instance));
        }

        public void Unregister(object instance)
        {
            if (instance == null || !_instances.TryGetValue(instance.GetType(), out List<WeakReference<object>> instances))
                return;
            instances.RemoveAll(reference => !reference.TryGetTarget(out object target) || ReferenceEquals(target, instance));
        }

        public bool Execute(string input, out List<string> results, out string error)
        {
            results = new List<string>();
            error = null;
            if (!EditorConsoleParser.Tokenize(input, _tokens, out error) || _tokens.Count == 0)
                return false;

            string name = _tokens[0].Value;
            if (!_commands.TryGetValue(name, out EditorCommandRecord record))
            {
                error = $"Unknown editor command '{name}'.";
                return false;
            }

            string lastError = null;
            foreach (EditorCommandBinding binding in record.Bindings)
            {
                if (!TryPrepare(binding, _tokens, out object[] arguments, out bool assign, out bool invokeDelegate, out lastError))
                    continue;

                List<object> targets = GetTargets(binding);
                if (!binding.IsStatic && targets.Count == 0)
                {
                    lastError = $"No registered instance for '{binding.DeclaringType.Name}'.";
                    continue;
                }
                if (binding.IsStatic)
                    targets.Add(null);

                try
                {
                    foreach (object target in targets)
                    {
                        object result = Invoke(binding, target, arguments, assign, invokeDelegate);
                        if (result != null || IsReadableValue(binding.Member, assign, invokeDelegate))
                            results.Add(EditorConsoleParser.FormatValue(result));
                    }
                    if (results.Count == 0)
                        results.Add("OK");
                    return true;
                }
                catch (TargetInvocationException exception)
                {
                    error = (exception.InnerException ?? exception).Message;
                    return false;
                }
                catch (Exception exception)
                {
                    error = exception.Message;
                    return false;
                }
            }

            error = lastError ?? $"Arguments do not match '{name}'.";
            return false;
        }

        private static bool TryPrepare(EditorCommandBinding binding, IReadOnlyList<EditorConsoleToken> tokens,
            out object[] arguments, out bool assign, out bool invokeDelegate, out string error)
        {
            arguments = null;
            assign = false;
            invokeDelegate = false;
            error = null;

            if (binding.Member is MethodInfo method)
                return EditorConsoleParser.TryParseArguments(method.GetParameters(), tokens, 1, out arguments, out error);

            Type valueType = binding.Member is FieldInfo field ? field.FieldType : ((PropertyInfo)binding.Member).PropertyType;
            if (typeof(Delegate).IsAssignableFrom(valueType))
            {
                MethodInfo invoke = valueType.GetMethod("Invoke");
                if (invoke == null)
                {
                    error = "Delegate has no invoke signature.";
                    return false;
                }
                invokeDelegate = true;
                return EditorConsoleParser.TryParseArguments(invoke.GetParameters(), tokens, 1, out arguments, out error);
            }

            if (tokens.Count == 1)
                return true;

            int tokenIndex = 1;
            if (!EditorConsoleParser.TryConvert(valueType, tokens, ref tokenIndex, out object value, out error))
                return false;
            if (tokenIndex != tokens.Count)
            {
                error = "Too many arguments.";
                return false;
            }
            assign = true;
            arguments = new[] { value };
            return true;
        }

        private static object Invoke(EditorCommandBinding binding, object target, object[] arguments,
            bool assign, bool invokeDelegate)
        {
            if (binding.Member is MethodInfo method)
                return method.Invoke(target, arguments);

            object value;
            if (binding.Member is FieldInfo field)
            {
                if (assign)
                {
                    field.SetValue(target, arguments[0]);
                    return null;
                }
                value = field.GetValue(target);
            }
            else
            {
                var property = (PropertyInfo)binding.Member;
                if (assign)
                {
                    property.SetValue(target, arguments[0]);
                    return null;
                }
                value = property.GetValue(target);
            }

            if (invokeDelegate)
                return ((Delegate)value)?.DynamicInvoke(arguments);
            return value;
        }

        private static bool IsReadableValue(MemberInfo member, bool assign, bool invokeDelegate)
        {
            if (assign)
                return false;
            if (member is MethodInfo method)
                return method.ReturnType != typeof(void);
            if (invokeDelegate)
            {
                Type delegateType = member is FieldInfo field ? field.FieldType : ((PropertyInfo)member).PropertyType;
                return delegateType.GetMethod("Invoke")?.ReturnType != typeof(void);
            }
            return true;
        }

        private List<object> GetTargets(EditorCommandBinding binding)
        {
            var result = new List<object>();
            if (binding.IsStatic)
                return result;

            foreach (KeyValuePair<Type, List<WeakReference<object>>> pair in _instances)
            {
                if (!binding.DeclaringType.IsAssignableFrom(pair.Key))
                    continue;
                RemoveDead(pair.Value);
                foreach (WeakReference<object> reference in pair.Value)
                {
                    if (reference.TryGetTarget(out object target))
                        result.Add(target);
                }
            }
            return result;
        }

        private static void RemoveDead(List<WeakReference<object>> instances)
        {
            instances.RemoveAll(reference => !reference.TryGetTarget(out _));
        }

        private static string BuildSyntax(string name, MemberInfo member)
        {
            ParameterInfo[] parameters;
            if (member is MethodInfo method)
            {
                parameters = method.GetParameters();
            }
            else
            {
                Type valueType = member is FieldInfo field ? field.FieldType : ((PropertyInfo)member).PropertyType;
                if (typeof(Delegate).IsAssignableFrom(valueType))
                    parameters = valueType.GetMethod("Invoke")?.GetParameters() ?? Array.Empty<ParameterInfo>();
                else
                    return $"{name} [value:{EditorConsoleParser.FriendlyTypeName(valueType)}]";
            }

            var parts = new List<string> { name };
            foreach (ParameterInfo parameter in parameters)
            {
                bool optional = parameter.HasDefaultValue;
                bool isParams = parameter.GetCustomAttribute<ParamArrayAttribute>() != null;
                Type type = isParams ? parameter.ParameterType.GetElementType() : parameter.ParameterType;
                string value = $"{parameter.Name}:{EditorConsoleParser.FriendlyTypeName(type)}";
                if (isParams)
                    value += "...";
                else if (optional)
                    value += "=" + EditorConsoleParser.FormatValue(parameter.DefaultValue);
                parts.Add(optional || isParams ? "[" + value + "]" : "<" + value + ">");
            }
            return string.Join(" ", parts);
        }

        public void RegisterProvider(string commandName, int argumentIndex, EditorConsole.ArgumentAutoCompleteProvider provider)
        {
            RegisterProvider(_providers, commandName, argumentIndex, provider);
        }

        public void RegisterProvider(string commandName, int argumentIndex, EditorConsole.ArgumentAutoCompleteProviderWithContext provider)
        {
            RegisterProvider(_contextProviders, commandName, argumentIndex, provider);
        }

        private static void RegisterProvider<T>(Dictionary<string, Dictionary<int, T>> providers,
            string commandName, int argumentIndex, T provider)
        {
            commandName = commandName.Trim();
            if (!providers.TryGetValue(commandName, out Dictionary<int, T> byIndex))
            {
                byIndex = new Dictionary<int, T>();
                providers.Add(commandName, byIndex);
            }
            byIndex[argumentIndex] = provider;
        }

        public void UnregisterProvider(string commandName, int argumentIndex)
        {
            UnregisterProvider(_providers, commandName, argumentIndex);
            UnregisterProvider(_contextProviders, commandName, argumentIndex);
        }

        private static void UnregisterProvider<T>(Dictionary<string, Dictionary<int, T>> providers,
            string commandName, int argumentIndex)
        {
            if (!providers.TryGetValue(commandName, out Dictionary<int, T> byIndex))
                return;
            byIndex.Remove(argumentIndex);
            if (byIndex.Count == 0)
                providers.Remove(commandName);
        }

        public void GetSuggestions(string input, List<EditorCommandSuggestion> output, int maximum = 20)
        {
            output.Clear();
            input ??= string.Empty;
            if (!EditorConsoleParser.Tokenize(input, _tokens, out _, true))
                return;

            bool endsInWhitespace = input.Length > 0 && char.IsWhiteSpace(input[input.Length - 1]);
            if (_tokens.Count == 0 || (_tokens.Count == 1 && !endsInWhitespace))
            {
                AddCommandSuggestions(_tokens.Count == 0 ? string.Empty : _tokens[0].Value, output, maximum);
                return;
            }

            string typedCommand = _tokens[0].Value;
            if (!_commands.TryGetValue(typedCommand, out EditorCommandRecord command))
                return;

            int argumentIndex;
            int replacementStart;
            string current;
            int completedCount;
            if (endsInWhitespace)
            {
                argumentIndex = _tokens.Count - 1;
                replacementStart = input.Length;
                current = string.Empty;
                completedCount = _tokens.Count - 1;
            }
            else
            {
                EditorConsoleToken token = _tokens[_tokens.Count - 1];
                argumentIndex = _tokens.Count - 2;
                replacementStart = token.Start;
                current = token.Value;
                completedCount = _tokens.Count - 2;
            }

            _providerResults.Clear();
            var completed = new List<string>(completedCount);
            for (int i = 0; i < completedCount; i++)
                completed.Add(_tokens[i + 1].Value);

            bool hasProvider = false;
            if (_contextProviders.TryGetValue(command.Name, out Dictionary<int, EditorConsole.ArgumentAutoCompleteProviderWithContext> contexts) &&
                contexts.TryGetValue(argumentIndex, out EditorConsole.ArgumentAutoCompleteProviderWithContext contextProvider))
            {
                contextProvider(command.Name, argumentIndex, current, completed, _providerResults);
                hasProvider = true;
            }
            if (_providers.TryGetValue(command.Name, out Dictionary<int, EditorConsole.ArgumentAutoCompleteProvider> providers) &&
                providers.TryGetValue(argumentIndex, out EditorConsole.ArgumentAutoCompleteProvider provider))
            {
                provider(command.Name, argumentIndex, current, _providerResults);
                hasProvider = true;
            }
            if (!hasProvider)
                AddBuiltInArgumentSuggestions(command, argumentIndex, _providerResults);

            string prefix = input.Substring(0, replacementStart);
            foreach (var suggestion in _providerResults.Where(value => value != null).Distinct(StringComparer.OrdinalIgnoreCase)
                         .Select(value => new { Value = value, Score = FuzzyScore(value, current) })
                         .Where(item => item.Score >= 0)
                         .OrderByDescending(item => item.Score)
                         .ThenBy(item => item.Value, StringComparer.OrdinalIgnoreCase)
                         .Take(maximum))
            {
                output.Add(new EditorCommandSuggestion
                {
                    Text = prefix + QuoteSuggestion(suggestion.Value),
                    Display = suggestion.Value,
                    Detail = command.Bindings[0].Syntax,
                    IsCommand = false,
                });
            }
        }

        private static string QuoteSuggestion(string value)
        {
            if (value.IndexOfAny(new[] { ' ', '\t', '\r', '\n', '"' }) < 0)
                return value;
            return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        }

        private void AddCommandSuggestions(string query, List<EditorCommandSuggestion> output, int maximum)
        {
            foreach (var item in _commands.Values
                         .Select(command => new { Command = command, Score = FuzzyScore(command.Name, query) })
                         .Where(item => item.Score >= 0)
                         .OrderByDescending(item => item.Score)
                         .ThenBy(item => item.Command.Name, StringComparer.OrdinalIgnoreCase)
                         .Take(maximum))
            {
                EditorCommandRecord command = item.Command;
                output.Add(new EditorCommandSuggestion
                {
                    Text = command.Name,
                    Display = command.Name,
                    Detail = string.IsNullOrWhiteSpace(command.Attribute.Description)
                        ? command.Bindings[0].Syntax
                        : command.Bindings[0].Syntax + " — " + command.Attribute.Description,
                    IsCommand = true,
                });
            }
        }

        private static void AddBuiltInArgumentSuggestions(EditorCommandRecord command, int argumentIndex, List<string> suggestions)
        {
            EditorCommandBinding binding = command.Bindings[0];
            ParameterInfo[] parameters = binding.Member is MethodInfo method ? method.GetParameters() : Array.Empty<ParameterInfo>();
            if (argumentIndex < 0 || argumentIndex >= parameters.Length)
                return;

            Type type = Nullable.GetUnderlyingType(parameters[argumentIndex].ParameterType) ?? parameters[argumentIndex].ParameterType;
            if (type.IsEnum)
                suggestions.AddRange(Enum.GetNames(type));
            else if (type == typeof(bool))
            {
                suggestions.Add("true");
                suggestions.Add("false");
            }
        }

        private static int FuzzyScore(string candidate, string query)
        {
            if (string.IsNullOrEmpty(query))
                return 1;
            if (candidate.Equals(query, StringComparison.OrdinalIgnoreCase))
                return 10000;
            if (candidate.StartsWith(query, StringComparison.OrdinalIgnoreCase))
                return 5000 - candidate.Length;

            int candidateIndex = 0;
            int gaps = 0;
            for (int queryIndex = 0; queryIndex < query.Length; queryIndex++)
            {
                char wanted = char.ToUpperInvariant(query[queryIndex]);
                int previous = candidateIndex;
                while (candidateIndex < candidate.Length && char.ToUpperInvariant(candidate[candidateIndex]) != wanted)
                    candidateIndex++;
                if (candidateIndex == candidate.Length)
                    return -1;
                gaps += candidateIndex - previous;
                candidateIndex++;
            }
            return 1000 - gaps - candidate.Length;
        }
    }

    /// <summary>
    /// Editor-only command console API. Commands are independent from runtime console and engine debug commands.
    /// </summary>
    public static class EditorConsole
    {
        /// <summary>
        /// Provides autocomplete values for a command argument.
        /// </summary>
        public delegate void ArgumentAutoCompleteProvider(string commandName, int argumentIndex,
            string currentArgument, List<string> suggestions);

        /// <summary>
        /// Provides autocomplete values for a command argument, including already parsed arguments.
        /// </summary>
        public delegate void ArgumentAutoCompleteProviderWithContext(string commandName, int argumentIndex,
            string currentArgument, IReadOnlyList<string> parsedArguments, List<string> suggestions);

        private static readonly EditorConsoleRegistry Registry = new EditorConsoleRegistry();
        private static readonly List<EditorCommandSuggestion> Suggestions = new List<EditorCommandSuggestion>();
        private static bool _initialized;
        private static bool _scanReady;

        internal static Action<EditorConsoleMessageKind, string> MessageWritten;
        internal static Action ClearRequested;
        internal static Action OpenRequested;
        internal static Action CloseRequested;
        internal static Action OpenHtmlRequested;
        internal static Action OpenLogFolderRequested;
        internal static Func<string> HtmlLogPathProvider;

        /// <summary>
        /// Maximum number of entries retained by the editor console.
        /// </summary>
        [EditorCommand("console.Lines", "Maximum number of entries retained by the editor console.")]
        public static int Lines = 8192;

        /// <summary>
        /// Gets the current editor console HTML log path.
        /// </summary>
        public static string HtmlLogPath => HtmlLogPathProvider?.Invoke();

        internal static void Initialize()
        {
            if (_initialized)
                return;
            _initialized = true;
            ScriptsBuilder.ScriptsReloadBegin += OnScriptsReloadBegin;
            ScriptsBuilder.ScriptsReloadEnd += OnScriptsReloadEnd;
            if (Editor.Instance.IsInitialized)
            {
                _scanReady = true;
                ScanAssemblies();
            }
            else
            {
                Editor.Instance.InitializationEnd += OnEditorInitializationEnd;
            }
        }

        internal static void Shutdown()
        {
            if (!_initialized)
                return;
            ScriptsBuilder.ScriptsReloadBegin -= OnScriptsReloadBegin;
            ScriptsBuilder.ScriptsReloadEnd -= OnScriptsReloadEnd;
            Editor.Instance.InitializationEnd -= OnEditorInitializationEnd;
            Registry.Clear();
            Suggestions.Clear();
            MessageWritten = null;
            ClearRequested = null;
            OpenRequested = null;
            CloseRequested = null;
            OpenHtmlRequested = null;
            OpenLogFolderRequested = null;
            HtmlLogPathProvider = null;
            _scanReady = false;
            _initialized = false;
        }

        private static void OnEditorInitializationEnd()
        {
            Editor.Instance.InitializationEnd -= OnEditorInitializationEnd;
            _scanReady = true;
            ScanAssemblies();
        }

        private static void OnScriptsReloadBegin()
        {
            Registry.Clear();
            Suggestions.Clear();
        }

        private static void OnScriptsReloadEnd()
        {
            if (_scanReady)
                ScanAssemblies();
        }

        private static void ScanAssemblies()
        {
            Assembly editorAssembly = typeof(EditorConsole).Assembly;
            string editorAssemblyName = editorAssembly.GetName().Name;
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (assembly == editorAssembly || assembly.GetReferencedAssemblies().Any(reference => reference.Name == editorAssemblyName))
                    Registry.ScanAssembly(assembly);
            }
            RegisterArgumentAutoComplete("open.settings", 0, EditorOpenCommands.GetSettingsSuggestions);
        }

        /// <summary>
        /// Registers all annotated members declared by a type.
        /// </summary>
        public static void RegisterStaticType(Type type)
        {
            Initialize();
            Registry.RegisterStaticType(type);
        }

        /// <summary>
        /// Registers an object instance for instance editor commands.
        /// </summary>
        public static void Register(object instance)
        {
            Initialize();
            Registry.Register(instance);
        }

        /// <summary>
        /// Unregisters an object instance.
        /// </summary>
        public static void Unregister(object instance)
        {
            Registry.Unregister(instance);
        }

        /// <summary>
        /// Registers an autocomplete provider for a command argument.
        /// </summary>
        public static void RegisterArgumentAutoComplete(string commandName, int argumentIndex,
            ArgumentAutoCompleteProvider provider)
        {
            ValidateProvider(commandName, argumentIndex, provider);
            Initialize();
            Registry.RegisterProvider(commandName, argumentIndex, provider);
        }

        /// <summary>
        /// Registers a contextual autocomplete provider for a command argument.
        /// </summary>
        public static void RegisterArgumentAutoComplete(string commandName, int argumentIndex,
            ArgumentAutoCompleteProviderWithContext provider)
        {
            ValidateProvider(commandName, argumentIndex, provider);
            Initialize();
            Registry.RegisterProvider(commandName, argumentIndex, provider);
        }

        /// <summary>
        /// Removes autocomplete providers for a command argument.
        /// </summary>
        public static void UnregisterArgumentAutoComplete(string commandName, int argumentIndex)
        {
            if (string.IsNullOrWhiteSpace(commandName) || argumentIndex < 0)
                return;
            Registry.UnregisterProvider(commandName.Trim(), argumentIndex);
        }

        /// <summary>
        /// Executes an editor command.
        /// </summary>
        /// <returns>True if the command executed successfully.</returns>
        public static bool Execute(string input)
        {
            Initialize();
            input = input?.Trim() ?? string.Empty;
            if (input.Length == 0)
                return false;

            MessageWritten?.Invoke(EditorConsoleMessageKind.Command, "> " + input);
            if (!Registry.Execute(input, out List<string> results, out string error))
            {
                MessageWritten?.Invoke(EditorConsoleMessageKind.Error, error);
                return false;
            }

            foreach (string result in results)
                MessageWritten?.Invoke(EditorConsoleMessageKind.Result, result);
            return true;
        }

        /// <summary>
        /// Gets editor command autocomplete results.
        /// </summary>
        public static IReadOnlyList<EditorCommandSuggestion> GetAutoComplete(string input, int maximum = 20)
        {
            Initialize();
            Registry.GetSuggestions(input, Suggestions, Math.Max(1, maximum));
            return Suggestions;
        }

        /// <summary>
        /// Opens and focuses the editor console.
        /// </summary>
        public static void Open() => OpenRequested?.Invoke();

        /// <summary>
        /// Closes the editor console tab.
        /// </summary>
        public static void Close() => CloseRequested?.Invoke();

        /// <summary>
        /// Clears retained editor console entries.
        /// </summary>
        public static void Clear() => ClearRequested?.Invoke();

        /// <summary>
        /// Opens this editor session's HTML console log.
        /// </summary>
        public static void OpenHtmlLog() => OpenHtmlRequested?.Invoke();

        /// <summary>
        /// Opens the editor log folder.
        /// </summary>
        public static void OpenLogFolder() => OpenLogFolderRequested?.Invoke();

        private static void ValidateProvider(string commandName, int argumentIndex, Delegate provider)
        {
            if (string.IsNullOrWhiteSpace(commandName))
                throw new ArgumentException("Command name cannot be empty.", nameof(commandName));
            if (argumentIndex < 0)
                throw new ArgumentOutOfRangeException(nameof(argumentIndex));
            if (provider == null)
                throw new ArgumentNullException(nameof(provider));
        }

        internal static string GetHelp(string filter)
        {
            return string.Join(Environment.NewLine, Registry.GetHelp(filter));
        }
    }

    [EditorCommandAlias("console")]
    internal static class EditorConsoleCommands
    {
        [EditorCommand("Clear", "Clear retained console entries.")]
        private static void Clear() => EditorConsole.Clear();

        [EditorCommand("Open", "Open the editor console.")]
        private static void Open() => EditorConsole.Open();

        [EditorCommand("Close", "Close the editor console.")]
        private static void Close() => EditorConsole.Close();

        [EditorCommand("Html", "Open this editor session's HTML console log.")]
        private static string Html()
        {
            EditorConsole.OpenHtmlLog();
            return EditorConsole.HtmlLogPath;
        }

        [EditorCommand("LogsFolder", "Open the editor log folder.")]
        private static void LogsFolder() => EditorConsole.OpenLogFolder();

        [EditorCommand("Help", "List editor commands, optionally filtered by name.")]
        private static string Help(string filter = "") => EditorConsole.GetHelp(filter);

        [EditorCommand("Echo", "Write text back to the console.")]
        private static string Echo(string text) => text;

        [EditorCommand("GarbageCollect", "Run a full managed garbage collection.")]
        private static void GarbageCollect()
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
        }
    }

    internal static class EditorOpenCommands
    {
        [EditorCommand("open.settings", "Open an Editor Options section.")]
        private static string Settings(string settingName)
        {
            var window = Editor.Instance.Windows.EditorOptionsWin;
            if (!window.OpenTab(settingName))
                throw new ArgumentException($"Unknown settings section '{settingName}'.", nameof(settingName));
            return $"Opened settings: {settingName}";
        }

        internal static void GetSettingsSuggestions(string commandName, int argumentIndex,
            string currentArgument, List<string> suggestions)
        {
            Editor.Instance.Windows.EditorOptionsWin.GetTabNames(suggestions);
        }
    }
}
