# Flax CLI Development Phases

**Version:** Draft 0.3
**Date:** August 7, 2026
**Status:** Living implementation roadmap
**Implementation:** `FlaxCLI/` and `Source/Editor/CLI/`

## Purpose

This document is the phased technical design and delivery plan for Flax CLI. It records what is implemented, what is intentionally incomplete, and what should be built next.

The exhaustive current/target command inventory and Unity parity mapping live in `Docs/flax_cli_capability_catalog.md`.

Unity CLI is the primary product and command-surface reference. Flax CLI should reuse Unity CLI ideas where they fit, but it must preserve Flax concepts such as custom engine forks, Flax.Build targets, Game Cooker builds, the Flax content database, Actors, Prefabs, and Visject assets.

The target is one control plane shared by:

- people using a terminal;
- CI and build automation;
- one-shot headless Editor processes;
- connected Flax Editor and Player instances;
- project-specific typed commands;
- MCP clients and coding agents.

The CLI remains an orchestrator. It does not reimplement Flax.Build, Game Cooker, the content database, scene editing, or runtime input inside the CLI process.

## Status legend

| Status | Meaning |
| --- | --- |
| Complete | Implemented in this checkout and available through the standalone CLI. |
| Partial | A useful subset is implemented, but the phase acceptance criteria are not complete. |
| Next | The immediate implementation target. |
| Planned | Designed and sequenced, but not implemented. |
| Deferred | Intentionally postponed until a prerequisite or product decision exists. |

“Complete” describes implementation, not a promise that every platform and engine version has passed the final release matrix.

## Unity CLI evidence and unknown-handling rule

The locally installed `unity` command was inspected on August 6, 2026:

| Reference fact | Locally verified value |
| --- | --- |
| Unity CLI | `1.0.0-beta.3` |
| Connected Unity Pipeline package | `0.4.0-exp.1` |
| Discoverable connected tools | 140 from `unity list --json` |
| Live instance discovery | `unity status --json` returns port, project, Unity version, PID, and state. |
| Pipeline discovery | `unity pipeline list --json` returns package version, reachability, API URL, and safe-mode/update state. |
| Typed execution | `unity command [name] [args...]` targets an Editor, named Player runtime, or runtime descriptor. |
| One-shot typed execution | `unity run <project> --command <name> -- <schema arguments>`. |
| MCP | `unity mcp` starts a stdio server; `unity mcp configure --list` includes Codex and other clients. |
| Project lifecycle | `unity projects` exposes list, add, remove, info, create, clone, new, open, pin, require, size, upgrade, export, and import. |
| Template lifecycle | `unity templates` exposes list, info, create, delete, location, and edit. |

Every unresolved Unity-parity question must be checked against the installed Unity CLI before the corresponding Flax phase is implemented:

1. Run `unity <area> --help` and the relevant subcommand help.
2. Prefer `--json` or `unity list --json` when inspecting a machine contract.
3. Record the Unity CLI and Pipeline versions and the verification date in the implementation issue or design update.
4. Mark the Flax behavior as **matched**, **adapted**, or **deliberately different**.
5. If the Unity command cannot be exercised locally, mark the behavior unknown or blocked; do not fill the gap from memory.
6. Recheck experimental Unity contracts when upgrading the reference CLI.

Local verification is product research, not a compatibility promise. Flax command names and schemas may diverge when Flax has a materially different engine concept.

## Phase summary

| Phase | Status | Outcome |
| --- | --- | --- |
| 0. Contracts and standalone host | Complete | Stable command grammar, result model, exit codes, process isolation, configuration, and diagnostics. |
| 1. Local engines, projects, launch, and compilation | Complete | Pin any Flax project to a custom engine, open/play it, and compile targets deterministically. |
| 2. Typed one-shot builds and assets | Complete | Run Game Cooker and create or manipulate assets through the Editor content database with structured results. |
| 3. Typed project commands | **Implemented in source** | Reusable one-shot command registry, schema discovery, argument binding, structured results, and CLI routing. |
| 4. Running Editor and Player bridge | **Partial — active** | Authenticated running-Editor discovery, typed commands, status/control, and console access are implemented in source; Player transport and the remaining live tools are next. |
| 5. Full scene, Actor, Prefab, and specialized asset authoring | **In progress** | Core scenes, Actors, Script components, Prefabs, authoring-root safety, and deterministic saves are implemented in source; specialized assets remain. |
| 6. Deterministic runtime playtesting | Planned | Start play mode, drive input, observe collisions/events, capture evidence, assert results, and stop. |
| 7. MCP and warm automation shell | Planned | Expose the same typed catalog to Codex and other agents without inventing a second API. |
| 8. Managed distribution, projects, and templates | Deferred | Install/update engines and platforms and provide full project/template lifecycle once feeds are defined. |
| 9. Tests, diagnostics, and ecosystem completion | Planned | Async test adapters, support bundles, richer logs, cache operations, and optional account/cloud integrations. |

---

## Phase 0 — Contracts and standalone host

**Status:** Complete

### Goal

Create a standalone, self-contained .NET 8 executable named `flax` that owns the stable user contract while invoking Flax systems out of process.

### Implemented decisions

- Grammar: `flax [global options] <command> [subcommand] [arguments] [options]`.
- Human, TSV, JSON, and NDJSON render from the same typed result.
- Stable error codes and process exit codes.
- Project and engine context are resolved before invoking child tools.
- Flax.Build and project build scripts are never loaded into the CLI process.
- Editor-dependent operations run in FlaxEditor.
- Cancellation and timeouts are propagated to child processes.
- Configuration and registry writes use atomic replacement.
- Optional tracing writes a structured local diagnostic record.
- Unknown arguments fail instead of being silently ignored.

### Implemented global options

| Option | Behavior |
| --- | --- |
| `--help`, `-h` | Contextual help. |
| `--version`, `-V` | CLI and protocol versions. |
| `--format human\|tsv\|json\|ndjson` | Select output format. |
| `--json` | Alias for JSON output. |
| `--quiet` | Suppress nonessential human output. |
| `--verbose` | Include additional diagnostics. |
| `--no-color` | Disable terminal styling. |
| `--non-interactive` | Disable prompts for CI. |
| `--project <path>` | Select project context. |
| `--engine <selector>` | Override engine resolution. |
| `--timeout <seconds>` | Bound an operation. |
| `--trace` | Write a redacted trace. |
| `--` | Pass remaining arguments where the selected adapter supports it. |

This intentionally follows Unity CLI’s global-format and non-interactive model. The Flax spelling remains `--project` and `--engine` because the selected engine may be a source checkout or custom fork, not only an installed editor version.

### Result envelope

