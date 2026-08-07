# Flax CLI

This directory contains the standalone .NET 8 `flax` command described by
`Docs/flax_cli_technical_design.md`.

The exhaustive current and target capability inventory, including development
Player control, runtime input, hot reload, eval, observability, Visject
authoring, and MCP, is in
`Docs/flax_cli_capability_catalog.md`.

The current implementation is the design's local-first release. It shares the
Launcher's `Versions.txt` and `Projects.txt` registries, keeps CLI-only engine
metadata separately, resolves and pins engines deterministically, launches the
Editor, invokes Flax.Build out of process, exposes the legacy Game Cooker
adapter, negotiates versioned one-shot Game Cooker requests with compatible
Editors, manages project assets through typed one-shot Editor requests,
discovers and executes project-defined typed commands through one-shot or authenticated live-Editor requests,
provides native scene, Actor, Script component, Prefab, and Visject graph authoring
command groups, discovers compatible running Editors and development Players,
controls play mode/Player state and virtual keyboard/mouse input, reads console
entries and performance snapshots, captures viewport/game evidence, exposes
deterministic live playtest observation, projects the typed catalog through MCP
stdio, scaffolds safe local projects, and emits human, TSV, JSON, or NDJSON
results from one result model. Long-running compile/build/command requests can
be detached into durable records, and signed local feed manifests can be
verified before installing archives.

Player, raw input, arbitrary C# evaluation, Visject mutation, detached jobs, and
feed installation are development capabilities. A route is available only when
the selected bridge advertises its capability; otherwise the CLI returns the
stable `FLX-BRIDGE-CAPABILITY-0004` error. Arbitrary C# is explicitly unlocked,
audited, in-process code and is not a security sandbox.

Build or publish the CLI without building the engine:

```powershell
dotnet build .\FlaxCLI\Flax.CLI\Flax.CLI.csproj
dotnet publish .\FlaxCLI\Flax.CLI\Flax.CLI.csproj -c Release -r win-x64
dotnet test .\FlaxCLI\Flax.CLI.Tests\Flax.CLI.Tests.csproj
```

Example:

```powershell
flax engines add F:\Engines\Flax --nickname team-main
flax engine pin team-main --project F:\Games\Example
flax compile F:\Games\Example --target ExampleEditor --configuration Development --platform Windows --arch x64
flax assets create Material Materials\Player.flax --project F:\Games\Example
flax assets import C:\Art\Player.fbx --to Models --project F:\Games\Example
flax assets set Settings.json Instance.MouseSensitivity 1.25 --project F:\Games\Example
flax commands list --project F:\Games\Example --json
flax command example.validate --project F:\Games\Example
flax command example.port --project F:\Games\Example -- --manifest Conversion\legacy-scene.json --dry-run
flax authoring-root set Content\Generated --project F:\Games\Example
flax scenes create --path Scenes\Automation.scene --open --project F:\Games\Example
flax actors create --type FlaxEngine.EmptyActor --name SpawnPoint --project F:\Games\Example
flax prefabs instantiate --prefab Prefabs\Player.prefab --project F:\Games\Example
flax status --project F:\Games\Example --json
flax editor play --project F:\Games\Example
flax console --project F:\Games\Example --cursor 0 --limit 100 --json
flax console clear --project F:\Games\Example --json
flax performance --project F:\Games\Example --json
flax selection set --actor <actor-id> --project F:\Games\Example --json
flax scenes active get --project F:\Games\Example --json
flax scenes build-list list --project F:\Games\Example --json
flax settings get --group game --project F:\Games\Example --json
flax bake status --project F:\Games\Example --json
flax dev unlock-eval --expires-seconds 60 --project F:\Games\Example --json
flax dev eval --code "return Level.ScenesCount;" --project F:\Games\Example --json
flax dev unlock-csharp --expires-seconds 60 --project F:\Games\Example --json
# Use --input for complex code; the token is returned by unlock-csharp.
flax dev eval-csharp --code "return Level.ScenesCount;" --token <unlock-token> --project F:\Games\Example --json
flax visject groups list --project F:\Games\Example --json
flax visject asset inspect --asset Materials\Player.flax --kind material --project F:\Games\Example --json
flax player status --project F:\Games\Example --json
flax player input key --key A --state press --project F:\Games\Example --json
flax player input pointer --x 640 --y 360 --buttons 0 --project F:\Games\Example --json
flax jobs list --project F:\Games\Example --json
flax compile F:\Games\Example --target ExampleEditor --detach --json
flax jobs wait <job-id> --project F:\Games\Example --json
flax feeds verify --manifest feed.json --signature feed.sig --public-key feed-public.pem --json
flax capture viewport --to Artifacts\viewport.png --project F:\Games\Example --json
flax playtest begin --project F:\Games\Example --json
flax playtest find --name Camera --project F:\Games\Example --json
flax playtest assert --name Camera --active true --project F:\Games\Example --json
flax playtest capture game --to Artifacts\playtest.png --project F:\Games\Example --json
flax playtest end --project F:\Games\Example --json
flax projects create F:\Games\NewFlax --name NewFlax --template empty --engine F:\Engines\Flax --json
flax templates list --json
flax test list --project F:\Games\Example --engine F:\Engines\Flax --json
flax diagnose status --project F:\Games\Example --engine F:\Engines\Flax --json
flax diagnose bundle --to Artifacts\diagnostics.zip --project F:\Games\Example --engine F:\Engines\Flax --json
```

