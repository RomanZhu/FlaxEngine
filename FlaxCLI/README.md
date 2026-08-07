# Flax CLI

This directory contains the standalone .NET 8 `flax` command described by
`Docs/flax_cli_technical_design.md`.

The exhaustive current and target capability inventory, including Unity
Pipeline parity, development Player control, runtime input, hot reload, eval,
observability, Visject authoring, and MCP, is in
`Docs/flax_cli_capability_catalog.md`.

The current implementation is the design's local-first release. It shares the
Launcher's `Versions.txt` and `Projects.txt` registries, keeps CLI-only engine
metadata separately, resolves and pins engines deterministically, launches the
Editor, invokes Flax.Build out of process, exposes the legacy Game Cooker
adapter, negotiates versioned one-shot Game Cooker requests with compatible
Editors, manages project assets through typed one-shot Editor requests,
discovers and executes project-defined typed commands through one-shot or authenticated live-Editor requests,
provides native scene, Actor, Script component, and Prefab authoring command groups,
discovers compatible running Editors, controls play mode/script compilation, reads console entries, and emits
human, TSV, JSON, or NDJSON results from one result model.

Player-side live registration, captures, performance counters, async jobs, and
feed-backed installation remain capability-gated future work.

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
flax command example.port --project F:\Games\Example -- --manifest Conversion\unity-scene.json --dry-run
flax authoring-root set Content\Generated --project F:\Games\Example
flax scenes create Scenes\Automation.scene --open --project F:\Games\Example
flax actors create --type FlaxEngine.EmptyActor --name SpawnPoint --project F:\Games\Example
flax prefabs instantiate --prefab Prefabs\Player.prefab --project F:\Games\Example
flax status --project F:\Games\Example --json
flax editor play --project F:\Games\Example
flax console --project F:\Games\Example --cursor 0 --limit 100 --json
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
The live bridge uses a current-user named pipe on Windows or a user-owned Unix
domain socket on Linux/macOS, plus a per-instance authentication token. It is
disabled in headless one-shot Editor processes.

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