```json
{
  "schemaVersion": "1.0",
  "success": false,
  "command": "build",
  "data": null,
  "errors": [{
    "code": "FLX-BUILD-0006",
    "message": "Game Cooker failed.",
    "details": {
      "logPath": "..."
    }
  }],
  "warnings": [],
  "events": [{
    "type": "progress",
    "value": 0.63,
    "message": "Cooking materials"
  }],
  "meta": {
    "cliVersion": "0.1.0",
    "durationMs": 182431,
    "requestId": "01J4..."
  }
}
```

Unity CLI uses the same core idea: data, errors, and warnings are separate and long operations can stream NDJSON. Flax keeps its own schema/version and diagnostic code namespace. Buffered one-shot and live-bridge events are included in JSON and emitted before the terminal result in NDJSON; truly incremental live streaming remains Phase 4 work.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success. |
| 1 | Unexpected internal/general failure. |
| 2 | Usage error. |
| 3 | Authentication or authorization; reserved for optional account-backed features. |
| 4 | Required context, configuration, engine, SDK, or capability is missing. |
| 5 | Reserved. |
| 6 | Primary operation failed. |
| 130 | Interrupted by Ctrl+C/SIGINT. |
| 143 | Terminated/SIGTERM. |

### Current architecture

```text
User / CI
    |
    v
+-------------------+       +--------------------+
| flax CommandHost  |------>| Output / Exit Map  |
+---------+---------+       +--------------------+
          |
          v
+-------------------+
| ContextResolver   |
+---------+---------+
          |
          +----------------------+-----------------------+
          |                      |                       |
          v                      v                       v
+----------------+     +------------------+     +------------------+
| Registry /     |     | Process          |     | Protocol DTOs    |
| Project State  |     | Coordinator      |     | and Adapters     |
+----------------+     +--------+---------+     +---------+--------+
                               |                         |
                         +-----+------+            +-----+------+
                         |            |            |            |
                         v            v            v            v
                    Flax.Build   FlaxEditor     Game Cooker   Content DB
```

Future phases add the running Editor/Player bridge, command registry, and MCP facade without changing the host/result contract.

### Implementation layout

```text
FlaxCLI/
  Flax.CLI/
    Adapters/
    Commands/
    Core/
    Protocol/
    Services/
  Flax.CLI.Tests/

Source/Editor/CLI/
  CliRequestService.cs
  CliAssetRequestService.cs
```

Editor startup integration also touches the existing command-line bootstrap and Game Cooker completion path. Future bridge code belongs under `Source/Editor/CLI/` rather than in the standalone host.

### Security baseline

- Child processes receive argument vectors; the CLI does not invoke a shell.
- Paths are canonicalized before mutations.
- Project-controlled terminal text must not inject terminal control sequences.
- Traces must redact secrets, tokens, proxy credentials, and sensitive environment values.
- Destructive commands require explicit confirmation in non-interactive use.
- No general C# `eval` or `eval_file` command is part of the default trusted Flax surface. A future development-only eval tier may be enabled explicitly under the capability catalog's audit and session-unlock policy.
- Release downloads, when implemented, require signed metadata, hashes, safe extraction, and staging.

Unity Pipeline exposes `eval` and `eval_file` as open-world automation escape hatches. Flax prioritizes typed commands, but the capability catalog now specifies a deliberately opt-in development eval tier so agents can fill genuine catalog holes without presenting arbitrary execution as an ordinary safe command.

### Acceptance

- Local help and diagnostics do not load Flax.Build or FlaxEditor.
- All output formats use one result model.
- Unknown arguments produce exit code 2.
- Cancellation terminates the launched process tree and maps to 130/143.
- CLI failures include stable diagnostic codes.

---

## Phase 1 — Local engines, projects, launch, and compilation

**Status:** Complete

### Goal

Make the common local-development path work for stock engines, source checkouts, and custom forks without depending on a remote release feed.

### Implemented commands

| Area | Commands |
| --- | --- |
| Engines | `engines list`, `engines add`, `engines remove`, `engines default`, `engines info` |
| Project engine | `engine pin`, `engine unpin` |
| Projects | `projects list`, `projects add`, `projects remove`, `projects info`, `projects size` |
| Launch | `open`, `play` |
| Build system | `generate`, `compile`, `clean` |
| Local tooling | `doctor`, `logs`, `env`, `config`, `status`, `completion` |

Phase 1 originally exposed process-only Editor status. Phase 4 now augments it with authenticated instance discovery and capabilities while retaining unbridged-process reporting for older Editors.

### Engine resolution

Resolution is deterministic in this order:

1. Explicit `--engine <selector>`.
2. Exact project `.flax/engine.lock`.
3. Project `EngineNickname`.
4. Highest compatible installed stable engine satisfying `MinEngineVersion`.
5. Configured default engine.
6. The only installed compatible engine.
7. Otherwise fail with exit code 4 and return candidates.

A lock or nickname mismatch is an error. A custom fork can share an upstream semantic version, so version alone is not identity.

Example lock:

```json
{
  "schemaVersion": 1,
  "engine": {
    "version": "1.12.0",
    "channel": "stable",
    "nickname": "team-main",
    "fingerprint": "sha256:4f7b...",
    "source": "local"
  }
}
```

The lock lives at `<project>/.flax/engine.lock` and is suitable for source control.

### Compile versus build

- `flax compile` invokes Flax.Build to compile source targets.
- `flax clean` invokes the selected Flax.Build cleaning operation.
- `flax build` invokes Game Cooker to produce a deployable game.

This distinction is intentionally Flax-native. Unity CLI has a general build surface, but collapsing Flax.Build target compilation and Game Cooker packaging would make scripts and failures ambiguous.

### Project capability baseline

At the end of this phase, Flax CLI can:

- pin a project to the correct Flax engine;
- launch or play the project;
- compile project targets;
- participate in deterministic JSON/NDJSON automation.

Phase 2 adds the asset and Game Cooker items to the same baseline.

### Unity CLI relationship

This phase corresponds to parts of Unity CLI’s `editors`, `projects`, `open`, `run`, `doctor`, `logs`, `env`, and `config` areas. Flax adapts “editor selection” into engine resolution because source-built and nickname-addressed engines are first-class.

Before adding any missing engine/project subcommand, check the installed Unity CLI help for the equivalent. In particular, Phase 8 must recheck `unity editors`, `unity projects`, `unity install`, and `unity templates` rather than copying the current beta surface blindly.

### Acceptance

- A project resolves to its pinned custom engine consistently.
- `open` and `play` launch the resolved Editor.
- `compile` invokes Flax.Build out of process.
- Removing a project or local engine registration does not delete its files.
- JSON/NDJSON callers receive deterministic results and errors.

---

## Phase 2 — Typed one-shot builds and asset operations

**Status:** Complete

### Goal

Use the Editor as a one-shot headless service for operations that require Editor subsystems, while returning structured events and results instead of scraping human logs.

### Implemented Game Cooker path