`flax assets` supports listing and inspecting content, discovering creatable
types, creating assets and folders, importing, duplicating, moving, renaming,
guarded deletion, reimporting, exporting, and reading or writing public asset
property paths. Relative paths are resolved below the project's `Content`
folder. Mutating operations run inside the selected Editor so the content
database, asset GUIDs, loaded objects, and import pipeline remain authoritative.

Project Editor code can register synchronous public static methods with
`FlaxEditor.CliCommandAttribute`. Parameters are schema-discovered and can use
`CliOptionAttribute`; an optional `CliCommandContext` reports progress and
warnings. Methods may return ordinary structured data or `CliCommandResult`.
Commands marked `CliCommandAccess.Destructive` require `--yes`.

```csharp
[CliCommand(
    "example.validate",
    Description = "Validate the converted scene.",
    Access = CliCommandAccess.ReadOnly)]
public static CliCommandResult Validate(CliCommandContext context)
{
    context.ReportProgress("Validating scene", 0.5f);
    return CliCommandResult.Success(new { invalidActors = 0 });
}
```

Typed commands prefer a compatible running Editor for the selected project and
fall back to a new headless Editor process. Use `--live-only` or `--one-shot` to
force one route, and `--instance <id-or-pid>` when more than one Editor matches.
The live Editor and development Player use authenticated current-user IPC (a
named pipe on Windows or a user-owned Unix socket where supported), plus a
per-instance token. It is disabled in headless one-shot Editor processes. A
standalone Player must be a valid Development Player build before it can publish
its runtime manifest.

The Phase 5 authoring groups are thin routes over the typed registry. Use
`flax commands info scenes.create`, `flax commands info actors.transform`, or
the corresponding dotted name to inspect an exact option schema. Destructive
operations such as `actors delete`, `actors component remove`, `prefabs revert`,
and `prefabs unpack` require `--yes`. Bare authoring paths are confined by the
project-scoped `flax authoring-root get|set` setting.

Scene authoring is non-interactive by design. Successful CLI mutations save
their affected scenes synchronously. Before a non-additive `scenes open`, the
CLI saves any genuinely dirty loaded scenes and then transitions without a
“save changes?” dialog; clean scenes are neither prompted for nor rewritten.

Before shutting down a live Editor, choose an explicit dirty-scene policy:

```powershell
flax editor close --save --project F:\Games\Example --json
flax editor close --discard --project F:\Games\Example --json
```

These commands save or discard edited scene nodes on the Editor thread and then
request a clean exit. Do not close `FlaxEditor.exe` directly while scenes may be
dirty: the native window prompt is interactive and can block automation. The
The `settings` group currently covers the stable Flax game, time, audio, layers,
physics, input, graphics, network, navigation, localization, build, and
streaming settings, plus runtime-discovered platform settings when the installed
platform API exposes them. Use `--input <file.json>` for complex JSON patches. Bake
operations use native Flax builders (`lighting`, `navmesh`, `probes`, `csg`,
`scenes`, and `sdf`) and report capability-gated status/progress. The bounded
evaluator is session-unlocked and expression-only. `dev unlock-csharp` enables
the separate arbitrary C# path for a short, audited, in-process development
session; it is not a sandbox.

The live Editor also supports deterministic playtest observation: `playtest begin`
opens the persisted startup scene when necessary and waits for play mode,
`find`/`wait`/`assert` query runtime Actors by stable ID/name/type/active state,
and `capture` writes project-confined evidence. `flax player input key|pointer`
injects virtual events through Flax's input devices without moving the user's
OS cursor. Gamepad and project action synthesis return a deterministic
unsupported error until Flax exposes a stable ABI.

MCP clients can launch `flax mcp --project <path> --engine <path>` as a stdio
server. It supports `initialize`, `ping`, `tools/list`, and `tools/call`; the
generic `flax_command` tool and `flax.command.<dotted-name>` tools route through
the same CLI safety, capability, and confirmation checks. Closing stdin does not
close the Editor.
