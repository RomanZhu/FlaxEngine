// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using Flax.CLI.Adapters;
using Flax.CLI.Core;
using Flax.CLI.Protocol;
using Flax.CLI.Services;

namespace Flax.CLI.Commands;

internal sealed class CommandDispatcher(
    AppPaths paths,
    EngineRegistry engines,
    ProjectRegistry projects,
    ConfigStore config,
    ContextResolver resolver,
    FlaxBuildAdapter buildAdapter,
    EditorAdapter editorAdapter,
    EditorBridgeClient bridgeClient)
{
    public async Task<CliResult> ExecuteAsync(CommandContext context)
    {
        if (context.Options.Version)
            return CliResult.Ok(new
            {
                cliVersion = Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "0.1.0",
                protocolVersion = "1.0",
            });
        if (context.Options.CommandTokens.Count == 0)
            return CliResult.Ok(HelpText);

        var command = context.Options.CommandTokens[0].ToLowerInvariant();
        var args = new CommandArguments(context.Options.CommandTokens.Skip(1));
        if (context.Options.Help)
            return CliResult.Ok(Help(command));

        return command switch
        {
            "help" => CliResult.Ok(Help(args.Positional())),
            "engines" => Engines(args),
            "engine" => Engine(args, context),
            "projects" => Projects(args),
            "open" => Open(args, context, false),
            "play" => Open(args, context, true),
            "generate" => await Generate(args, context),
            "compile" => await Compile(args, context, false),
            "clean" => await Compile(args, context, true),
            "build" => await Build(args, context),
            "assets" or "asset" => await Assets(args, context),
            "authoring-root" => AuthoringRoot(args, context),
            "doctor" => Doctor(args, context),
            "logs" => Logs(args, context),
            "env" => EnvironmentInfo(args, context),
            "config" => Config(args, context),
            "status" => Status(args, context),
            "editor" => await Editor(args, context),
            "console" => await ConsoleLogs(args, context),
            "commands" => await Commands(args, context),
            "command" => await Command(args, context),
            "scenes" or "actors" or "prefabs" => await Authoring(command, args, context),
            "install" or "uninstall" or "releases" or "platforms" or "templates" or "new" or "upgrade" => Deferred(command),
            "test" => CliResult.Fail(ExitCode.ContextRequired, "FLX-TEST-0004", "No project test adapter is registered."),
            "completion" => Completion(args),
            _ => throw CommandLine.Usage($"Unknown command '{command}'. Run 'flax help' for the command list."),
        };
    }

    private CliResult Engines(CommandArguments args)
    {
        var subcommand = args.Positional()?.ToLowerInvariant() ?? "list";
        switch (subcommand)
        {
        case "list":
            args.Complete();
            return CliResult.Ok(engines.List().Select(x => EngineView(x)).ToArray());
        case "add":
            var nickname = args.Option("--nickname");
            var source = args.Option("--source") ?? "local";
            var channel = args.Option("--channel") ?? "local";
            var roots = args.Positionals();
            args.Complete();
            if (roots.Count == 0)
                throw CommandLine.Usage("engines add requires at least one engine path.");
            if (roots.Count > 1 && nickname != null)
                throw CommandLine.Usage("--nickname can only be used when adding one engine.");
            return CliResult.Ok(roots.Select(x => EngineView(engines.Add(x, nickname, source, channel), false)).ToArray());
        case "remove":
            var removeSelector = args.Positional() ?? throw CommandLine.Usage("engines remove requires a selector.");
            args.Complete();
            engines.Remove(removeSelector);
            return CliResult.Ok(new { removed = removeSelector, filesDeleted = false });
        case "default":
            var defaultSelector = args.Positional();
            args.Complete();
            if (defaultSelector == null)
                return CliResult.Ok(engines.GetDefault() is { } current ? EngineView(current) : null);
            var selected = engines.SetDefault(defaultSelector);
            config.Set("defaultEngine", selected.Id, unset: false);
            return CliResult.Ok(EngineView(selected));
        case "info":
            var selector = args.Positional() ?? throw CommandLine.Usage("engines info requires a selector.");
            args.Complete();
            return CliResult.Ok(EngineView(engines.ResolveSelector(selector), includeCapabilities: true));
        default:
            throw CommandLine.Usage($"Unknown engines subcommand '{subcommand}'.");
        }
    }

    private CliResult Engine(CommandArguments args, CommandContext context)
    {
        var subcommand = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage("engine requires pin or unpin.");
        switch (subcommand)
        {
        case "pin":
            var selector = args.Positional() ?? context.Options.Engine;
            args.Complete();
            var project = FindProject(null, context.Options.Project);
            var selected = resolver.Resolve(project, selector);
            resolver.Pin(project, selected);
            return CliResult.Ok(new { project = project.Root, lockFile = project.LockFile, engine = EngineView(selected) });
        case "unpin":
            args.Complete();
            var unpinProject = FindProject(null, context.Options.Project);
            var existed = File.Exists(unpinProject.LockFile);
            if (existed)
                File.Delete(unpinProject.LockFile);
            return CliResult.Ok(new { project = unpinProject.Root, lockFile = unpinProject.LockFile, removed = existed });
        default:
            throw CommandLine.Usage($"Unknown engine subcommand '{subcommand}'.");
        }
    }

    private CliResult Projects(CommandArguments args)
    {
        var subcommand = args.Positional()?.ToLowerInvariant() ?? "list";
        switch (subcommand)
        {
        case "list":
            args.Complete();
            return CliResult.Ok(projects.List().Select(x => ProjectViewSafe(x)).ToArray());
        case "add":
            var add = ProjectContext.Find(args.Positional());
            args.Complete();
            projects.Add(add.Root);
            return CliResult.Ok(ProjectView(add));
        case "remove":
            var remove = args.Positional() ?? throw CommandLine.Usage("projects remove requires a project path.");
            args.Complete();
            return CliResult.Ok(new { project = Path.GetFullPath(remove), removed = projects.Remove(remove), filesDeleted = false });
        case "info":
            var info = ProjectContext.Find(args.Positional());
            args.Complete();
            return CliResult.Ok(ProjectView(info));
        case "size":
            var sizeProject = ProjectContext.Find(args.Positional());
            args.Complete();
            var bytes = Directory.EnumerateFiles(sizeProject.Root, "*", SearchOption.AllDirectories).Sum(x =>
            {
                try { return new FileInfo(x).Length; } catch { return 0L; }
            });
            return CliResult.Ok(new { project = sizeProject.Root, bytes });
        default:
            throw CommandLine.Usage($"Unknown projects subcommand '{subcommand}'.");
        }
    }

    private CliResult AuthoringRoot(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "get";
        var project = FindProject(null, context.Options.Project);
        switch (action)
        {
        case "get":
            args.Complete();
            var configured = config.GetString("authoringRoot", project.ProjectConfigFile) ?? "Content";
            return CliResult.Ok(new { project = project.Root, authoringRoot = AssetPath(project, configured), configured });
        case "set":
            var requested = args.Positional() ?? throw CommandLine.Usage("authoring-root set requires a path under the project Content folder.");
            args.Complete();
            var contentRoot = Path.GetFullPath(Path.Combine(project.Root, "Content")).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var resolved = AssetPath(project, requested).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var pathComparison = OperatingSystem.IsLinux() ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase;
            if (!resolved.Equals(contentRoot, pathComparison) && !resolved.StartsWith(contentRoot + Path.DirectorySeparatorChar, pathComparison))
                throw CommandLine.Usage("The authoring root must be the project Content folder or one of its descendants.");
            var relative = Path.GetRelativePath(project.Root, resolved).Replace(Path.DirectorySeparatorChar, '/');
            config.Set("authoringRoot", relative, unset: false, project.ProjectConfigFile);
            return CliResult.Ok(new { project = project.Root, authoringRoot = resolved, configured = relative });
        default:
            throw CommandLine.Usage($"Unknown authoring-root subcommand '{action}'.");
        }
    }

    private CliResult Open(CommandArguments args, CommandContext context, bool play)
    {
        var project = FindProject(args.Positional(), context.Options.Project);
        args.Complete();
        var engine = resolver.Resolve(project, context.Options.Engine);
        var process = editorAdapter.Open(engine, project, play, context.Options.PassThrough);
        projects.Add(project.Root);
        return CliResult.Ok(new { action = play ? "play" : "open", project = project.Root, engine = EngineView(engine), pid = process.ProcessId });
    }

    private async Task<CliResult> Generate(CommandArguments args, CommandContext context)
    {
        var ide = args.Option("--ide");
        var raw = args.Options("--build-arg");
        var project = FindProject(args.Positional(), context.Options.Project);
        args.Complete();
        var engine = resolver.Resolve(project, context.Options.Engine);
        var process = await buildAdapter.GenerateAsync(engine, project, ide, raw, context);
        return ChildResult("generate", engine, process);
    }

    private async Task<CliResult> Compile(CommandArguments args, CommandContext context, bool clean)
    {
        var targets = args.Options("--target");
        var configuration = args.Option("--configuration");
        var platform = args.Option("--platform");
        var architecture = args.Option("--arch");
        var raw = args.Options("--build-arg");
        var project = FindProject(args.Positional(), context.Options.Project);
        args.Complete();
        var engine = resolver.Resolve(project, context.Options.Engine);
        var process = await buildAdapter.CompileAsync(engine, project, targets, configuration, platform, architecture, raw, clean, context);
        return ChildResult(clean ? "clean" : "compile", engine, process);
    }

    private async Task<CliResult> Build(CommandArguments args, CommandContext context)
    {
        var preset = args.Option("--preset") ?? "Development";
        var target = args.Option("--target") ?? throw CommandLine.Usage("build requires --target <platform>.");
        var output = args.Option("--output");
        var customDefines = args.Options("--define");
        var clean = args.Flag("--clean");
        var runAfterBuild = args.Flag("--run");
        var project = FindProject(args.Positional(), context.Options.Project);
        args.Complete();
        var engine = resolver.Resolve(project, context.Options.Engine);
        var invocation = await editorAdapter.BuildAsync(engine, project, preset, target, output, customDefines, clean, runAfterBuild, context.Options.PassThrough, context);
        var details = new
        {
            adapter = invocation.Structured ? "one-shot-request" : "legacy-editor-arguments",
            preset,
            target,
            output,
            events = invocation.Events,
            result = invocation.Result,
        };
        var structuredFailed = invocation.Result is JsonElement protocolResult &&
                               protocolResult.TryGetProperty("success", out var success) &&
                               !success.GetBoolean();
        if (invocation.Process.ExitCode != 0 || structuredFailed)
        {
            var data = new { operation = "build", engine = EngineView(engine), invocation.Process.ProcessId, invocation.Process.ExitCode, stdout = invocation.Process.StandardOutput, stderr = invocation.Process.StandardError, details };
            return WithEvents(CliResult.Fail(ExitCode.OperationFailed, "FLX-BUILD-0006", "The Game Cooker build failed.", data), invocation.Events);
        }
        return WithEvents(ChildResult("build", engine, invocation.Process, details), invocation.Events);
    }

    private async Task<CliResult> Assets(CommandArguments args, CommandContext context)
    {
        var subcommand = args.Positional()?.ToLowerInvariant() ?? "list";
        var project = FindProject(null, context.Options.Project);
        var options = new AssetRequestOptions { Action = subcommand };
        switch (subcommand)
        {
        case "list":
            options.Path = AssetPath(project, args.Positional() ?? ".");
            options.Recursive = args.Flag("--recursive");
            break;
        case "types":
            options.Path = AssetPath(project, args.Positional() ?? ".");
            break;
        case "info" or "save" or "reimport":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage($"assets {subcommand} requires an asset path."));
            break;
        case "create":
            options.AssetType = args.Positional() ?? throw CommandLine.Usage("assets create requires an asset type.");
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets create requires an output path."));
            break;
        case "mkdir":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets mkdir requires a folder path."));
            break;
        case "import":
            options.Destination = AssetPath(project, args.Option("--to") ?? throw CommandLine.Usage("assets import requires --to <content-folder>."));
            options.Sources = args.Positionals().Select(Path.GetFullPath).ToArray();
            if (options.Sources.Length == 0)
                throw CommandLine.Usage("assets import requires at least one source path.");
            break;
        case "duplicate" or "move":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage($"assets {subcommand} requires a source path."));
            options.Destination = AssetPath(project, args.Positional() ?? throw CommandLine.Usage($"assets {subcommand} requires a destination path."));
            break;
        case "rename":
            options.Action = "move";
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets rename requires a source path."));
            var newName = args.Positional() ?? throw CommandLine.Usage("assets rename requires a new name.");
            if (newName.IndexOfAny([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) >= 0)
                throw CommandLine.Usage("assets rename accepts a name, not a path. Use assets move to change folders.");
            var extension = Path.GetExtension(options.Path);
            if (!string.IsNullOrEmpty(extension) && string.IsNullOrEmpty(Path.GetExtension(newName)))
                newName += extension;
            options.Destination = Path.Combine(Path.GetDirectoryName(options.Path)!, newName);
            break;
        case "delete":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets delete requires an asset path."));
            options.Force = args.Flag("--yes");
            if (!options.Force)
                throw CommandLine.Usage("assets delete is destructive and requires --yes.");
            break;
        case "export":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets export requires an asset path."));
            options.Destination = Path.GetFullPath(args.Option("--to") ?? throw CommandLine.Usage("assets export requires --to <folder>."));
            break;
        case "get":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets get requires an asset path."));
            options.PropertyPath = args.Positional() ?? throw CommandLine.Usage("assets get requires a public property path.");
            break;
        case "set":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets set requires an asset path."));
            options.PropertyPath = args.Positional() ?? throw CommandLine.Usage("assets set requires a public property path.");
            var rawValue = args.Option("--value") ?? args.Positional() ?? throw CommandLine.Usage("assets set requires a JSON value.");
            try
            {
                options.Value = JsonNode.Parse(rawValue);
            }
            catch (JsonException)
            {
                options.Value = JsonValue.Create(rawValue);
            }
            options.Save = !args.Flag("--no-save");
            break;
        default:
            throw CommandLine.Usage($"Unknown assets subcommand '{subcommand}'.");
        }
        args.Complete();

        var engine = resolver.Resolve(project, context.Options.Engine);
        var invocation = await editorAdapter.AssetAsync(engine, project, options, context.Options.PassThrough, context);
        if (!invocation.Structured || invocation.Result == null)
        {
            var details = new { operation = "asset", action = subcommand, engine = EngineView(engine), invocation.Process.ProcessId, invocation.Process.ExitCode, stdout = invocation.Process.StandardOutput, stderr = invocation.Process.StandardError };
            return CliResult.Fail(ExitCode.ContextRequired, "FLX-ASSET-PROTOCOL-0004", "The selected Editor does not support typed asset requests. Rebuild it with the CLI request service.", details);
        }

        var result = invocation.Result.Value;
        var succeeded = result.TryGetProperty("success", out var success) && success.GetBoolean();
        if (!succeeded || invocation.Process.ExitCode != 0)
        {
            var details = new { operation = "asset", action = subcommand, engine = EngineView(engine), invocation.Process.ProcessId, invocation.Process.ExitCode, result, events = invocation.Events, stdout = invocation.Process.StandardOutput, stderr = invocation.Process.StandardError };
            return WithEvents(CliResult.Fail(ExitCode.OperationFailed, "FLX-ASSET-0006", $"Asset {subcommand} failed.", details), invocation.Events);
        }
        return WithEvents(CliResult.Ok(result.TryGetProperty("data", out var data) ? data.Clone() : result.Clone()), invocation.Events);
    }

    private async Task<CliResult> Commands(CommandArguments args, CommandContext context)
    {
        var instance = args.Option("--instance");
        var liveOnly = args.Flag("--live-only");
        var oneShot = args.Flag("--one-shot");
        var action = args.Positional()?.ToLowerInvariant() ?? "list";
        var options = new EditorCommandRequestOptions { Action = action };
        switch (action)
        {
        case "list":
            break;
        case "info":
            options.Name = args.Positional() ?? throw CommandLine.Usage("commands info requires a command name.");
            break;
        default:
            throw CommandLine.Usage($"Unknown commands subcommand '{action}'.");
        }
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("commands list/info do not accept arguments after '--'.");

        return await ExecuteEditorCommand(options, context, instance, liveOnly, oneShot);
    }

    private async Task<CliResult> Command(CommandArguments args, CommandContext context)
    {
        var name = args.Positional() ?? throw CommandLine.Usage("command requires a command name.");
        var rawArguments = args.Option("--arguments");
        var inputPath = args.Option("--input");
        var confirm = args.Flag("--yes");
        var instance = args.Option("--instance");
        var liveOnly = args.Flag("--live-only");
        var oneShot = args.Flag("--one-shot");
        if (rawArguments != null && inputPath != null)
            throw CommandLine.Usage("command accepts either --arguments or --input, not both.");
        if (inputPath != null)
        {
            inputPath = Path.GetFullPath(inputPath);
            if (!File.Exists(inputPath))
                throw new CliException(ExitCode.ContextRequired, "FLX-COMMAND-INPUT-0004", $"Command input file '{inputPath}' does not exist.");
            rawArguments = File.ReadAllText(inputPath);
        }

        var remaining = args.TakeRemaining();
        remaining.AddRange(context.Options.PassThrough);
        var arguments = ParseCommandArguments(rawArguments, remaining);
        var options = new EditorCommandRequestOptions
        {
            Action = "invoke",
            Name = name,
            Arguments = arguments,
            Confirm = confirm,
        };
        return await ExecuteEditorCommand(options, context, instance, liveOnly, oneShot);
    }

    private async Task<CliResult> Authoring(string group, CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage($"{group} requires a subcommand.");
        if ((group == "actors" && action == "component") || (group == "scenes" && action == "build-list"))
        {
            var nested = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage($"{group} {action} requires a subcommand.");
            action += "." + nested;
        }
        var commandArgs = new List<string> { AuthoringCommandName(group, action) };
        commandArgs.AddRange(args.TakeRemaining());
        return await Command(new CommandArguments(commandArgs), context);
    }

    internal static string AuthoringCommandName(string group, string action) => group + "." + action;

    private async Task<CliResult> ExecuteEditorCommand(EditorCommandRequestOptions options, CommandContext context, string? instanceSelector, bool liveOnly, bool oneShot)
    {
        if (liveOnly && oneShot)
            throw CommandLine.Usage("--live-only and --one-shot cannot be used together.");
        if (oneShot && instanceSelector != null)
            throw CommandLine.Usage("--instance cannot be combined with --one-shot.");
        var projectPath = ResolveOptionalProject(context.Options.Project);
        if (!oneShot)
        {
            var instance = bridgeClient.Select(projectPath, instanceSelector, liveOnly || instanceSelector != null);
            if (instance != null)
            {
                var action = options.Action switch
                {
                    "list" => "commands.list",
                    "info" => "commands.info",
                    "invoke" => "command.invoke",
                    _ => throw CommandLine.Usage($"Unsupported typed command action '{options.Action}'."),
                };
                var liveInvocation = await bridgeClient.InvokeAsync(instance, action, options.Name, options.Arguments, options.Confirm, context);
                return BridgeResult(liveInvocation, action);
            }
        }
        if (liveOnly)
            throw new CliException(ExitCode.ContextRequired, "FLX-BRIDGE-NOTFOUND-0004", "No compatible running Flax Editor instance matched the project.");

        var project = FindProject(null, context.Options.Project);
        var engine = resolver.Resolve(project, context.Options.Engine);
        var oneShotInvocation = await editorAdapter.CommandAsync(engine, project, options, context);
        if (!oneShotInvocation.Structured || oneShotInvocation.Result == null)
        {
            var details = new { operation = "command", action = options.Action, name = options.Name, engine = EngineView(engine), oneShotInvocation.Process.ProcessId, oneShotInvocation.Process.ExitCode, stdout = oneShotInvocation.Process.StandardOutput, stderr = oneShotInvocation.Process.StandardError };
            return CliResult.Fail(ExitCode.ContextRequired, "FLX-COMMAND-PROTOCOL-0004", "The selected Editor does not support typed command requests. Rebuild it with the CLI command service.", details);
        }

        var result = oneShotInvocation.Result.Value;
        var succeeded = result.TryGetProperty("success", out var success) && success.GetBoolean();
        if (!succeeded || oneShotInvocation.Process.ExitCode != 0)
        {
            var details = new { operation = "command", action = options.Action, name = options.Name, engine = EngineView(engine), oneShotInvocation.Process.ProcessId, oneShotInvocation.Process.ExitCode, result, events = oneShotInvocation.Events, stdout = oneShotInvocation.Process.StandardOutput, stderr = oneShotInvocation.Process.StandardError };
            return WithEvents(CliResult.Fail(ExitCode.OperationFailed, "FLX-COMMAND-0006", $"Editor command {options.Action} failed.", details), oneShotInvocation.Events);
        }
        return WithEvents(CliResult.Ok(result.TryGetProperty("data", out var data) ? data.Clone() : result.Clone()), oneShotInvocation.Events);
    }

    private async Task<CliResult> Editor(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "status";
        var instanceSelector = args.Option("--instance");
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("editor commands do not accept arguments after '--'.");
        var bridgeAction = action switch
        {
            "status" => "editor.status",
            "play" => "editor.play",
            "pause" => "editor.pause",
            "resume" => "editor.resume",
            "stop" => "editor.stop",
            "step" => "editor.step",
            "focus" => "editor.focus",
            "save-all" or "save" => "editor.saveAll",
            "recompile" => "editor.recompile",
            _ => throw CommandLine.Usage($"Unknown editor subcommand '{action}'."),
        };
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, required: true)!;
        return BridgeResult(await bridgeClient.InvokeAsync(instance, bridgeAction, null, null, false, context), bridgeAction);
    }

    private async Task<CliResult> ConsoleLogs(CommandArguments args, CommandContext context)
    {
        var instanceSelector = args.Option("--instance");
        var cursorText = args.Option("--cursor");
        var limitText = args.Option("--limit");
        var level = args.Option("--level");
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("console does not accept arguments after '--'.");
        if (cursorText != null && (!long.TryParse(cursorText, out var cursor) || cursor < 0))
            throw CommandLine.Usage("console --cursor must be a non-negative integer.");
        if (limitText != null && (!int.TryParse(limitText, out var limit) || limit < 1 || limit > 1000))
            throw CommandLine.Usage("console --limit must be between 1 and 1000.");
        var arguments = new JsonObject();
        if (cursorText != null)
            arguments["cursor"] = long.Parse(cursorText);
        if (limitText != null)
            arguments["limit"] = int.Parse(limitText);
        if (level != null)
            arguments["level"] = level;
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, required: true)!;
        return BridgeResult(await bridgeClient.InvokeAsync(instance, "console", null, arguments, false, context), "console");
    }

    private static CliResult BridgeResult(EditorBridgeInvocation invocation, string action)
    {
        var response = invocation.Response;
        var succeeded = response.TryGetProperty("success", out var success) && success.GetBoolean();
        if (!succeeded)
        {
            var code = "FLX-BRIDGE-0006";
            var message = $"Live Editor action '{action}' failed.";
            if (response.TryGetProperty("errors", out var errors) && errors.ValueKind == JsonValueKind.Array && errors.GetArrayLength() != 0)
            {
                var first = errors.EnumerateArray().First();
                if (first.TryGetProperty("code", out var errorCode) && !string.IsNullOrWhiteSpace(errorCode.GetString()))
                    code = errorCode.GetString()!;
                if (first.TryGetProperty("message", out var errorMessage) && !string.IsNullOrWhiteSpace(errorMessage.GetString()))
                    message = errorMessage.GetString()!;
            }
            return WithBridgeEvents(CliResult.Fail(ExitCode.OperationFailed, code, message, new { instance = EditorBridgeClient.View(invocation.Instance), response }), response);
        }

        var result = CliResult.Ok(response.TryGetProperty("data", out var data) ? data.Clone() : response.Clone());
        if (response.TryGetProperty("warnings", out var warnings) && warnings.ValueKind == JsonValueKind.Array)
        {
            foreach (var warning in warnings.EnumerateArray())
            {
                var code = warning.TryGetProperty("code", out var warningCode) ? warningCode.GetString() ?? "FLX-BRIDGE-W001" : "FLX-BRIDGE-W001";
                var message = warning.TryGetProperty("message", out var warningMessage) ? warningMessage.GetString() ?? "Live Editor warning." : "Live Editor warning.";
                result.Warnings.Add(new CliMessage(code, message, warning.Clone()));
            }
        }
        return WithBridgeEvents(result, response);
    }

    private static CliResult WithEvents(CliResult result, IEnumerable<JsonElement> events)
    {
        result.Events.AddRange(events.Select(x => (object)x.Clone()));
        return result;
    }

    private static CliResult WithBridgeEvents(CliResult result, JsonElement response)
    {
        if (response.TryGetProperty("events", out var events) && events.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in events.EnumerateArray())
                result.Events.Add(item.Clone());
        }
        return result;
    }

    internal static JsonObject ParseCommandArguments(string? rawArguments, IReadOnlyList<string> tokens)
    {
        JsonObject result;
        if (rawArguments == null)
        {
            result = new JsonObject();
        }
        else
        {
            try
            {
                result = JsonNode.Parse(rawArguments) as JsonObject
                         ?? throw CommandLine.Usage("Command arguments must be a JSON object.");
            }
            catch (JsonException ex)
            {
                throw CommandLine.Usage($"Command arguments are invalid JSON: {ex.Message}");
            }
        }

        for (var index = 0; index < tokens.Count; index++)
        {
            var token = tokens[index];
            if (!token.StartsWith("--", StringComparison.Ordinal) || token.Length == 2)
                throw CommandLine.Usage($"Unexpected command argument '{token}'. Typed arguments must use --name [value].");

            var option = token[2..];
            string name;
            JsonNode? value;
            var separator = option.IndexOf('=');
            if (separator >= 0)
            {
                name = option[..separator];
                value = ParseCommandArgumentValue(option[(separator + 1)..]);
            }
            else
            {
                name = option;
                if (index + 1 < tokens.Count && !tokens[index + 1].StartsWith("--", StringComparison.Ordinal))
                    value = ParseCommandArgumentValue(tokens[++index]);
                else
                    value = JsonValue.Create(true);
            }
            if (string.IsNullOrWhiteSpace(name))
                throw CommandLine.Usage("Typed command argument names cannot be empty.");
            AddCommandArgument(result, name, value);
        }
        return result;
    }

    private static JsonNode? ParseCommandArgumentValue(string value)
    {
        try
        {
            return JsonNode.Parse(value);
        }
        catch (JsonException)
        {
            return JsonValue.Create(value);
        }
    }

    private static void AddCommandArgument(JsonObject arguments, string name, JsonNode? value)
    {
        var existingName = arguments.Select(x => x.Key).FirstOrDefault(x => string.Equals(x, name, StringComparison.OrdinalIgnoreCase));
        if (existingName == null)
        {
            arguments[name] = value;
            return;
        }

        var existing = arguments[existingName];
        if (existing is JsonArray array)
        {
            array.Add(value);
            return;
        }

        arguments.Remove(existingName);
        arguments[existingName] = new JsonArray(existing, value);
    }

    private CliResult Doctor(CommandArguments args, CommandContext context)
    {
        args.Complete();
        var checks = new List<object>();
        checks.Add(Check("launcher-registry", File.Exists(paths.VersionsFile), paths.VersionsFile));
        var registered = engines.List();
        checks.Add(Check("registered-engines", registered.Count != 0, $"{registered.Count} registered"));
        foreach (var engine in registered)
            checks.Add(Check($"engine:{engine.Id}", engine.IsValid && engine.BuildToolPath != null, engine.Path));
        try
        {
            var project = FindProject(null, context.Options.Project);
            checks.Add(Check("project", true, project.ProjectFile));
            try
            {
                var selected = resolver.Resolve(project, context.Options.Engine);
                checks.Add(Check("engine-resolution", true, selected.Path));
            }
            catch (CliException ex)
            {
                checks.Add(Check("engine-resolution", false, ex.Message));
            }
        }
        catch (CliException ex)
        {
            checks.Add(Check("project", false, ex.Message));
        }
        var failed = checks.Count(x => (bool)x.GetType().GetProperty("ok")!.GetValue(x)! == false);
        var result = failed == 0 ? CliResult.Ok(checks) : CliResult.Fail(ExitCode.ContextRequired, "FLX-DOCTOR-0004", $"{failed} health check(s) failed.", new { checks });
        if (failed != 0)
            result.Warnings.Add(new CliMessage("FLX-DOCTOR-W001", "Doctor is diagnostic only and did not change local state."));
        return result;
    }

    private CliResult Logs(CommandArguments args, CommandContext context)
    {
        var projectOnly = args.Flag("--project-only");
        var projectInput = args.Positional();
        args.Complete();
        var files = new List<string>();
        if (!projectOnly && Directory.Exists(paths.LauncherDirectory))
            files.AddRange(Directory.EnumerateFiles(paths.LauncherDirectory, "*.log", SearchOption.TopDirectoryOnly));
        try
        {
            var project = FindProject(projectInput, context.Options.Project);
            foreach (var directory in new[] { Path.Combine(project.Root, "Logs"), Path.Combine(project.Root, "Cache") })
                if (Directory.Exists(directory))
                    files.AddRange(Directory.EnumerateFiles(directory, "*.log", SearchOption.TopDirectoryOnly));
        }
        catch (CliException) when (!projectOnly && projectInput == null && context.Options.Project == null)
        {
        }
        return CliResult.Ok(files.Distinct(ProjectRegistry.PathComparer).Select(x => new { path = x, size = new FileInfo(x).Length, modified = File.GetLastWriteTimeUtc(x) }).ToArray());
    }

    private CliResult EnvironmentInfo(CommandArguments args, CommandContext context)
    {
        args.Complete();
        return CliResult.Ok(new
        {
            project = context.Options.Project,
            engine = context.Options.Engine,
            format = context.Options.Format,
            nonInteractive = context.Options.NonInteractive,
            paths = new { paths.ConfigDirectory, paths.StateDirectory, paths.CacheDirectory, paths.RuntimeDirectory, paths.VersionsFile, paths.ProjectsFile },
            host = new { os = System.Runtime.InteropServices.RuntimeInformation.OSDescription, architecture = System.Runtime.InteropServices.RuntimeInformation.OSArchitecture.ToString(), dotnet = System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription },
        });
    }

    private CliResult Config(CommandArguments args, CommandContext context)
    {
        var subcommand = args.Positional()?.ToLowerInvariant() ?? "list";
        var scope = (args.Option("--scope") ?? "user").ToLowerInvariant();
        if (scope is not ("user" or "project"))
            throw CommandLine.Usage("config --scope must be 'user' or 'project'.");
        var projectScope = scope == "project";
        string? projectFile = projectScope ? FindProject(null, context.Options.Project).ProjectConfigFile : null;
        switch (subcommand)
        {
        case "list":
            args.Complete();
            return CliResult.Ok(config.Load(projectFile));
        case "get":
            var getKey = args.Positional() ?? throw CommandLine.Usage("config get requires a key.");
            args.Complete();
            return CliResult.Ok(new { key = getKey, value = config.Get(getKey, projectFile) });
        case "set":
            var setKey = args.Positional() ?? throw CommandLine.Usage("config set requires a key.");
            var setValue = args.Positional() ?? throw CommandLine.Usage("config set requires a value.");
            args.Complete();
            return CliResult.Ok(config.Set(setKey, setValue, unset: false, projectFile));
        case "unset":
            var unsetKey = args.Positional() ?? throw CommandLine.Usage("config unset requires a key.");
            args.Complete();
            return CliResult.Ok(config.Set(unsetKey, null, unset: true, projectFile));
        default:
            throw CommandLine.Usage($"Unknown config subcommand '{subcommand}'.");
        }
    }

    private CliResult Status(CommandArguments args, CommandContext context)
    {
        args.Complete();
        var projectPath = context.Options.Project == null ? null : FindProject(null, context.Options.Project).Root;
        var bridged = bridgeClient.Discover()
            .Where(x => projectPath == null || ProjectRegistry.PathComparer.Equals(
                Path.GetFullPath(x.ProjectPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                projectPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)))
            .ToArray();
        var bridgedPids = bridged.Select(x => x.Pid).ToHashSet();
        var legacy = Process.GetProcesses().Where(x => x.ProcessName.Contains("FlaxEditor", StringComparison.OrdinalIgnoreCase) && !bridgedPids.Contains(x.Id)).Select(x =>
        {
            try { return new { pid = x.Id, process = x.ProcessName, started = (DateTime?)x.StartTime.ToUniversalTime(), bridge = false }; }
            catch { return new { pid = x.Id, process = x.ProcessName, started = (DateTime?)null, bridge = false }; }
        }).ToArray();
        return CliResult.Ok(new
        {
            project = projectPath,
            instances = bridged.Select(EditorBridgeClient.View).ToArray(),
            unbridgedProcesses = legacy,
            protocolVersion = "1.0",
            capabilities = bridged.SelectMany(x => x.Capabilities).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(x => x).ToArray(),
        });
    }

    private static CliResult Completion(CommandArguments args)
    {
        var shell = args.Positional()?.ToLowerInvariant() ?? "powershell";
        args.Complete();
        return shell switch
        {
            "powershell" or "pwsh" => CliResult.Ok("Register-ArgumentCompleter -Native -CommandName flax -ScriptBlock { param($wordToComplete) 'engines','engine','projects','open','play','generate','compile','build','assets','scenes','actors','prefabs','commands','command','editor','console','doctor','logs','env','config','status' | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object { $_ } }"),
            "bash" => CliResult.Ok("complete -W 'engines engine projects open play generate compile build assets scenes actors prefabs commands command editor console doctor logs env config status' flax"),
            _ => throw CommandLine.Usage($"Unsupported completion shell '{shell}'."),
        };
    }

    private static CliResult Deferred(string command) =>
        CliResult.Fail(ExitCode.ContextRequired, "FLX-FEED-0004", $"'{command}' requires the signed release/catalog contract that is still an open decision in the technical design.");

    private static CliResult ChildResult(string operation, EngineInfo engine, ProcessResult process, object? additional = null)
    {
        var data = new { operation, engine = EngineView(engine), process.ProcessId, process.ExitCode, stdout = process.StandardOutput, stderr = process.StandardError, details = additional };
        return process.ExitCode == 0
            ? CliResult.Ok(data)
            : CliResult.Fail(ExitCode.OperationFailed, "FLX-BUILD-0006", $"The {operation} child process exited with code {process.ExitCode}.", data);
    }

    private static object EngineView(EngineInfo engine, bool includeCapabilities = false) => new
    {
        engine.Id,
        engine.Version,
        engine.Nickname,
        engine.Path,
        engine.Source,
        engine.Channel,
        engine.Fingerprint,
        engine.IsDefault,
        engine.IsValid,
        engine.EditorPath,
        engine.BuildToolPath,
        capabilities = includeCapabilities ? new
        {
            legacyEditorArgs = engine.EditorPath != null,
            oneShotRequests = (bool?)null,
            typedCommands = (bool?)null,
            eventProtocol = (string?)null,
            editorBridge = (string?)null,
            platformPackages = false,
        } : null,
    };

    private static object ProjectView(ProjectContext project) => new
    {
        project.Name,
        path = project.Root,
        projectFile = project.ProjectFile,
        minimumEngineVersion = project.MinimumEngineVersion.ToString(),
        project.EngineNickname,
        engineLock = File.Exists(project.LockFile) ? project.LockFile : null,
        exists = true,
    };

    private static object ProjectViewSafe(string path)
    {
        try { return ProjectView(ProjectContext.Find(path)); }
        catch (CliException ex) { return new { name = Path.GetFileName(path), path, projectFile = (string?)null, minimumEngineVersion = (string?)null, engineNickname = (string?)null, engineLock = (string?)null, exists = Directory.Exists(path), error = ex.Message }; }
    }

    private static object Check(string name, bool ok, string detail) => new { name, ok, detail };

    private static ProjectContext FindProject(string? positional, string? global) => ProjectContext.Find(positional ?? global ?? Environment.GetEnvironmentVariable("FLAX_PROJECT"));

    private static string? ResolveOptionalProject(string? global)
    {
        try
        {
            return FindProject(null, global).Root;
        }
        catch (CliException) when (global == null)
        {
            return null;
        }
    }

    internal static string AssetPath(ProjectContext project, string path)
    {
        if (Path.IsPathRooted(path))
            return Path.GetFullPath(path);
        var normalized = path.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
        if (normalized.Equals("Content", StringComparison.OrdinalIgnoreCase) || normalized.StartsWith("Content" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            return Path.GetFullPath(normalized, project.Root);
        return Path.GetFullPath(normalized, Path.Combine(project.Root, "Content"));
    }

    private static string Help(string? command) => command?.ToLowerInvariant() switch
    {
        "engines" => "flax engines <list|add|remove|default|info> [arguments]",
        "engine" => "flax engine <pin|unpin> [selector] [--project path]",
        "projects" => "flax projects <list|add|remove|info|size> [path]",
        "generate" => "flax generate [project] [--ide rider|vscode|vs2022|vs2026] [--build-arg value]",
        "compile" => "flax compile [project] [--target name] [--configuration value] [--platform value] [--arch value]",
        "build" => "flax build [project] --preset name --target platform [--output path] [--define value] [--clean] [--run]",
        "assets" or "asset" => AssetsHelp,
        "authoring-root" => "flax authoring-root <get|set> [path] [--project path]",
        "scenes" or "actors" or "prefabs" => AuthoringHelp,
        "commands" => "flax commands <list|info> [name] [--project path]",
        "command" => CommandHelp,
        "editor" => "flax editor <status|play|pause|resume|stop|step|focus|save-all|recompile> [--project path] [--instance id|pid]",
        "console" => "flax console [--project path] [--instance id|pid] [--cursor number] [--limit 1..1000] [--level value]",
        _ => HelpText,
    };

    private const string AssetsHelp = """
flax assets list [path] [--recursive]
flax assets types [folder]
flax assets info <path>
flax assets create <type> <path>
flax assets mkdir <path>
flax assets import <source...> --to <folder>
flax assets duplicate <source> <destination>
flax assets move <source> <destination>
flax assets rename <source> <name>
flax assets delete <path> --yes
flax assets reimport <path>
flax assets export <path> --to <folder>
flax assets get <path> <property.path>
flax assets set <path> <property.path> <json-value> [--no-save]
flax assets set <path> <property.path> --value <json-value> [--no-save]
flax assets save <path>

Relative asset paths are resolved under the project's Content folder.
""";

    private const string CommandHelp = """
flax commands list [--project path] [--instance id|pid] [--live-only|--one-shot]
flax commands info <name> [--project path] [--instance id|pid] [--live-only|--one-shot]
flax command <name> [--project path] [--instance id|pid] [--live-only|--one-shot] [--arguments <json> | --input <file.json>] [--yes]
flax command <name> [--project path] [--instance id|pid] -- --option value [--flag]

Typed project commands prefer a matching running Editor and fall back to a one-shot headless Editor. Repeated options become JSON arrays.
""";

    private const string AuthoringHelp = """
flax scenes <list|create|open|save|hierarchy> [options]
flax actors <find|get|create|create-batch|delete|rename|transform|parent|active|tag|layer> [options]
flax actors component <add|remove|get|set> [options]
flax prefabs <create|instantiate|variant|apply|revert|unpack|save> [options]

Use flax commands info <dotted-name> for each command's typed option schema.
""";

    private const string HelpText = """
Flax CLI 0.1

Usage: flax [global options] <command> [arguments] [options]

Local engine commands:
  engines list|add|remove|default|info
  engine pin|unpin
  projects list|add|remove|info|size
  open, play, generate, compile, clean, build
  assets list|types|info|create|mkdir|import|duplicate|move|rename|delete|reimport|export|get|set|save
  authoring-root get|set
  scenes list|create|open|save|hierarchy
  actors find|get|create|create-batch|delete|rename|transform|parent|active|tag|layer|component
  prefabs create|instantiate|variant|apply|revert|unpack|save
  commands list|info, command <name>
  editor status|play|pause|resume|stop|step|focus|save-all|recompile, console
  doctor, logs, env, config, status, completion

Global options:
  --project <path>       Select the project context
  --engine <selector>   Override engine resolution
  --format <format>     human, tsv, json, or ndjson
  --json                Alias for --format json
  --non-interactive     Never prompt for missing context
  --timeout <seconds>   Graceful child shutdown timeout
  --quiet, --verbose, --no-color, --trace
  --help, -h            Show contextual help
  --version, -V         Show CLI and protocol versions
  --                    Pass remaining arguments to the selected adapter or typed command
""";
}