`flax build` creates a versioned request, launches the resolved Editor headlessly with `-cliRequest`, and waits for a structured result. The Editor invokes Game Cooker and reports completion only after the asynchronous build has completed.

Representative request:

```json
{
  "schemaVersion": 1,
  "operation": "build",
  "requestId": "01J4...",
  "projectPath": "F:/Games/Example",
  "preset": "Development",
  "target": "Windows",
  "outputPath": "Artifacts/Windows",
  "customDefines": ["CLIENT"],
  "options": {
    "clean": false,
    "runAfterBuild": false
  }
}
```

### Implemented asset surface

| Command | Behavior |
| --- | --- |
| `assets list [path] [--recursive]` | List content database entries. |
| `assets types [folder]` | List creatable asset types. |
| `assets info <path>` | Inspect identity, type, size, and metadata. |
| `assets create <type> <path>` | Create a supported Flax asset. |
| `assets mkdir <path>` | Create a content folder. |
| `assets import <source...> --to <folder>` | Import source files through Flax importers. |
| `assets duplicate <source> <destination>` | Duplicate through the content database. |
| `assets move <source> <destination>` | Move through the content database. |
| `assets rename <source> <name>` | Rename without using a path as the new name. |
| `assets delete <path> --yes` | Delete only with explicit confirmation. |
| `assets reimport <path>` | Run the asset reimport path. |
| `assets export <path> --to <folder>` | Export when the asset type supports it. |
| `assets get <path> <property.path>` | Read a public dotted property path. |
| `assets set <path> <property.path> <json> [--no-save]` | Deserialize and write a public property. |
| `assets save <path>` | Persist the asset. |

These operations use Flax’s content database rather than editing asset files behind the Editor’s back.

### Current structured event model

Long Editor operations may produce:

- `started`;
- `phase`;
- `progress`;
- `diagnostic`;
- `artifact`;
- `result`.

Example:

```json
{"type":"phase","requestId":"01J4...","name":"CookAssets"}
{"type":"progress","requestId":"01J4...","value":0.63,"message":"Cooking materials"}
{"type":"diagnostic","requestId":"01J4...","severity":"warning","code":"FLX-ASSET-W001","message":"..."}
{"type":"result","requestId":"01J4...","success":true,"exitCode":0}
```

### Complete project capability baseline

Currently Flax CLI can:

- pin a project to the correct Flax engine;
- launch or play the project;
- compile project targets;
- import, inspect, reimport, and modify assets through Flax’s content database;
- run Game Cooker builds;
- return structured JSON/NDJSON results and diagnostics.

### Current limitation

Phase 2 Editor-dependent operations use the one-shot transport. The generic `flax command` surface also uses that transport when no compatible live Editor is selected. Phase 4 adds live routing without changing the asset and Game Cooker contracts.

### Unity CLI relationship

Unity Pipeline exposes content operations such as find/create/import/copy/move/rename/delete assets, serialized property editing, import settings, and save operations. Phase 2 establishes the same architectural rule—Editor-owned state is mutated through the Editor—but only implements the general Flax asset lifecycle.

Unity’s connected tools are generally live. Flax’s Phase 2 asset and Game Cooker operations remain one-shot so they work in CI and do not require a manually opened Editor. Phase 4 adds live typed-command routing without removing one-shot execution.

### Acceptance

- A Game Cooker result cannot report success before the cooker callback completes.
- Asset mutation uses the content database and returns typed results.
- Destructive deletion requires `--yes`.
- Temporary request/result files are cleaned.
- Unsupported asset types or properties fail with structured diagnostics.

---

## Phase 3 — Typed project commands

**Status:** **Implemented in source; Editor integration validation pending**

The reusable engine/CLI phase is implemented in source:

- public `CliCommandAttribute`, `CliOptionAttribute`, `CliCommandContext`, `CliCommandResult`, and cooperative `CliCommandOperation` APIs;
- reflection-based command discovery and schema validation;
- one-shot `command` request dispatch with list, info, and invoke actions;
- `flax commands list|info` and `flax command <name>`;
- JSON files/objects, schema-style options, repeated option arrays, progress, warnings, structured failures, and destructive-command confirmation.

The remaining work is integration validation after the Editor is rebuilt. Project-specific commands belong to project code or plugins and are consumers of this API, not FlaxCLI phases.

### Goal

Implement the shared typed command registry and execute built-in or project-defined commands in a one-shot headless Editor:

```powershell
flax commands list --project . --json
flax command example.validate --project .
```

MCP and a running Editor bridge are not prerequisites. Phase 3 extends the already working `-cliRequest` path with `operation: "command"`.

### Why this phase comes next

Unity CLI proves that a discoverable custom-command surface is more scalable than adding a top-level CLI command for every project workflow. Its verified one-shot equivalent is:

```text
unity run <project> --command <name> -- <schema arguments>
```

Its verified live equivalent is:

```text
unity command <name> <args...>
```

Flax shares the registry between one-shot and live execution. The schema produced here is reused by Phases 4 and 7.

### Public surface

| Command | Purpose |
| --- | --- |
| `flax commands list --project <path>` | Discover project and built-in commands with schemas and requirements. |
| `flax commands info <name> --project <path>` | Inspect one command. |
| `flax command <name> --project <path> [-- <args...>]` | Invoke a typed command. |
| `flax command <name> --arguments <json>` | Provide a JSON argument object for complex automation. |
| `flax command <name> --input <file.json>` | Read large structured input without command-line quoting. |

Global options may appear before or after the command because the current parser already recognizes them across the invocation. Documentation should show one canonical style.

### Registration contract

```csharp
[CliCommand(
    "example.validate",
    Description = "Validate project content and return structured findings.",
    Access = CliCommandAccess.ReadOnly,
    RequiresMainThread = true)]
public static CliCommandResult Validate(
    [CliOption("scope", Required = false)] string scope = "Content",
    CliCommandContext context = null)
{
    context?.ReportProgress("Validating project content", 0.5f);
    return CliCommandResult.Success(new { scope, errors = 0 });
}
```

These attribute names are now the implemented Phase 3 API. The design was checked against the installed `unity command`/`unity list --json`; Flax uses public static attributed methods for the default trusted surface. Any future general-purpose eval is a separately unlocked development capability, not an implicit registry feature.

### Command contract

The implemented registry declares:

- unique dotted name;
- description and version;
- typed parameters with defaults, required state, enum values, and descriptions;
- an inferred return type descriptor;
- access level: `ReadOnly`, `MutatesProject`, or `Destructive`;
- main-thread requirement;
- whether a loaded scene or play mode is required;
- project/plugin owner used in diagnostics;
- a cancellation token in `CliCommandContext` for live requests.

Commands that can exceed a frame budget return `CliCommandOperation`. The live bridge advances one operation on the Editor thread in short, cancellation-aware slices; one-shot execution drains the same operation synchronously because that Editor process is dedicated to the request. Returning `Task` is intentionally unsupported because an arbitrary continuation cannot safely access main-thread-only Editor state.

