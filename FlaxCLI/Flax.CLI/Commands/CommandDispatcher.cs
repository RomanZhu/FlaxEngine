// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
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
    TestAdapter testAdapter,
    EditorAdapter editorAdapter,
    EditorBridgeClient bridgeClient)
{
    private readonly JobStore _jobs = new(paths);
    private readonly SignedFeedService _feeds = new();

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
            "projects" => Projects(args, context),
            "open" => Open(args, context, false),
            "play" => Open(args, context, true),
            "generate" => await Generate(args, context),
            "compile" => await Compile(args, context, false),
            "clean" => await Compile(args, context, true),
            "build" => await Build(args, context),
            "assets" or "asset" => await Assets(args, context),
            "authoring-root" => AuthoringRoot(args, context),
            "doctor" => Doctor(args, context),
            "diagnose" => Diagnose(args, context),
            "logs" => Logs(args, context),
            "env" => EnvironmentInfo(args, context),
            "config" => Config(args, context),
            "status" => Status(args, context),
            "editor" => await Editor(args, context),
            "console" => await ConsoleLogs(args, context),
            "performance" => await Performance(args, context),
            "selection" => await Selection(args, context),
            "capture" => await Capture(args, context),
            "playtest" => await Playtest(args, context),
            "mcp" => await Mcp(args, context),
            "commands" => await Commands(args, context),
            "command" => await Command(args, context),
            "generators" or "generator" => await Generators(args, context),
            "jobs" => await Jobs(args, context),
            "feeds" => Feeds(args, context),
            "player" => await Player(args, context),
            "runtime" => await Runtime(args, context),
            "scenes" or "actors" or "prefabs" or "settings" or "bake" or "dev" or "visject" => await Authoring(command, args, context),
            "templates" => Templates(args),
            "new" => CreateProject(args, context),
            "test" => await Tests(args, context),
            "install" or "uninstall" or "releases" or "platforms" or "upgrade" => Deferred(command),
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

    private CliResult Projects(CommandArguments args, CommandContext context)
    {
        var subcommand = args.Positional()?.ToLowerInvariant() ?? "list";
        switch (subcommand)
        {
        case "create":
            return CreateProject(args, context);
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

    private CliResult CreateProject(CommandArguments args, CommandContext context)
    {
        var pathOption = args.Option("--path");
        var nameOption = args.Option("--name");
        var template = args.Option("--template") ?? "empty";
        var minVersion = args.Option("--min-engine-version");
        var input = pathOption ?? args.Positional() ?? throw CommandLine.Usage("projects create requires a project path.");
        args.Complete();
        if (!template.Equals("empty", StringComparison.OrdinalIgnoreCase))
            throw new CliException(ExitCode.ContextRequired, "FLX-TEMPLATE-0004", $"Template '{template}' is not installed locally. Signed template feeds remain deferred.");

        var root = Path.GetFullPath(input);
        if (File.Exists(root))
            throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"Project path '{root}' is a file.");
        if (Directory.Exists(root))
        {
            if (Directory.EnumerateFileSystemEntries(root).Any())
                throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"Project directory '{root}' is not empty.");
        }
        else
        {
            Directory.CreateDirectory(root);
        }

        var name = string.IsNullOrWhiteSpace(nameOption) ? new DirectoryInfo(root).Name : nameOption.Trim();
        if (name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new CliException(ExitCode.Usage, "FLX-CLI-0002", $"Project name '{name}' contains invalid file-name characters.");
        if (!SemanticVersion.TryParse(minVersion, out var parsedVersion))
            parsedVersion = new SemanticVersion(1, 0, 0);
        var projectFile = CreateEmptyProjectScaffold(root, name, parsedVersion);
        var project = ProjectContext.Find(projectFile);
        projects.Add(project.Root);
        return CliResult.Ok(new { created = true, template = "empty", project = ProjectView(project) });
    }

    internal static string CreateEmptyProjectScaffold(string root, string name, SemanticVersion minimumEngineVersion)
    {
        var codeName = GetProjectCodeName(name);
        var gameTarget = codeName + "Target";
        var editorTarget = codeName + "EditorTarget";
        var editorModule = codeName + "Editor";
        var projectFile = Path.Combine(root, name + ".flaxproj");
        var projectJson = new JsonObject
        {
            ["Name"] = name,
            ["Version"] = new JsonObject { ["Major"] = 1, ["Minor"] = 0, ["Revision"] = 0, ["Build"] = 0 },
            ["Company"] = string.Empty,
            ["Copyright"] = string.Empty,
            ["References"] = new JsonArray(new JsonObject { ["Name"] = "$(EnginePath)/Flax.flaxproj" }),
            ["GameTarget"] = gameTarget,
            ["EditorTarget"] = editorTarget,
            ["MinEngineVersion"] = minimumEngineVersion.ToString(),
        };
        File.WriteAllText(projectFile, projectJson.ToJsonString(new JsonSerializerOptions(JsonSupport.Options) { WriteIndented = true }) + Environment.NewLine);
        Directory.CreateDirectory(Path.Combine(root, "Content"));
        var source = Path.Combine(root, "Source");
        var module = Path.Combine(source, codeName);
        var editorModulePath = Path.Combine(source, editorModule);
        Directory.CreateDirectory(module);
        Directory.CreateDirectory(editorModulePath);
        Directory.CreateDirectory(Path.Combine(root, ".flax"));

        WriteSourceFile(Path.Combine(source, gameTarget + ".Build.cs"),
            "using Flax.Build;",
            "",
            $"public class {gameTarget} : GameProjectTarget",
            "{",
            "    public override void Init()",
            "    {",
            "        base.Init();",
            $"        Modules.Add(\"{codeName}\");",
            "    }",
            "}");
        WriteSourceFile(Path.Combine(source, editorTarget + ".Build.cs"),
            "using Flax.Build;",
            "",
            $"public class {editorTarget} : GameProjectEditorTarget",
            "{",
            "    public override void Init()",
            "    {",
            "        base.Init();",
            $"        Modules.Add(\"{codeName}\");",
            $"        Modules.Add(\"{editorModule}\");",
            "    }",
            "}");
        WriteSourceFile(Path.Combine(module, codeName + ".Build.cs"),
            "using Flax.Build;",
            "using Flax.Build.NativeCpp;",
            "",
            $"public class {codeName} : GameModule",
            "{",
            "    public override void Init()",
            "    {",
            "        base.Init();",
            "        BuildNativeCode = false;",
            "    }",
            "",
            "    public override void Setup(BuildOptions options)",
            "    {",
            "        base.Setup(options);",
            "        options.ScriptingAPI.IgnoreMissingDocumentationWarnings = true;",
            "    }",
            "}");
        WriteSourceFile(Path.Combine(editorModulePath, editorModule + ".Build.cs"),
            "using Flax.Build;",
            "using Flax.Build.NativeCpp;",
            "",
            $"public class {editorModule} : GameEditorModule",
            "{",
            "    public override void Init()",
            "    {",
            "        base.Init();",
            "        BuildNativeCode = false;",
            "    }",
            "",
            "    public override void Setup(BuildOptions options)",
            "    {",
            "        base.Setup(options);",
            "        options.ScriptingAPI.IgnoreMissingDocumentationWarnings = true;",
            "    }",
            "}");
        return projectFile;
    }

    internal static string GetProjectCodeName(string name)
    {
        var characters = name.Select(character => char.IsLetterOrDigit(character) || character == '_' ? character : '_').ToArray();
        var result = characters.Length == 0 ? "Game" : new string(characters);
        return char.IsLetter(result[0]) || result[0] == '_' ? result : "_" + result;
    }

    private static void WriteSourceFile(string path, params string[] lines)
    {
        File.WriteAllText(path, string.Join(Environment.NewLine, lines) + Environment.NewLine);
    }

    private static CliResult Templates(CommandArguments args)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "list";
        switch (action)
        {
        case "list":
            args.Complete();
            return CliResult.Ok(new
            {
                source = "local",
                feedBacked = false,
                templates = new[] { new { id = "empty", name = "Empty Flax project", installed = true, source = "flax-cli" } },
            });
        case "info":
            var id = args.Positional() ?? throw CommandLine.Usage("templates info requires a template ID.");
            args.Complete();
            if (!id.Equals("empty", StringComparison.OrdinalIgnoreCase))
                throw new CliException(ExitCode.ContextRequired, "FLX-TEMPLATE-0004", $"Template '{id}' is not installed locally.");
            return CliResult.Ok(new { id = "empty", name = "Empty Flax project", supports = new[] { "windows", "linux", "mac" }, feedBacked = false });
        default:
            throw CommandLine.Usage($"Unknown templates subcommand '{action}'.");
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
        if (args.Flag("--detach"))
        {
            var detachedProject = TryFindProject(context.Options.Project);
            return CliResult.Ok(new { operation = clean ? "clean" : "compile", detached = true, job = _jobs.Start(detachedProject, context.Options.OriginalArgs, detachedProject?.Root ?? Environment.CurrentDirectory) });
        }
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
        if (args.Flag("--detach"))
        {
            var detachedProject = TryFindProject(context.Options.Project);
            return CliResult.Ok(new { operation = "build", detached = true, job = _jobs.Start(detachedProject, context.Options.OriginalArgs, detachedProject?.Root ?? Environment.CurrentDirectory) });
        }
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
        var instance = args.Option("--instance");
        var liveOnly = args.Flag("--live-only");
        var oneShot = args.Flag("--one-shot");
        var verifyReload = args.Flag("--verify-reload");
        var confirm = args.Flag("--yes");

        if (subcommand == "batch")
        {
            var inputPath = Path.GetFullPath(args.Option("--input") ?? args.Positional() ?? throw CommandLine.Usage("assets batch requires --input <manifest.json>."));
            var continueOnError = args.Flag("--continue-on-error");
            args.Complete();
            if (context.Options.PassThrough.Count != 0)
                throw CommandLine.Usage("assets batch does not accept arguments after '--'.");
            var batch = ReadAssetBatch(inputPath);
            if (batch.SchemaVersion != 1)
                throw CommandLine.Usage($"Unsupported asset batch schema {batch.SchemaVersion}.");
            if (batch.Operations.Length == 0)
                throw CommandLine.Usage("The asset batch manifest contains no operations.");
            foreach (var operation in batch.Operations)
                NormalizeAssetOperation(project, operation, confirm);
            var batchArguments = new JsonObject
            {
                ["operations"] = JsonSerializer.SerializeToNode(batch.Operations, JsonSupport.Options),
                ["continue-on-error"] = continueOnError || batch.ContinueOnError,
                ["verify-reload"] = verifyReload || batch.VerifyReload,
            };
            return await ExecuteEditorCommand(new EditorCommandRequestOptions
            {
                Action = "invoke",
                Name = "assets.batch",
                Arguments = batchArguments,
                Confirm = confirm,
            }, context, instance, liveOnly, oneShot);
        }

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
            options.IfExists = args.Option("--if-exists") ?? "error";
            break;
        case "mkdir":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets mkdir requires a folder path."));
            options.IfExists = args.Option("--if-exists") ?? "error";
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
            options.Force = confirm;
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
        case "refresh":
            options.Path = AssetPath(project, args.Positional() ?? ".");
            options.Recursive = args.Flag("--recursive");
            break;
        case "verify":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets verify requires an asset path."));
            verifyReload = true;
            break;
        case "material-instance":
            options.Path = AssetPath(project, args.Positional() ?? throw CommandLine.Usage("assets material-instance requires an output path."));
            options.BaseMaterial = AssetPath(project, args.Option("--base-material") ?? throw CommandLine.Usage("assets material-instance requires --base-material <asset>."));
            options.IfExists = args.Option("--if-exists") ?? "error";
            options.Save = !args.Flag("--no-save");
            var rawParameters = args.Option("--parameters");
            var parametersFile = args.Option("--parameters-file");
            if (rawParameters != null && parametersFile != null)
                throw CommandLine.Usage("assets material-instance accepts either --parameters or --parameters-file, not both.");
            if (parametersFile != null)
                rawParameters = File.ReadAllText(Path.GetFullPath(parametersFile));
            if (rawParameters != null)
            {
                try
                {
                    options.Parameters = JsonNode.Parse(rawParameters) as JsonObject
                                         ?? throw CommandLine.Usage("Material parameters must be a JSON object.");
                }
                catch (JsonException ex)
                {
                    throw CommandLine.Usage($"Material parameters are invalid JSON: {ex.Message}");
                }
            }
            break;
        default:
            throw CommandLine.Usage($"Unknown assets subcommand '{subcommand}'.");
        }
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("typed asset commands do not accept arguments after '--'.");
        NormalizeAssetOperation(project, options, confirm);
        var commandArguments = new JsonObject
        {
            ["operation"] = JsonSerializer.SerializeToNode(options, JsonSupport.Options),
            ["verify-reload"] = verifyReload,
        };
        return await ExecuteAssetOperation(new EditorCommandRequestOptions
        {
            Action = "invoke",
            Name = "assets.execute",
            Arguments = commandArguments,
            Confirm = confirm,
        }, project, options, context, instance, liveOnly, oneShot);
    }

    private async Task<CliResult> ExecuteAssetOperation(EditorCommandRequestOptions command, ProjectContext project, AssetRequestOptions legacyOptions, CommandContext context, string? instance, bool liveOnly, bool oneShot)
    {
        var typed = await ExecuteEditorCommand(command, context, instance, liveOnly, oneShot);
        if (typed.ExitCode == ExitCode.Success || typed.Errors.Count == 0 || typed.Errors[0].Code != "FLX-COMMAND-NOTFOUND-0004" || liveOnly)
            return typed;

        // Compatibility for Editors built before assets.execute was added. New
        // Editors use the live-or-one-shot typed command above; old Editors retain
        // the original one-shot asset protocol for single operations.
        var engine = resolver.Resolve(project, context.Options.Engine);
        var invocation = await editorAdapter.AssetAsync(engine, project, legacyOptions, context.Options.PassThrough, context);
        if (!invocation.Structured || invocation.Result == null)
        {
            var details = new { operation = "asset", action = legacyOptions.Action, engine = EngineView(engine), invocation.Process.ProcessId, invocation.Process.ExitCode, stdout = invocation.Process.StandardOutput, stderr = invocation.Process.StandardError };
            return CliResult.Fail(ExitCode.ContextRequired, "FLX-ASSET-PROTOCOL-0004", "The selected Editor supports neither typed asset commands nor the legacy asset request protocol.", details);
        }
        var result = invocation.Result.Value;
        var succeeded = result.TryGetProperty("success", out var success) && success.GetBoolean();
        if (!succeeded || invocation.Process.ExitCode != 0)
        {
            var details = new { operation = "asset", action = legacyOptions.Action, engine = EngineView(engine), invocation.Process.ProcessId, invocation.Process.ExitCode, result, events = invocation.Events, stdout = invocation.Process.StandardOutput, stderr = invocation.Process.StandardError };
            return WithEvents(CliResult.Fail(ExitCode.OperationFailed, "FLX-ASSET-0006", $"Asset {legacyOptions.Action} failed.", details), invocation.Events);
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
        if (args.Flag("--detach"))
        {
            var project = TryFindProject(context.Options.Project);
            return CliResult.Ok(new { operation = "command", detached = true, job = _jobs.Start(project, context.Options.OriginalArgs, project?.Root ?? Environment.CurrentDirectory) });
        }
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

    private async Task<CliResult> Generators(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "list";
        if (action is "list" or "info")
        {
            var instance = args.Option("--instance");
            var liveOnly = args.Flag("--live-only");
            var oneShot = args.Flag("--one-shot");
            var options = new EditorCommandRequestOptions
            {
                Action = action == "list" ? "generator-list" : "generator-info",
                Name = action == "info" ? args.Positional() ?? throw CommandLine.Usage("generators info requires a generator name.") : null,
            };
            args.Complete();
            if (context.Options.PassThrough.Count != 0)
                throw CommandLine.Usage("generators list/info do not accept arguments after '--'.");
            return await ExecuteEditorCommand(options, context, instance, liveOnly, oneShot);
        }
        if (action != "run")
            throw CommandLine.Usage($"Unknown generators subcommand '{action}'.");

        if (args.Flag("--detach"))
        {
            var project = TryFindProject(context.Options.Project);
            return CliResult.Ok(new { operation = "generator", detached = true, job = _jobs.Start(project, context.Options.OriginalArgs, project?.Root ?? Environment.CurrentDirectory) });
        }

        var name = args.Positional() ?? throw CommandLine.Usage("generators run requires a generator name.");
        var rawArguments = args.Option("--arguments");
        var inputPath = args.Option("--input");
        var dryRun = args.Flag("--dry-run");
        var confirm = args.Flag("--yes");
        var instanceSelector = args.Option("--instance");
        var requireLive = args.Flag("--live-only");
        var requireOneShot = args.Flag("--one-shot");
        if (rawArguments != null && inputPath != null)
            throw CommandLine.Usage("generators run accepts either --arguments or --input, not both.");
        if (inputPath != null)
        {
            inputPath = Path.GetFullPath(inputPath);
            if (!File.Exists(inputPath))
                throw new CliException(ExitCode.ContextRequired, "FLX-GENERATOR-INPUT-0004", $"Generator input file '{inputPath}' does not exist.");
            rawArguments = File.ReadAllText(inputPath);
        }

        var remaining = args.TakeRemaining();
        remaining.AddRange(context.Options.PassThrough);
        var arguments = ParseCommandArguments(rawArguments, remaining);
        if (dryRun)
            arguments["dry-run"] = true;
        return await ExecuteEditorCommand(new EditorCommandRequestOptions
        {
            Action = "generator-invoke",
            Name = name,
            Arguments = arguments,
            Confirm = confirm,
        }, context, instanceSelector, requireLive, requireOneShot);
    }

    private async Task<CliResult> Authoring(string group, CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage($"{group} requires a subcommand.");
        if ((group == "actors" && (action == "component" || action == "primitive" || action == "property")) || (group == "scenes" && (action == "build-list" || action == "active")))
        {
            var nested = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage($"{group} {action} requires a subcommand.");
            action += "." + nested;
        }
        else if (group == "bake")
        {
            if (action == "status")
            {
                var statusArgs = new List<string> { AuthoringCommandName(group, action) };
                statusArgs.AddRange(args.TakeRemaining());
                return await Command(new CommandArguments(statusArgs), context);
            }
            var nested = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage("bake requires an operation group.");
            action += "." + nested;
            if (nested is "lighting" or "navmesh" or "probes" or "csg" or "scenes" or "sdf")
            {
                var operation = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage($"bake {nested} requires an operation.");
                action += "." + operation;
            }
        }
        else if (group == "visject")
        {
            // `groups`, `asset`, and `node` have a second verb. `validate`,
            // `connect`, and `disconnect` are complete command names and must
            // leave their first option (for example --asset) untouched.
            if (action is "groups" or "asset" or "node")
            {
                var nested = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage($"visject {action} requires an operation.");
                action += "." + nested;
            }
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
            var instance = bridgeClient.Select(projectPath, instanceSelector, liveOnly || instanceSelector != null, kind: "editor");
            if (instance != null)
            {
                var action = options.Action switch
                {
                    "list" => "commands.list",
                    "info" => "commands.info",
                    "invoke" => "command.invoke",
                    "generator-list" => "generators.list",
                    "generator-info" => "generators.info",
                    "generator-invoke" => "generator.invoke",
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
        JsonObject? actionArguments = null;
        if (action is "close" or "exit")
        {
            var save = args.Flag("--save");
            var discard = args.Flag("--discard");
            if (save == discard)
                throw CommandLine.Usage("editor close requires exactly one of --save or --discard.");
            actionArguments = new JsonObject { ["save"] = save };
        }
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
            "close" or "exit" => "editor.close",
            _ => throw CommandLine.Usage($"Unknown editor subcommand '{action}'."),
        };
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, required: true, kind: "editor")!;
        return BridgeResult(await bridgeClient.InvokeAsync(instance, bridgeAction, null, actionArguments, false, context), bridgeAction);
    }

    private async Task<CliResult> ConsoleLogs(CommandArguments args, CommandContext context)
    {
        var instanceSelector = args.Option("--instance");
        var cursorText = args.Option("--cursor");
        var limitText = args.Option("--limit");
        var level = args.Option("--level");
        var action = args.Positional()?.ToLowerInvariant() ?? "read";
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("console does not accept arguments after '--'.");
        if (action is not ("read" or "clear"))
            throw CommandLine.Usage($"Unknown console subcommand '{action}'.");
        if (action == "clear" && (cursorText != null || limitText != null || level != null))
            throw CommandLine.Usage("console clear does not accept --cursor, --limit, or --level.");
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
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, required: true, kind: "editor")!;
        var bridgeAction = action == "clear" ? "console.clear" : "console";
        return BridgeResult(await bridgeClient.InvokeAsync(instance, bridgeAction, null, arguments, false, context), bridgeAction);
    }

    private async Task<CliResult> Performance(CommandArguments args, CommandContext context)
    {
        var instanceSelector = args.Option("--instance");
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("performance does not accept arguments after '--'.");
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, required: true)!;
        return BridgeResult(await bridgeClient.InvokeAsync(instance, "performance", null, null, false, context), "performance");
    }

    private async Task<CliResult> Selection(CommandArguments args, CommandContext context)
    {
        var instanceSelector = args.Option("--instance");
        var actorIds = args.Options("--actor");
        var additive = args.Flag("--additive");
        var action = args.Positional()?.ToLowerInvariant() ?? "get";
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("selection does not accept arguments after '--'.");
        if (action is not ("get" or "set" or "clear"))
            throw CommandLine.Usage($"Unknown selection subcommand '{action}'.");
        if (action == "set" && actorIds.Count == 0)
            throw CommandLine.Usage("selection set requires at least one --actor <id>.");
        if (action != "set" && (actorIds.Count != 0 || additive))
            throw CommandLine.Usage("--actor and --additive are only valid with selection set.");
        foreach (var actorId in actorIds)
        {
            if (!Guid.TryParse(actorId, out _))
                throw CommandLine.Usage($"selection actor '{actorId}' is not a GUID.");
        }
        var arguments = new JsonObject();
        if (action == "set")
        {
            arguments["actors"] = new JsonArray(actorIds.Select(x => JsonValue.Create(x)).ToArray());
            arguments["additive"] = additive;
        }
        var bridgeAction = $"selection.{action}";
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, required: true, kind: "editor")!;
        return BridgeResult(await bridgeClient.InvokeAsync(instance, bridgeAction, null, arguments, false, context), bridgeAction);
    }

    private async Task<CliResult> Capture(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "viewport";
        var output = args.Option("--to") ?? throw CommandLine.Usage("capture requires --to <project-relative-or-absolute-path>.");
        var instanceSelector = args.Option("--instance");
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("capture does not accept arguments after '--'.");
        if (action is not ("viewport" or "game"))
            throw CommandLine.Usage("capture supports viewport or game.");
        var project = FindProject(null, context.Options.Project);
        var path = Path.GetFullPath(output, project.Root);
        var root = project.Root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (!path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw CommandLine.Usage("Capture output must remain under the selected project root.");
        if (string.IsNullOrWhiteSpace(Path.GetExtension(path)))
            path += ".png";
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var instance = bridgeClient.Select(project.Root, instanceSelector, required: true, kind: "editor")!;
        var bridgeAction = $"capture.{action}";
        var arguments = new JsonObject { ["path"] = path };
        return BridgeResult(await bridgeClient.InvokeAsync(instance, bridgeAction, null, arguments, false, context), bridgeAction);
    }

    private async Task<CliResult> Player(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "status";
        var instanceSelector = args.Option("--instance");
        // The Editor bridge owns the embedded Player while play mode is active.
        // A standalone development Player publishes its own compatible manifest.
        var instance = bridgeClient.Select(ResolveOptionalProject(context.Options.Project), instanceSelector, true);
        string bridgeAction;
        var arguments = new JsonObject();
        switch (action)
        {
        case "status": bridgeAction = "player.status"; break;
        case "pause": bridgeAction = "player.pause"; break;
        case "resume": bridgeAction = "player.resume"; break;
        case "step": bridgeAction = "player.step"; break;
        case "quit" or "close": bridgeAction = "player.quit"; break;
        case "input":
            var inputKind = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage("player input requires key, pointer, inspect, gamepad, action, or reset.");
            bridgeAction = inputKind switch { "key" => "runtime.input.key", "pointer" or "mouse" => "runtime.input.pointer", "inspect" or "sample" => "runtime.input.inspect", "gamepad" => "runtime.input.gamepad", "action" => "runtime.input.action", "reset" => "runtime.input.reset", _ => throw CommandLine.Usage($"Unknown player input kind '{inputKind}'.") };
            // Reuse the typed option parser so both `--name=value` and
            // `--name value` forms preserve booleans, numbers, and arrays.
            arguments = ParseCommandArguments(null, args.TakeRemaining());
            break;
        default: throw CommandLine.Usage($"Unknown player subcommand '{action}'.");
        }
        args.Complete();
        return BridgeResult(await bridgeClient.InvokeAsync(instance!, bridgeAction, null, arguments, action is "quit" or "close", context), bridgeAction);
    }

    private Task<CliResult> Runtime(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? throw CommandLine.Usage("runtime currently supports the input subcommand.");
        if (action != "input")
            throw CommandLine.Usage("runtime currently supports the input subcommand.");
        var forwarded = new List<string> { "input" };
        forwarded.AddRange(args.TakeRemaining());
        return Player(new CommandArguments(forwarded), context);
    }

    private async Task<CliResult> Jobs(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "list";
        if (action == "worker")
        {
            var recordPath = args.Option("--record") ?? throw CommandLine.Usage("jobs worker requires --record <path>.");
            args.Complete();
            var exitCode = await _jobs.RunWorkerAsync(Path.GetFullPath(recordPath), context.Options.PassThrough, context.CancellationToken).ConfigureAwait(false);
            return CliResult.Ok(new { worker = true, exitCode });
        }
        ProjectContext? project = null;
        try { project = FindProject(null, context.Options.Project); } catch (CliException) when (context.Options.Project == null) { }
        switch (action)
        {
        case "list": args.Complete(); return CliResult.Ok(_jobs.List(project));
        case "info" or "status":
            var infoId = args.Positional() ?? throw CommandLine.Usage($"jobs {action} requires a job id.");
            args.Complete(); return CliResult.Ok(_jobs.Require(infoId, project));
        case "cancel":
            var cancelId = args.Positional() ?? throw CommandLine.Usage("jobs cancel requires a job id.");
            var yes = args.Flag("--yes"); args.Complete();
            if (!yes) throw new CliException(ExitCode.Authorization, "FLX-JOB-CONFIRM-0004", "Cancelling a detached job requires --yes.");
            return CliResult.Ok(_jobs.Cancel(_jobs.Require(cancelId, project)));
        case "wait":
            var waitId = args.Positional() ?? throw CommandLine.Usage("jobs wait requires a job id.");
            var timeoutText = args.Option("--timeout-seconds"); args.Complete();
            var timeout = double.TryParse(timeoutText, out var parsed) && parsed > 0 ? TimeSpan.FromSeconds(Math.Min(parsed, 86400)) : context.Options.Timeout ?? TimeSpan.FromMinutes(30);
            var deadline = DateTime.UtcNow + timeout;
            while (DateTime.UtcNow < deadline)
            {
                var record = _jobs.Require(waitId, project);
                if (record.State is "succeeded" or "failed" or "cancelled") return CliResult.Ok(record);
                Thread.Sleep(100);
            }
            throw new CliException(ExitCode.OperationFailed, "FLX-JOB-TIMEOUT-0006", $"Detached job '{waitId}' did not finish before the timeout.");
        case "prune":
            var ageText = args.Option("--older-than-hours"); args.Complete();
            var hours = double.TryParse(ageText, out var age) && age >= 0 ? age : 168;
            _jobs.Prune(project, TimeSpan.FromHours(hours)); return CliResult.Ok(new { pruned = true, olderThanHours = hours });
        default: throw CommandLine.Usage($"Unknown jobs subcommand '{action}'.");
        }
    }

    private CliResult Feeds(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "verify";
        var manifestPath = args.Option("--manifest") ?? throw CommandLine.Usage("feeds requires --manifest <file>.");
        var manifest = _feeds.ReadManifest(Path.GetFullPath(manifestPath));
        var signature = args.Option("--signature") ?? throw CommandLine.Usage($"feeds {action} requires --signature <file>.");
        var publicKey = args.Option("--public-key") ?? throw CommandLine.Usage($"feeds {action} requires --public-key <file>.");
        var verification = _feeds.Verify(manifest, Path.GetFullPath(signature), Path.GetFullPath(publicKey));
        if (!verification.Valid) throw new CliException(ExitCode.Authorization, "FLX-FEED-SIGNATURE-0003", "The signed feed manifest could not be verified.", verification);
        switch (action)
        {
        case "verify": args.Complete(); return CliResult.Ok(new { verified = true, manifest = Path.GetFullPath(manifestPath), verification });
        case "list": args.Complete(); return CliResult.Ok(new { verified = true, entries = manifest["entries"] ?? new JsonArray(), verification });
        case "install":
            var entry = args.Option("--id") ?? throw CommandLine.Usage("feeds install requires --id <entry>.");
            var destination = args.Option("--to") ?? throw CommandLine.Usage("feeds install requires --to <directory>.");
            var confirm = args.Flag("--yes"); args.Complete();
            _feeds.Install(manifest, entry, Path.GetFullPath(destination), confirm);
            return CliResult.Ok(new { installed = true, entry, destination = Path.GetFullPath(destination), verification });
        default: throw CommandLine.Usage($"Unknown feeds subcommand '{action}'.");
        }
    }

    private async Task<CliResult> Playtest(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "status";
        var instanceSelector = args.Option("--instance");
        var projectPath = ResolveOptionalProject(context.Options.Project);
        var instance = bridgeClient.Select(projectPath, instanceSelector, required: true, kind: "editor")!;

        if (action == "capture")
        {
            var source = args.Positional()?.ToLowerInvariant() ?? "game";
            var output = args.Option("--to") ?? throw CommandLine.Usage("playtest capture requires --to <project-relative-or-absolute-path>.");
            args.Complete();
            if (context.Options.PassThrough.Count != 0)
                throw CommandLine.Usage("playtest capture does not accept arguments after '--'.");
            if (source is not ("viewport" or "game"))
                throw CommandLine.Usage("playtest capture supports viewport or game.");
            var project = FindProject(null, context.Options.Project);
            var path = Path.GetFullPath(output, project.Root);
            var root = project.Root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (!path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw CommandLine.Usage("Capture output must remain under the selected project root.");
            if (string.IsNullOrWhiteSpace(Path.GetExtension(path)))
                path += ".png";
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            return BridgeResult(await bridgeClient.InvokeAsync(instance, "capture." + source, null, new JsonObject { ["path"] = path }, false, context), "capture." + source);
        }

        if (action is "find" or "assert" or "wait")
        {
            var arguments = new JsonObject();
            var actor = args.Option("--actor");
            var name = args.Option("--name");
            var type = args.Option("--type");
            var active = args.Option("--active");
            var limit = args.Option("--limit");
            var timeoutSeconds = args.Option("--timeout-seconds");
            var exists = args.Option("--exists");
            if (actor != null)
            {
                if (!Guid.TryParse(actor, out _))
                    throw CommandLine.Usage("playtest --actor must be a GUID.");
                arguments["actor"] = actor;
            }
            if (name != null) arguments["name"] = name;
            if (type != null) arguments["type"] = type;
            if (active != null)
            {
                if (!bool.TryParse(active, out var activeValue))
                    throw CommandLine.Usage("playtest --active must be true or false.");
                arguments["active"] = activeValue;
            }
            if (limit != null)
            {
                if (!int.TryParse(limit, out var limitValue))
                    throw CommandLine.Usage("playtest --limit must be an integer.");
                arguments["limit"] = limitValue;
            }
            if (timeoutSeconds != null)
            {
                if (!double.TryParse(timeoutSeconds, out var timeoutValue))
                    throw CommandLine.Usage("playtest --timeout-seconds must be a number.");
                arguments["timeoutSeconds"] = timeoutValue;
            }
            if (exists != null)
            {
                if (!bool.TryParse(exists, out var existsValue))
                    throw CommandLine.Usage("playtest --exists must be true or false.");
                arguments["exists"] = existsValue;
            }
            args.Complete();
            if (context.Options.PassThrough.Count != 0)
                throw CommandLine.Usage("playtest observation commands do not accept arguments after '--'.");
            return await ExecuteEditorCommand(new EditorCommandRequestOptions
            {
                Action = "invoke",
                Name = "playtest." + action,
                Arguments = arguments,
            }, context, instanceSelector, liveOnly: true, oneShot: false);
        }

        if (action is not ("status" or "begin" or "start" or "pause" or "resume" or "step" or "end" or "stop"))
            throw CommandLine.Usage("playtest supports status, begin, pause, resume, step, find, assert, wait, capture, and end.");

        var scene = args.Option("--scene");
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("playtest control commands do not accept arguments after '--'.");

        if (action is "begin" or "start")
        {
            if (scene == null)
            {
                var current = await bridgeClient.InvokeAsync(instance, "editor.status", null, null, false, context);
                if (current.Response.TryGetProperty("data", out var currentData) && currentData.TryGetProperty("loadedScenes", out var loaded) && loaded.GetInt32() == 0)
                {
                    var build = await bridgeClient.InvokeAsync(instance, "command.invoke", "scenes.build-list.list", new JsonObject(), false, context);
                    var buildResult = BridgeResult(build, "scenes.build-list.list");
                    if (buildResult.ExitCode != ExitCode.Success)
                        return buildResult;
                    if (buildResult.Data is JsonElement buildData && buildData.TryGetProperty("startupSceneId", out var startup) && startup.ValueKind == JsonValueKind.String && Guid.TryParse(startup.GetString(), out var startupId) && startupId != Guid.Empty)
                        scene = startupId.ToString();
                    else
                        throw new CliException(ExitCode.ContextRequired, "FLX-PLAYTEST-SCENE-0004", "No scene is loaded and the project has no startup scene in its build list.");
                }
            }
            if (scene != null)
            {
                var opened = await bridgeClient.InvokeAsync(instance, "command.invoke", "scenes.open", new JsonObject { ["scene"] = scene }, false, context);
                var openedResult = BridgeResult(opened, "scenes.open");
                if (openedResult.ExitCode != ExitCode.Success)
                    return openedResult;
            }
            var started = await bridgeClient.InvokeAsync(instance, "editor.play", null, null, false, context);
            return await AwaitPlayState(instance, started, expected: true, context);
        }

        if (action is "end" or "stop")
        {
            var stopped = await bridgeClient.InvokeAsync(instance, "editor.stop", null, null, false, context);
            return await AwaitPlayState(instance, stopped, expected: false, context);
        }

        var bridgeAction = action switch
        {
            "status" => "editor.status",
            "pause" => "editor.pause",
            "resume" => "editor.resume",
            "step" => "editor.step",
            _ => throw new InvalidOperationException(),
        };
        return BridgeResult(await bridgeClient.InvokeAsync(instance, bridgeAction, null, null, false, context), bridgeAction);
    }

    private async Task<CliResult> Mcp(CommandArguments args, CommandContext context)
    {
        var instance = args.Option("--instance");
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("mcp does not accept arguments after '--'.");
        return await new McpServer(this, context, instance).RunAsync();
    }

    private async Task<CliResult> AwaitPlayState(EditorInstanceManifest instance, EditorBridgeInvocation requested, bool expected, CommandContext context)
    {
        var initial = BridgeResult(requested, expected ? "editor.play" : "editor.stop");
        if (initial.ExitCode != ExitCode.Success)
            return initial;
        var timeout = context.Options.Timeout ?? TimeSpan.FromSeconds(10);
        var started = Stopwatch.StartNew();
        while (started.Elapsed < timeout)
        {
            var status = await bridgeClient.InvokeAsync(instance, "editor.status", null, null, false, context);
            if (status.Response.TryGetProperty("data", out var data) && data.TryGetProperty("playMode", out var playMode) &&
                (playMode.ValueKind == JsonValueKind.True || playMode.ValueKind == JsonValueKind.False) && playMode.GetBoolean() == expected)
                return BridgeResult(status, "editor.status");
            await Task.Delay(50, context.CancellationToken);
        }
        return initial;
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
            // PowerShell can remove the quotes around JSON object property names
            // when forwarding native command arguments. Accept that common shape
            // so values such as {X:0,Y:20,Z:0} remain usable at the terminal.
            if (value.StartsWith('{') || value.StartsWith('['))
            {
                var normalized = Regex.Replace(
                    value,
                    @"(?<=[{,])\s*(?<name>[A-Za-z_][A-Za-z0-9_]*)\s*:",
                    match => $"\"{match.Groups["name"].Value}\":");
                try
                {
                    return JsonNode.Parse(normalized);
                }
                catch (JsonException)
                {
                    // Preserve the original value when it is not relaxed JSON.
                }
            }
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

    private CliResult Diagnose(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "status";
        if (action == "status")
        {
            args.Complete();
            return Doctor(new CommandArguments(Array.Empty<string>()), context);
        }
        if (action != "bundle")
            throw CommandLine.Usage("diagnose supports status or bundle.");

        var output = args.Option("--to") ?? throw CommandLine.Usage("diagnose bundle requires --to <project-relative-zip-path>.");
        var project = FindProject(null, context.Options.Project);
        args.Complete();
        if (context.Options.PassThrough.Count != 0)
            throw CommandLine.Usage("diagnose bundle does not accept arguments after '--'.");
        var bundlePath = Path.GetFullPath(output, project.Root);
        var root = project.Root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (!bundlePath.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw CommandLine.Usage("Diagnostic bundle output must remain under the selected project root.");
        if (!string.Equals(Path.GetExtension(bundlePath), ".zip", StringComparison.OrdinalIgnoreCase))
            bundlePath += ".zip";
        if (File.Exists(bundlePath))
            throw new CliException(ExitCode.ContextRequired, "FLX-DIAGNOSE-0004", $"Diagnostic bundle '{bundlePath}' already exists.");
        Directory.CreateDirectory(Path.GetDirectoryName(bundlePath)!);

        var logFiles = new List<string>();
        foreach (var directory in new[] { Path.Combine(project.Root, "Logs"), Path.Combine(project.Root, "Cache"), paths.LauncherDirectory })
        {
            if (!Directory.Exists(directory))
                continue;
            logFiles.AddRange(Directory.EnumerateFiles(directory, "*.log", SearchOption.TopDirectoryOnly));
        }
        var manifest = new
        {
            schemaVersion = 1,
            createdUtc = DateTime.UtcNow,
            project = project.Root,
            projectFile = project.ProjectFile,
            engine = context.Options.Engine,
            files = logFiles.Distinct(ProjectRegistry.PathComparer).Select(Path.GetFileName).Where(x => x != null).ToArray(),
            redacted = true,
        };
        using (var archive = ZipFile.Open(bundlePath, ZipArchiveMode.Create))
        {
            var manifestEntry = archive.CreateEntry("manifest.json", CompressionLevel.Optimal);
            using (var writer = new StreamWriter(manifestEntry.Open()))
                writer.Write(JsonSerializer.Serialize(manifest, JsonSupport.Options));
            foreach (var file in logFiles.Distinct(ProjectRegistry.PathComparer))
            {
                var entryName = "logs/" + Path.GetFileName(file);
                var entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
                using var writer = new StreamWriter(entry.Open());
                writer.Write(RedactDiagnosticText(File.ReadAllText(file)));
            }
        }
        return CliResult.Ok(new { path = bundlePath, files = logFiles.Count + 1, redacted = true });
    }

    private static string RedactDiagnosticText(string value)
    {
        var lines = value.Split(["\r\n", "\n"], StringSplitOptions.None);
        for (var i = 0; i < lines.Length; i++)
        {
            var lower = lines[i].ToLowerInvariant();
            if (lower.Contains("token") || lower.Contains("password") || lower.Contains("secret") || lower.Contains("apikey") || lower.Contains("api-key"))
                lines[i] = "[REDACTED DIAGNOSTIC LINE]";
        }
        return string.Join(Environment.NewLine, lines);
    }

    private async Task<CliResult> Tests(CommandArguments args, CommandContext context)
    {
        var action = args.Positional()?.ToLowerInvariant() ?? "list";
        ProjectContext? project = null;
        if (!string.IsNullOrWhiteSpace(context.Options.Project))
            project = FindProject(null, context.Options.Project);
        var engine = resolver.Resolve(project, context.Options.Engine);
        switch (action)
        {
        case "list":
            args.Complete();
            return CliResult.Ok(new { engine = EngineView(engine), project = project?.Root, tests = testAdapter.List(engine, project) });
        case "run":
            var kind = args.Option("--kind") ?? args.Positional() ?? "native";
            var path = args.Option("--path");
            var filter = args.Option("--filter");
            args.Complete();
            var process = await testAdapter.RunAsync(engine, project, kind, path, filter, context.Options.PassThrough, context);
            return ChildResult("test", engine, process, new { kind, path, filter, project = project?.Root });
        default:
            throw CommandLine.Usage($"Unknown test subcommand '{action}'.");
        }
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
            "powershell" or "pwsh" => CliResult.Ok("Register-ArgumentCompleter -Native -CommandName flax -ScriptBlock { param($wordToComplete) 'engines','engine','projects','templates','new','open','play','generate','compile','build','assets','scenes','actors','prefabs','settings','bake','dev','visject','jobs','feeds','player','runtime','commands','command','generators','editor','console','performance','selection','capture','playtest','mcp','test','doctor','diagnose','logs','env','config','status','completion' | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object { $_ } }"),
            "bash" => CliResult.Ok("complete -W 'engines engine projects templates new open play generate compile build assets scenes actors prefabs settings bake dev visject jobs feeds player runtime commands command generators editor console performance selection capture playtest mcp test doctor diagnose logs env config status completion' flax"),
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

    private static ProjectContext? TryFindProject(string? global)
    {
        try { return FindProject(null, global); }
        catch (CliException) when (global == null) { return null; }
    }

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

    internal static AssetBatchInput ReadAssetBatch(string path)
    {
        if (!File.Exists(path))
            throw new CliException(ExitCode.ContextRequired, "FLX-ASSET-BATCH-INPUT-0004", $"Asset batch manifest '{path}' does not exist.");
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(path))
                       ?? throw CommandLine.Usage("The asset batch manifest is empty.");
            if (node is JsonArray array)
            {
                return new AssetBatchInput
                {
                    Operations = JsonSerializer.Deserialize<AssetRequestOptions[]>(array.ToJsonString(), JsonSupport.Options) ?? [],
                };
            }
            if (node is not JsonObject)
                throw CommandLine.Usage("The asset batch manifest must be a JSON object or operation array.");
            return JsonSerializer.Deserialize<AssetBatchInput>(node.ToJsonString(), JsonSupport.Options)
                   ?? throw CommandLine.Usage("The asset batch manifest is empty.");
        }
        catch (JsonException ex)
        {
            throw CommandLine.Usage($"Asset batch manifest JSON is invalid: {ex.Message}");
        }
    }

    internal static void NormalizeAssetOperation(ProjectContext project, AssetRequestOptions operation, bool confirm)
    {
        operation.Action = (operation.Action ?? string.Empty).Trim().ToLowerInvariant();
        if (string.IsNullOrEmpty(operation.Action))
            throw CommandLine.Usage("Every asset batch operation requires an action.");
        if (operation.Path != null)
            operation.Path = AssetPath(project, operation.Path);
        if (operation.Destination != null)
        {
            operation.Destination = operation.Action == "export"
                ? Path.GetFullPath(operation.Destination)
                : AssetPath(project, operation.Destination);
        }
        if (operation.Sources != null)
            operation.Sources = operation.Sources.Select(Path.GetFullPath).ToArray();
        if (operation.BaseMaterial != null)
            operation.BaseMaterial = AssetPath(project, operation.BaseMaterial);
        operation.IfExists = string.IsNullOrWhiteSpace(operation.IfExists) ? "error" : operation.IfExists.Trim().ToLowerInvariant();
        if (operation.IfExists is not ("error" or "skip" or "update"))
            throw CommandLine.Usage("Asset operation ifExists must be error, skip, or update.");
        if (operation.Action == "delete")
        {
            if (!confirm)
                throw CommandLine.Usage("Asset batches containing delete operations require --yes.");
            operation.Force = true;
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
        "projects" => "flax projects <list|add|create|remove|info|size> [path]",
        "templates" => "flax templates <list|info> [template-id]",
        "new" => "flax new <path> [--name <name>] [--template empty]",
        "generate" => "flax generate [project] [--ide rider|vscode|vs2022|vs2026] [--build-arg value]",
        "compile" => "flax compile [project] [--target name] [--configuration value] [--platform value] [--arch value]",
        "build" => "flax build [project] --preset name --target platform [--output path] [--define value] [--clean] [--run]",
        "assets" or "asset" => AssetsHelp,
        "authoring-root" => "flax authoring-root <get|set> [path] [--project path]",
        "scenes" or "actors" or "prefabs" => AuthoringHelp,
        "settings" => SettingsHelp,
        "bake" => BakeHelp,
        "dev" => DevHelp,
        "commands" => "flax commands <list|info> [name] [--project path]",
        "command" => CommandHelp,
        "generators" or "generator" => GeneratorHelp,
        "editor" => "flax editor <status|play|pause|resume|stop|step|focus|save-all|recompile|close> [--save|--discard] [--project path] [--instance id|pid]",
        "console" => "flax console [read|clear] [--project path] [--instance id|pid] [--cursor number] [--limit 1..1000] [--level value]",
        "performance" => "flax performance [--project path] [--instance id|pid]",
        "selection" => "flax selection <get|set|clear> [--actor id ...] [--additive] [--project path] [--instance id|pid]",
        "capture" => "flax capture <viewport|game> --to <project-relative-or-absolute-path> [--project path] [--instance id|pid]",
        "playtest" => "flax playtest <status|begin|pause|resume|step|find|assert|wait|capture|end> [options] [--project path] [--instance id|pid]",
        "player" => "flax player <status|pause|resume|step|quit|input> [--project path] [--instance id|pid]\nflax player input key --key W [--state down|up|press]\nflax player input pointer --state move --x 100 --y 100\nflax player input pointer --state relative --dx 12 --dy -4\nflax player input inspect [--key W] [--axis \"Mouse X\"] [--action Jump]",
        "runtime" => "flax runtime input <key|pointer|inspect|reset> [options] [--project path] [--instance id|pid]",
        "jobs" => "flax jobs <list|info|status|wait|cancel|prune> [job-id] [--project path] [--yes]",
        "feeds" => "flax feeds <verify|list|install> --manifest path --signature path --public-key path [--id entry --to directory --yes]",
        "visject" => "flax visject groups list | asset inspect --asset path [--kind material|animation] | validate | node add|remove|set | connect|disconnect",
        "mcp" => "flax mcp [--project path] [--instance id|pid]",
        "test" => "flax test <list|run> [native|managed|build] [--kind kind] [--path path] [--filter value]",
        "diagnose" => "flax diagnose <status|bundle> [--to project-relative-zip-path] [--project path]",
        _ => HelpText,
    };

    private const string AssetsHelp = """
flax assets list [path] [--recursive]
flax assets types [folder]
flax assets info <path>
flax assets create <type> <path> [--if-exists error|skip|update]
flax assets mkdir <path> [--if-exists error|skip|update]
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
flax assets refresh [path] [--recursive]
flax assets verify <path>
flax assets material-instance <path> --base-material <asset> [--parameters <json> | --parameters-file <path>] [--if-exists error|skip|update]
flax assets batch --input <manifest.json> [--continue-on-error] [--verify-reload]

Relative asset paths are resolved under the project's Content folder.
Typed asset commands support --instance, --live-only, and --one-shot.
""";

    private const string CommandHelp = """
flax commands list [--project path] [--instance id|pid] [--live-only|--one-shot]
flax commands info <name> [--project path] [--instance id|pid] [--live-only|--one-shot]
flax command <name> [--project path] [--instance id|pid] [--live-only|--one-shot] [--arguments <json> | --input <file.json>] [--yes]
flax command <name> [--project path] [--instance id|pid] -- --option value [--flag]

Typed project commands prefer a matching running Editor and fall back to a one-shot headless Editor. Repeated options become JSON arrays.
""";

    private const string GeneratorHelp = """
flax generators list [--project path] [--instance id|pid] [--live-only|--one-shot]
flax generators info <name> [--project path] [--instance id|pid] [--live-only|--one-shot]
flax generators run <name> [--project path] [--instance id|pid] [--live-only|--one-shot] [--arguments <json> | --input <file.json>] [--dry-run] [--yes] [--detach]
flax generators run <name> [--project path] -- --option value [--flag]

Project-owned generators are public static methods marked with [CliGenerator]. Their typed schemas, dry-run support, access level, and automatic save behavior are discoverable before invocation.
""";

    private const string AuthoringHelp = """
flax scenes <list|create|open|close|reload|save|dirty|hierarchy|active|build-list> [options]
flax actors <find|get|create|create-batch|delete|rename|transform|parent|active|tag|layer> [options]
flax actors component <add|remove|get|set> [options]
flax actors primitive <list|create> [--shape cube|sphere|plane|cylinder|cone|capsule] [options]
flax actors property <list|get|set> [options]
flax prefabs <create|instantiate|variant|apply|revert|unpack|save> [options]
flax settings <list|get|schema|diff|set> [options]
flax bake <lighting|navmesh|probes|csg|scenes|sdf> <start|cancel|clear> [options]

Live scene mutations remain dirty in the Editor. Persist them explicitly with `flax scenes save` or `flax editor save-all`.

Use flax commands info <dotted-name> for each command's typed option schema.
""";

    private const string SettingsHelp = """
flax settings list
flax settings get --group <group from settings list>
flax settings schema --group <group>
flax settings diff --group <group> --values <json-object>
flax settings set --group <group> --values <json-object> [--dry-run]
""";

    private const string BakeHelp = """
flax bake status
flax bake lighting start|cancel|clear
flax bake navmesh start|clear
flax bake probes start
flax bake csg start
flax bake scenes start|cancel
flax bake sdf start
""";

    private const string DevHelp = """
flax dev unlock-eval [--expires-seconds 120]
flax dev eval --code <expression>
flax dev eval-file --path <project-relative-file>
flax dev unlock-csharp [--expires-seconds 60]
flax dev eval-csharp --code <statements-or-expression> --token <unlock-token>
flax dev eval-csharp-file --path <project-relative-file> --token <unlock-token>

The expression evaluator is bounded and read-only. Arbitrary C# is an explicit,
audited in-process development capability and is not a sandbox.
""";

    private const string HelpText = """
Flax CLI 0.1

Usage: flax [global options] <command> [arguments] [options]

Local engine commands:
  engines list|add|remove|default|info
  engine pin|unpin
  projects list|add|create|remove|info|size
  templates list|info, new <path>
  open, play, generate, compile, clean, build
  assets list|types|info|create|mkdir|import|duplicate|move|rename|delete|reimport|export|get|set|save|refresh|verify|material-instance|batch
  authoring-root get|set
  scenes list|create|open|close|reload|save|dirty|hierarchy|active|build-list
  actors find|get|create|create-batch|delete|rename|transform|parent|active|tag|layer|component
  prefabs create|instantiate|variant|apply|revert|unpack|save
  settings list|get|schema|diff|set
  bake status, lighting|navmesh|probes|csg|scenes|sdf start|cancel|clear
  dev unlock-eval|eval|eval-file|unlock-csharp|eval-csharp|eval-csharp-file
  visject groups|asset|validate|node|connect|disconnect
  player status|pause|resume|step|quit|input
  runtime input key|pointer|inspect|reset (alias for Player virtual input)
  jobs list|info|status|wait|cancel|prune [--detach on long-running commands]
  feeds verify|list|install (signed local manifests and archives)
  commands list|info, command <name>
  generators list|info|run
  editor status|play|pause|resume|stop|step|focus|save-all|recompile|close, console, performance, selection, capture
  playtest status|begin|pause|resume|step|find|assert|wait|capture|end
  mcp (stdio MCP server)
  test list|run
  doctor, diagnose status|bundle, logs, env, config, status, completion

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