Current parameters and results use JSON-serializable primitives, enums, arrays, and objects. Explicit bounds, stable Asset/Actor/file reference schemas, Player-runtime requirements, dry-run/Undo metadata, and per-command timeout metadata belong to later schema revisions; arbitrary CLR object serialization is not a promised public protocol.

### One-shot request

```json
{
  "schemaVersion": 1,
  "operation": "command",
  "requestId": "01J...",
  "projectPath": "F:/Games/Example",
  "command": {
    "action": "invoke",
    "name": "example.validate",
    "confirm": false,
    "arguments": {
      "scope": "Content"
    }
  },
  "resultPath": "C:/Temp/flax-cli-result.json"
}
```

The Editor loads the project and scripts, builds the command catalog, validates arguments before mutation, invokes the command on the required thread, saves only when requested, writes the final result atomically, and exits.

### Reliability requirements

- Validate the full schema and required access level before invoking project code.
- Require explicit `--yes` confirmation for commands marked `Destructive`.
- Reject duplicate names, invalid method shapes, unknown arguments, and unsupported parameter types deterministically.
- Keep large structured inputs in versioned files or project content rather than shell arguments.
- Buffer progress and warnings into the stable result/event protocol.
- Cancellation must stop at safe checkpoints and return a structured interrupted result.
- Do not expose general-purpose C# evaluation in the default trusted catalog; require the separate development-only unlock, audit, and execution policy defined in the capability catalog.

### Acceptance

- `flax commands list --project . --json` returns built-in and project command schemas.
- `flax commands info <name>` returns one exact descriptor or a structured not-found error.
- `flax command <name> --project .` completes in a new headless Editor when no live instance is selected.
- Invalid or missing arguments fail before the command method runs.
- Read-only, mutating, and destructive access levels are reported; destructive commands require explicit confirmation.
- The final JSON contains data, warnings, failures, and progress events.
- The route works with MCP absent and no running Editor.

---

## Phase 4 — Running Editor and Player bridge

**Status:** **Partial — active development**

### Goal

Discover compatible running Flax Editor and Player instances and execute the Phase 3 command catalog without launching a new Editor for every operation. The running-Editor slice is implemented in source; Player registration and the remaining live capabilities are not.

### Implemented running-Editor slice

- A non-headless Editor starts a per-instance local bridge module.
- Windows uses a current-user-only named pipe; Linux and macOS use a user-owned Unix domain socket.
- Each instance publishes a versioned manifest plus a random 256-bit token in the CLI runtime directory.
- The Editor refreshes the manifest atomically when it moves between ready, compiling, playing, and paused states.
- The CLI validates protocol version, PID, process start time, token path, project match, and selector ambiguity.
- Stale descriptors are pruned only from the owned runtime directory.
- `flax status` reports bridged instances and labels legacy process-only Editors separately.
- `flax commands list|info` and `flax command` prefer a matching live Editor, with `--live-only`, `--one-shot`, and `--instance` for deterministic routing.
- `flax editor status|play|pause|resume|stop|step|focus|save-all|recompile` runs on the Editor main thread.
- `flax console` reads a bounded in-memory console buffer using monotonically increasing cursors.
- Live typed commands receive a connection timeout cancellation token and return structured data, warnings, errors, and buffered progress events.
- The Editor admits at most one request per frame, advances one cooperative command with a 3 ms frame budget, bounds the pending queue and buffered command events, and keeps all scene/asset/Undo access on the Editor thread.

The bridge is not exposed in headless one-shot Editor processes, so the one-shot request service remains the CI-safe fallback.

### Unity CLI reference

The installed Unity CLI verifies the intended control-plane behavior:

- `unity pipeline list --json` reports installed Pipeline package version and server reachability.
- `unity status --json` reports live instances with project, version, PID, port, and state.
- `unity list --json` exposes command schemas.
- `unity command` targets a project, a named Player runtime, or a runtime descriptor.
- The connected Pipeline instance currently exposes 140 tools.

Flax matches discoverability and typed execution but deliberately adapts the transport. Unity Pipeline currently reports an HTTP endpoint on `127.0.0.1`. Flax uses operating-system local IPC to reduce network and firewall surface.

### Transport and discovery

- Windows: named pipe with an ACL limited to the current user.
- Linux/macOS: Unix domain socket under a user-owned runtime directory with mode 0600.
- Each process writes a versioned instance manifest.
- The CLI verifies PID/start time and endpoint ownership before connecting.
- Each session authenticates with a random 256-bit token stored in an owner-readable file.
- Stale descriptors are removed only after process and ownership checks.
- No LAN listener or unauthenticated TCP endpoint.

Representative manifest:

```json
{
  "schemaVersion": 1,
  "instanceId": "9d2f...",
  "pid": 18432,
  "processStartTimeUtc": "2026-08-07T15:42:10Z",
  "kind": "editor",
  "projectPath": "F:/Games/Example",
  "engineVersion": "1.12.0",
  "engineNickname": "team-main",
  "protocolVersion": 1,
  "transport": "namedPipe",
  "endpoint": "flax-cli-18432-9d2f",
  "tokenPath": ".../editor-18432-9d2f.token",
  "state": "ready",
  "capabilities": ["commands", "playMode", "console", "saveAll", "focus", "recompile"]
}
```

### Public surface

| Command | Status | Purpose |
| --- | --- | --- |
| `flax status [--project <path>]` | Implemented for Editors | List authenticated instances and capabilities. |
| `flax commands list` | Implemented | Discover the selected instance’s catalog. |
| `flax command <name>` | Implemented | Execute a typed command on the selected live instance. |
| `flax editor status` | Implemented | Report edit/play/paused/compiling state. |
| `flax editor play\|pause\|resume\|stop\|step\|focus` | Implemented | Control play mode and focus. |
| `flax editor save-all` | Implemented | Save dirty scenes and assets. |
| `flax console [--level <level>] [--cursor <cursor>]` | Implemented | Read structured console entries. |
| `flax editor recompile` | Implemented | Queue script compilation. |
| `flax capture game\|viewport --to <path>` | Planned | Capture game or scene view. |
| `flax performance` | Planned | Read frame, CPU, memory, render, and scene counters. |

Selection options should mirror Unity’s useful concepts after local re-verification:

- project path;
- explicit PID/instance ID;
- Editor versus Player;
- Player executable name;
- runtime descriptor path;
- timeout.

Flax option names must be chosen only after checking the then-installed `unity command --help`. Multi-match selection must fail with candidates rather than guess.

### Routing

`flax command` uses one catalog and two transports:

1. If a compatible live instance matches the project, use the bridge; an explicit selector wins.
2. If no live instance exists and the command supports one-shot Editor execution, use Phase 3.
3. If the command requires a Player/runtime, fail with actionable candidates.
4. `--live-only` and `--one-shot` allow deterministic caller choice.

### Async jobs

Unity Pipeline uses queued operations plus status tools for builds, recompiles, tests, and bakes. Flax adopts the general pattern:

```text
flax <operation> ... --detach       -> jobId
flax jobs status <jobId>            -> queued/running/succeeded/failed/cancelled
flax jobs wait <jobId>
flax jobs cancel <jobId>
```

Job records include owner instance, operation, timestamps, progress, diagnostics cursor, artifacts, and final result. Disconnecting a CLI client does not silently cancel a detached job.

### Acceptance

- `flax status --json` reports authenticated Editor instances and their capabilities; Player support remains an explicit incomplete item.
- A live `flax command example.validate` avoids launching a second Editor.
- Multiple matching instances cause a deterministic selection error.
- Console polling uses cursors and does not duplicate entries.
- An older Editor advertises reduced capabilities and is never sent an unsupported request.

Remaining acceptance for Phase 4 completion:

- Player processes can opt into the same authenticated discovery/command protocol.
- Capture and performance actions are implemented and capability-gated.
- Long-running actions can detach into durable jobs with status, wait, and cancellation.
- Progress can stream incrementally rather than only being returned in the final response.
- Captures can return a saved path and, for MCP, bounded inline image data.

---

## Phase 5 — Full scene, Actor, Prefab, and specialized asset authoring

**Status:** In progress — core scene, Actor, component, Prefab, and authoring-root commands are implemented in source; specialized assets, build-list/active-scene semantics, settings, and bake operations remain planned.

### Goal

Provide broad Flax-native authoring coverage comparable to the installed Unity Pipeline catalog, including scene hierarchy, GameObject/component-equivalent operations, Prefabs, import/settings workflows, bakes, and specialized assets.

### Implemented core slice

- Direct CLI groups route through the shared typed-command protocol: `flax scenes`, `flax actors`, and `flax prefabs`. They therefore use the same one-shot and authenticated live-Editor transports as `flax command`.
- Scene list/create/open/save/hierarchy, all listed core Actor operations, Script component add/remove/get/set, and Prefab create/instantiate/variant/apply/revert/unpack/save are registered typed commands.
- Actor results use scene ID, Actor ID, hierarchy path, runtime type, and name handles. Mutations report saved/dirty state.
- Destructive Actor, component, and Prefab operations inherit the registry's `--yes` enforcement. Actor batch creation validates the whole request before mutation and records one undo action.
- `flax authoring-root get|set` persists a project-scoped root in `.flax/cli.json`. Editor-side path resolution confines authoring paths to that root and rejects traversal through filesystem links.
- Successful CLI scene mutations save their affected scenes synchronously before returning. Non-additive CLI scene transitions synchronously save only scenes already marked dirty, then open the requested scene without invoking Flax's interactive save-confirmation path; clean scenes are not rewritten.
- This slice has CLI unit coverage and source inspection validation. It still requires the design's rebuilt-Editor integration validation before Phase 5 acceptance can be marked complete.

### Scene commands

| Flax command | Unity CLI reference |
| --- | --- |
| `scenes list` | `list_open_scenes` |
| `scenes create` | `create_scene` |
| `scenes open` | `open_scene` |
| `scenes active` | `set_active_scene` |
| `scenes save` | `save_scene` / `save_all` |
| `scenes hierarchy` | `get_scene_hierarchy` |
| `scenes build-list add\|remove\|list` | `add_scene_to_build` / `remove_scene_from_build` / build settings |

### Actor and component commands

| Flax command | Unity CLI reference |
| --- | --- |
| `actors find` | `find_gameobjects` |
| `actors get` | scene hierarchy and serialized property tools |
| `actors create` / `actors create-batch` | `create_gameobject` / `create_gameobjects` |
| `actors delete` / `actors rename` | `delete_gameobject` / `rename_gameobject` |
| `actors transform` | `set_transform` |
| `actors parent` | `set_parent` |
| `actors active` | `set_active` |
| `actors tag` / `actors layer` | `set_tag` / `set_layer` |
| `actors component add\|remove` | `add_component` / `remove_component` |
| `actors component get\|set` | `get_component_properties` / `set_component_properties` |

Flax returns stable typed handles containing scene ID, Actor ID, hierarchy path, type, and name. Names alone are not identity.

### Prefab commands

- `prefabs create`;
- `prefabs instantiate`;
- `prefabs variant`;
- `prefabs apply`;
- `prefabs revert`;
- `prefabs unpack`;
- `prefabs save`.

These correspond to Unity Pipeline’s create/instantiate/variant/apply/revert/unpack/save Prefab tools but must map to Flax’s Prefab and inherited-object semantics.

### Asset completion

Phase 2 is the generic lifecycle baseline. Full asset creation/manipulation also needs:

- search by name, type, path, ID, tag, dependency, and reference;
- stable asset/Actor reference encoding;
- batch create/import/move/delete/set with one transaction;
- import-setting get/set and importer-specific schemas;
- dependency and reverse-reference inspection;
- material parameter and shader metadata editing;
- model, texture, audio, animation, skeleton, font, and localization settings;
- Animation Graph and other Visject graph creation/editing;
- particle, behavior tree, scene animation, and timeline-style editing where Flax supports them;
- content diff/validate/fix commands;
- save-all and dirty-state inspection;
- plugin/package list/add/remove/resolve operations when a stable Flax package contract exists.

Before defining each specialized schema, inspect the equivalent installed Unity tools with `unity list --json` and then inspect the Flax API. Record matched concepts and Flax-specific differences. Do not transliterate Animator, Timeline, Shader Graph, or package concepts if the Flax asset model differs.

### Settings and bake operations

Unity Pipeline exposes settings for build, player, graphics, quality, physics, audio, time, input, lighting, NavMesh, tags/layers, and importers. Flax should add typed equivalents only where an Editor API and persistence contract exist.

Planned long-running operations:

- lighting bake/status/cancel/clear;
- navigation bake/status/cancel/clear;
- occlusion bake/status/cancel/clear;
- collision rebuild/status;
- shader and content recompile/status;
- build target switch/status where applicable.

The collision rebuild is important for reliable playtests such as the First Person Shooter template. It should be an explicit structured operation rather than an undocumented manual prerequisite.

### Mutation safety

Unity Pipeline’s verified authoring tools influenced these mandatory rules:

- `flax authoring-root get|set` confines bare authoring paths under a project content root.
- Mutations support `--dry-run` where a useful preview exists.
- Destructive operations require `--yes` or an authenticated interactive confirmation.
- Live Editor mutations create one coherent Undo action where the underlying Editor supports Undo.
- Batch requests validate every item before applying any item unless explicitly marked best-effort.
- Object references use typed handles, not ambiguous names.
- Omitted transform/property channels remain unchanged.
- Paths cannot escape the project authoring root through `..`, links, or case tricks.
- Core CLI scene mutations save synchronously without prompting and report whether a save occurred; read-only and no-op operations do not rewrite clean scenes.

### Acceptance

- A caller can create a scene, construct an Actor hierarchy, add/configure components, create/instantiate a Prefab, save, reopen, and validate it without GUI interaction.
- The same operations work through CLI JSON and the Phase 4 bridge.
- Invalid references fail before partial mutation in atomic mode.
- Every mutation reports changed object handles and dirty/saved state.
- Importer and specialized asset schemas are discoverable and versioned.

---

## Phase 6 — Deterministic runtime playtesting

**Status:** Planned

### Goal

Go beyond “launch with `-play`” and support reproducible gameplay tests: start play mode, find a runtime Actor, drive input or invoke a typed gameplay action, wait for observable state, capture evidence, assert results, and exit cleanly.

### Unity CLI reference and deliberate extension

The installed Unity Pipeline catalog includes Editor play/pause/stop, hierarchy queries, console, screenshots, performance statistics, custom commands, and `eval`. It does not expose a generic built-in keyboard/mouse injection tool in the inspected 140-tool catalog.

Flax therefore should not pretend input injection is inherited Unity parity. It is a deliberate Flax extension. Before implementation, recheck the installed `unity list --json` for any new runtime-input tools and record the result.

### Public surface

```text
flax playtest begin --project . [--scene Content/game.scene]
flax playtest find --name "Cube 4"
flax playtest input key W --duration 1.5s
flax playtest input mouse --dx 120 --dy -10
flax playtest input button Fire --press
flax playtest move --actor <handle> --forward 3m
flax playtest look-at --actor <handle>
flax playtest wait --condition <typed-condition> --timeout 5s
flax playtest raycast --from camera --through crosshair
flax playtest capture --to Artifacts/playtest.png
flax playtest end
```

Raw key/mouse input is useful for end-to-end coverage, but semantic project commands are more deterministic. A game may register commands such as `game.player.move` or `fps.fire` while the built-in playtest layer observes shared engine state.

### Observation and assertions

Typed conditions should cover:

- Actor exists/destroyed/enabled;
- transform enters bounds;
- component property equals or matches a tolerance;
- collision or trigger event occurred;
- raycast hit Actor/material/layer;
- log message/error occurred after a cursor;
- frame count or elapsed game time reached;
- screenshot captured;
- project-defined assertion command succeeded.

The bridge buffers a bounded event stream with sequence numbers so a test can distinguish “Cube 4 was hit after firing” from a stale log entry.

### First Person Shooter acceptance scenario

```text
1. Open FirstPersonShooter in the resolved Editor.
2. Ensure collision data is present or run the explicit collision rebuild job.
3. Enter play mode and wait for the player controller.
4. Locate "Cube 4" and obtain a stable runtime Actor handle.
5. Move/look using deterministic input or a typed project helper.
6. Fire.
7. Assert a raycast, damage event, impact event, or project-specific hit marker references Cube 4.
8. Capture the game view and relevant event/log records.
9. Stop play mode and report whether Cube 4 was hit.
```

A screenshot that merely appears aimed at the cube is not proof. The final result must include a typed hit observation or explicitly state that no instrumented hit signal was available.

### Determinism and safety

- Support fixed-step/frame-step execution where the runtime permits it.
- Record scene, engine fingerprint, command schema versions, seed, input timeline, and timestamps.
- Reset input state on cancellation/disconnect.
- Cap held-key duration and mouse deltas.
- Keep Player control local-user-only and authenticated.
- Separate test actions from unrestricted OS input automation.
- Stop play mode gracefully; only terminate a process when explicitly requested or after a timeout policy.

### Acceptance

- A script can enter play mode, walk, look, shoot, observe a typed hit, capture evidence, and stop without desktop computer-use automation.
- The FPS Cube 4 scenario returns `hit: true|false` with the observed Actor ID and evidence.
- Cancellation cannot leave keys/buttons held.
- Repeated tests produce a replayable input/event record.

---

## Phase 7 — MCP and warm automation shell

**Status:** Planned

### Goal

Expose the Phase 3–6 typed command catalog to agents and long-lived automation without creating a separate implementation or privilege model.

### MCP surface

```text
flax mcp
flax mcp --project .
flax mcp --instance <id>
flax mcp configure codex
flax mcp configure --list
```

The installed Unity CLI verifies this product shape: `unity mcp` runs a stdio MCP server, and `unity mcp configure --list` currently includes Codex, Claude Desktop/Code, Cursor, VS Code, Copilot CLI, Windsurf, Cline, and other clients.

Before implementing client configuration, re-run the Unity command and check each target client’s current MCP configuration contract. Client file paths and formats are external and may change.

### One registry, multiple front ends

```text
                         +--------------------+
Terminal / CI ---------->|                    |
One-shot Editor -------->| Typed command      |----> Flax services
Live bridge ------------>| registry + schemas |----> Project commands
MCP stdio -------------->|                    |----> Player runtime
Warm shell -------------->|                    |
                         +--------------------+
```

Rules:

- MCP tool names, descriptions, input schemas, access levels, and results derive from the same command descriptors as `flax commands list`.
- MCP is a facade, not a second command implementation.
- Read-only, mutating, and destructive access metadata is preserved.
- Destructive tools require explicit approval/confirmation appropriate to the client.
- Large screenshots may be returned as bounded image content; large files return paths/resources.
- Logs use cursors and bounded page sizes.
- MCP never exposes eval unless the selected development instance advertises an active, explicitly unlocked eval capability; MCP cannot bypass the same expiration, audit, or confirmation policy.
- Project-defined commands such as `example.validate` appear automatically when their schemas are valid.

### Why MCP is not Phase 3

Requiring MCP for project automation would add a client protocol without improving the underlying operation. The typed command path must work first; MCP then makes the same catalog agent-friendly.

### Warm shell

A long-lived `flax shell --format ndjson` can reuse discovery, authentication, schemas, and connections for low-latency automation. It is optional and must not become the only way to run commands.

### Acceptance

- Codex can configure and launch `flax mcp` through the CLI.
- MCP discovery exposes the same schemas and permissions as `flax commands list`.
- An `example.validate` result is equivalent across CLI JSON, live bridge, and MCP.
- No MCP-only mutation bypasses CLI safety rules.
- Closing MCP does not terminate an Editor or detached job unless explicitly requested.

---

## Phase 8 — Managed distribution, project lifecycle, and templates

**Status:** Deferred pending feed and ownership contracts

### Goal

Bring the broader Unity CLI installation and project-management ideology to Flax after signed release catalogs, Launcher registry ownership, and platform-package layouts are defined.

### Planned engine/distribution commands

- `releases list|info`;
- `install <selector>`;
- `install repair <selector>`;
- `uninstall <selector>`;
- `upgrade`;
- `platforms list|install|remove`;
- `cache list|prune|clear`;
- `engines running`;
- `engines require`.

Unity CLI references to inspect at implementation time include `install`, `install-modules`, `modules`, `editors`, `releases`, `upgrade`, `uninstall`, and `cache`.

### Planned project commands

- `projects create` / `new`;
- `projects clone`;
- `projects open`;
- `projects close`;
- `projects pin` / `unpin`;
- `projects require`;
- `projects upgrade`;
- `projects export` / `import`;
- optional project link/unlink only if Flax gains a corresponding service.

The current installed `unity projects --help` exposes all of those concepts, including create/new distinctions and registry export/import. Flax must check their detailed help before deciding whether both `create` and `new` add value.

### Planned template commands

- `templates list`;
- `templates info`;
- `templates create`;
- `templates edit`;
- `templates delete`;
- `templates location`;
- `new <name> --template <selector>`.

The installed Unity CLI has the same template lifecycle. Flax template packaging and project-generation semantics remain unknown until defined; they must be checked against both Unity CLI and Flax Launcher behavior before implementation.

### Release manifest

A versioned signed manifest should identify:

- CLI protocol/schema version;
- engine release ID, semantic version, channel, and publication time;
- OS and architecture;
- Editor archive and SHA-256;
- optional debug symbols/source;
- supported platform packages;
- dependencies and disk requirements;
- signature/key identity.

### Install transaction

1. Resolve selector and platform.
2. Acquire a scoped install lock.
3. Download to cache with resume.
4. Verify signature, size, and SHA-256.
5. Extract into a unique staging directory with traversal checks.
6. Validate Editor and Flax.Build identities/capabilities.
7. Atomically promote to the final directory.
8. Register only after promotion succeeds.
9. Roll back registry state on failure.

Local registered/source engines are never deleted by `uninstall` unless they were installed and owned by Flax CLI.

### Open ownership decisions

- Who owns and signs the release catalog?
- Does the Launcher expose a shared registry library/API, or do both clients migrate?
- What is the official platform-package manifest/layout?
- Which channels—stable, preview, daily—have compatibility guarantees?
- How are CLI self-updates packaged per OS?

These are real blockers. Unity CLI may guide user experience, but it cannot answer Flax feed ownership.

### Acceptance

- A clean CI machine can install an exact engine and platforms from signed metadata.
- Interrupted installs never appear as valid registered engines.
- Repair verifies and restores managed files.
- Project/template commands never overwrite existing content without confirmation.
- Launcher and CLI show consistent engine/project state.

---

## Phase 9 — Tests, diagnostics, and ecosystem completion

**Status:** Planned

### Goal

Complete the control plane around the core authoring/runtime features and make failures diagnosable in local development and CI.

### Test adapters

Planned surface:

```text
flax test list
flax test run [filter] [--detach]
flax test status <jobId>
flax test cancel <jobId>
```

Unity Pipeline exposes `list_tests`, `run_tests`, `test_status`, and cancellation. Flax adopts the async job shape but must define adapters because Flax does not have one universal user-project test framework.

Adapters may include:

- native engine tests;
- managed engine tests;
- project-defined test assemblies;
- runtime playtests from Phase 6;
- project commands that return a test result schema.

### Diagnostics and logs

- `doctor` grows scoped checks and opt-in repairs.
- `logs` gains source filters, cursors, tail/follow, levels, and time ranges.
- `diagnose bundle` creates a redacted support archive.
- `env` reports resolved tools/SDKs without dumping secrets.
- `status` includes compilation, asset discovery, cooker jobs, bridge health, and safe-mode equivalents where Flax has them.
- Proxy diagnostics are added only if the managed distribution phase needs network/proxy support.

Unity CLI areas to recheck include `diagnose`, `doctor`, `logs`, `status`, proxy options, `bug`, and `analytics`.

### Optional ecosystem areas

- authentication and license commands only for actual Flax services that require them;
- cloud commands only with a defined Flax cloud contract;
- opt-in anonymous analytics that never include paths, project names, arguments, or source;
- plugin/package lifecycle when the underlying package manager has a stable API;
- interactive shell/repl conveniences;
- changelog and upgrade advisories.

Local engine/project/build/authoring commands must remain account-independent.

### Acceptance

- Test runs can be queued, observed, cancelled, and reported in CI JSON.
- Support bundles are deterministic and redact known secret classes.
- Log following uses cursors and bounded memory.
- Optional network/account features cannot break offline local commands.

---

## Cross-phase technical contracts

### Configuration precedence

1. Command-line flags.
2. `FLAX_*` environment variables.
3. Project `.flax/cli.json`.
4. User CLI configuration.
5. Built-in defaults.

### Proposed user paths

| Data | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Config | `%APPDATA%\Flax\CLI\config.json` | `~/Library/Application Support/Flax/CLI/config.json` | `$XDG_CONFIG_HOME/flax/config.json` |
| State | `%LOCALAPPDATA%\Flax\CLI\state` | `~/Library/Application Support/Flax/CLI/state` | `$XDG_STATE_HOME/flax` |
| Cache | `%LOCALAPPDATA%\Flax\CLI\cache` | `~/Library/Caches/Flax/CLI` | `$XDG_CACHE_HOME/flax` |
| Runtime IPC | `%LOCALAPPDATA%\Flax\CLI\runtime` | `~/Library/Caches/Flax/CLI/runtime` | `$XDG_RUNTIME_DIR/flax-$UID` |

The CLI already uses platform-appropriate application paths. Shared Launcher registry ownership still requires a formal contract before managed distribution ships.

### Compatibility and capability negotiation

Existing FlaxEditor and Flax.Build command lines remain supported. Flax CLI uses legacy adapters when a selected engine lacks newer protocols.

```json
{
  "capabilities": {
    "legacyEditorArgs": true,
    "oneShotRequests": true,
    "assetRequests": true,
    "typedCommands": false,
    "editorBridge": false,
    "playerBridge": false,
    "mcp": false
  }
}
```

Capabilities are discovered, not inferred only from engine version. Unsupported structured operations fail explicitly instead of falling back to log scraping or arbitrary code execution.

### Process isolation

Flax.Build remains out of process because it loads project build rules, mutates global build configuration, probes SDKs, and can encounter assembly-version conflicts.

Game Cooker, content operations, scene authoring, and project commands remain inside FlaxEditor because they depend on Editor services, scripting domains, asset databases, scenes, platform tools, and main-thread state.

### Cancellation

- Stop accepting new work on Ctrl+C/SIGINT or termination.
- Signal the active request/child immediately.
- Allow a bounded graceful period, then terminate only the owned process tree.
- Preserve detached jobs unless cancellation was requested.
- Clean request/result staging files in a finally path.
- Return 130/143 for process-level interruption.
- Use cancelled job state for detached/live operations.

### Versioning

Version independently:

- CLI release;
- result envelope;
- one-shot request;
- command schema;
- bridge protocol;
- instance manifest;
- job/event stream;
- release manifest;
- project conversion manifest.

Minor additions are backward compatible. Removed/renamed fields or changed meanings require a major schema version or an explicit compatibility adapter.

## Validation strategy by phase

| Phase | Required validation |
| --- | --- |
| 0 | Parser, output envelope, exit mapping, timeout, cancellation, atomic-file, and trace-redaction tests. |
| 1 | Engine resolution/lock tests, registry tests, fake-process argument tests, local engine/project smoke tests. |
| 2 | Build and asset request contract tests; headless Editor smoke tests for create/import/set/save/build. |
| 3 | Command schema/argument/access tests, cancellation, structured progress, and headless end-to-end tests. |
| 4 | IPC auth, stale descriptor, multi-instance selection, reconnect, compatibility, console cursor, and capture tests. |
| 5 | Scene/Actor/Prefab round trips, Undo/dry-run, reference identity, batch atomicity, importer and bake tests. |
| 6 | Input reset, fixed-step/replay, event ordering, screenshot, collision, and Cube 4 end-to-end tests. |
| 7 | CLI/MCP schema parity, permission propagation, client configuration, disconnect, and resource-size tests. |
| 8 | Signed manifest, resume, checksum, traversal, rollback, repair, concurrent install, and registry consistency tests. |
| 9 | Test adapter lifecycle, cancellation, log cursor, support-bundle redaction, and offline behavior tests. |

Repository build/test commands remain governed by `AGENTS.md` and CI. Documentation edits do not authorize rebuilding the Editor while a user session may be active.

## Risks and mitigations

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Scene automation mutates large project state | High | Typed schema, dry-run, generated root, stable source IDs, temporary output/transaction, validation before save. |
| A custom command or eval becomes arbitrary execution | High | Explicit registration and access metadata for ordinary commands; no default eval; development-only session unlock, visible state, source audit, expiration, and authenticated local transport for eval. |
| Object names are ambiguous | High | Stable Actor/asset handles plus hierarchy paths; names are filters only. |
| Player input can remain active after failure | High | Bounded durations, cancellation cleanup, session ownership, automatic input reset. |
| Bridge expands attack surface | High | Current-user IPC ACLs/modes, per-instance token, no LAN listener, canonical paths. |
| Launcher registry is not a public contract | High | Shared library/API or migrated schema before remote install writes. |
| Release/platform feed is undefined | High | Keep Phase 8 deferred; require signed versioned catalogs and checksums. |
| Old engines lack typed protocols | Medium | Capability negotiation and explicit reduced functionality. |
| Long operations outlive CLI connections | Medium | Job IDs, status/wait/cancel, durable bounded results. |
| Unity CLI beta behavior changes | Medium | Apply the local verification rule at each phase; do not promise command-level compatibility. |
| No universal Flax project tests | Medium | Adapter interface plus project-defined typed tests/playtests. |
| Huge conversion manifests exceed CLI limits | Medium | Use versioned input files/resources, streaming parsing, and bounded result summaries. |

## Immediate implementation order

1. **Done in source:** add shared command descriptors, schemas, argument binding, access metadata, and result DTOs.
2. **Done in source:** add `operation: "command"` to the one-shot Editor request service.
3. **Done in source:** implement `flax commands list/info` and `flax command` with one-shot and live routing.
4. Validate the one-shot command service against a rebuilt Editor.
5. Complete the Phase 4 running-Editor bridge with Player registration, streaming progress, and live capture/performance capabilities.
6. **Done in source:** add the Phase 5 core scene/Actor/component/Prefab commands and project-scoped authoring root.
7. Add deterministic Phase 6 input and observation primitives.
8. Expose the stable registry through Phase 7 MCP.

## Sources and local reference commands

### Unity sources

- **[U1]** Unity CLI introduction: https://docs.unity.com/en-us/unity-cli/unity-cli
- **[U2]** Unity CLI reference: https://docs.unity.com/en-us/unity-cli/unity-cli-reference
- **[U3]** Unity Pipeline package: https://docs.unity.com/en-us/unity-production-pipeline/local-tools-cli/unity-pipeline-package
- **[U4]** Unity CLI release notes: https://docs.unity.com/en-us/unity-cli/release-notes

### Locally verified Unity commands

- **[UL1]** `unity --version` — `1.0.0-beta.3` on August 6, 2026.
- **[UL2]** `unity pipeline list --json` — connected Pipeline `0.4.0-exp.1` and local reachability fields.
- **[UL3]** `unity status --json` — project, version, PID, port, and ready state.
- **[UL4]** `unity command --help` — project, Player runtime, runtime-path, timeout, and typed execution.
- **[UL5]** `unity run --help` — one-shot `--command` execution with schema-bound arguments after `--`.
- **[UL6]** `unity list --json` — 140 tools and their schemas in the connected instance.
- **[UL7]** `unity mcp --help` and `unity mcp configure --list` — stdio MCP and client configuration including Codex.
- **[UL8]** `unity projects --help` — full project registry/lifecycle surface.
- **[UL9]** `unity templates --help` — list/info/create/edit/delete/location lifecycle.

### Flax sources

- **[F1]** Command line access: https://docs.flaxengine.com/manual/editor/advanced/command-line-access.html
- **[F2]** Flax.Build: https://docs.flaxengine.com/manual/editor/flax-build/index.html
- **[F3]** Game Cooker: https://docs.flaxengine.com/manual/editor/game-cooker/index.html
- **[F4]** Custom engine build: https://docs.flaxengine.com/manual/editor/advanced/custom-engine.html
- **[F5]** Installing the Flax Launcher: https://docs.flaxengine.com/manual/get-started/get-flax.html
- **[F6]** Create new project: https://docs.flaxengine.com/manual/get-started/create-a-project.html
- **[F7]** Flax.Build CommandLine.cs: https://github.com/FlaxEngine/FlaxEngine/blob/master/Source/Tools/Flax.Build/CommandLine.cs
- **[F8]** Flax.Build Program.cs: https://github.com/FlaxEngine/FlaxEngine/blob/master/Source/Tools/Flax.Build/Program.cs
- **[F9]** Editor ProjectInfo.cs: https://github.com/FlaxEngine/FlaxEngine/blob/master/Source/Editor/ProjectInfo.cs
- **[F10]** GameCooker.cpp: https://github.com/FlaxEngine/FlaxEngine/blob/master/Source/Editor/Cooker/GameCooker.cpp
- **[F11]** Repository build/test guidance: https://github.com/FlaxEngine/FlaxEngine/blob/master/AGENTS.md
