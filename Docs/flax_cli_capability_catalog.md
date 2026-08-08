# Flax CLI Capability and Parity Catalog

**Purpose:** Define the complete automation surface Flax CLI should expose, record what exists today, compare it with the installed Unity Pipeline reference, and identify where Flax can provide deeper engine-native integration.

**Primary implementation:** `FlaxCLI/` and `Source/Editor/CLI/`

**Companion design:** `Docs/flax_cli_technical_design.md`

## How to use this catalog

This document separates three different claims:

- **Available:** usable from the standalone CLI without rebuilding the Editor.
- **Implemented in source:** code exists in this checkout, but Editor integration validation still requires a requested rebuild.
- **Target:** the intended stable capability and command surface.

The catalog is broader than a list of subcommands. A capability is complete only when it has:

- a discoverable, versioned input/output schema;
- one-shot and live routing where the operation supports both;
- stable object or asset identity;
- deterministic save, Undo, confirmation, dry-run, and rollback behavior;
- progress, cancellation, timeout, and artifacts for long-running work;
- tests against the Flax API and persistence behavior;
- an MCP projection generated from the same registry rather than a second implementation.

## Status legend

| Status | Meaning |
| --- | --- |
| Available | Implemented and usable in the standalone CLI. |
| Implemented in source | Source exists; rebuilt-Editor integration validation remains. |
| Partial | A useful subset exists, but the target capability is incomplete. |
| Planned | Flax API mapping and implementation remain. |
| Adapted | Unity has an analogous tool, but Flax requires different concepts or schemas. |
| Opt-in | Powerful development capability that must not be enabled by default. |
| Deferred | Blocked on a stable product, feed, platform, or package contract. |
| Excluded | Deliberately not part of the default trusted surface. |

## Verified reference baseline

The local reference project is `F:\GameProject\Hypnothermia\Svalker`.

| Fact | Verified value |
| --- | --- |
| Unity project version | `6000.4.3f1` |
| Unity Pipeline package | `com.unity.pipeline` `0.4.0-exp.1` |
| Separate MCP package | `com.anklebreaker.unity-mcp` from GitHub |
| Pipeline source command registrations | 151 unique `[CliCommand]` methods in the installed package |
| Previously connected command catalog | 140 commands from `unity list --json` on August 6, 2026 |
| Unity CLI previously inspected | `1.0.0-beta.3` |
| Unity CLI availability now | Package present; the `unity` executable is not currently on `PATH` |

The 151 source registrations are a superset of the previously connected 140-command catalog. Runtime-only, platform-gated, package-version, or discovery filtering may account for the difference. Re-run `unity list --json` after restoring the Unity CLI before treating either count as a release guarantee.

## Integration model

```text
Terminal / CI / Codex / MCP
             |
             v
        standalone flax CLI
             |
      +------+-------------------+
      |                          |
      v                          v
one-shot headless Editor   authenticated live bridge
                                 |
                         +-------+--------+
                         |                |
                         v                v
                   running Editor   development Player
                         |
       +-----------------+-------------------+
       |                 |                   |
       v                 v                   v
  typed registry   Flax authoring       jobs/events/
                   transaction          artifacts
       |
       v
Flax.Build, Game Cooker, Content DB, scenes, Actors,
Prefabs, Visject, scripting, physics, renderer, profiler
```

The standalone CLI remains an orchestrator. Engine-owned state is read or mutated inside FlaxEditor or a development Player, using native Flax APIs rather than editing serialized files behind the engine's back.

## Current Flax CLI surface

### Standalone host, engines, projects, and builds

**Status:** Available

| Area | Commands |
| --- | --- |
| Engine registry | `flax engines list\|add\|remove\|default\|info` |
| Project engine | `flax engine pin\|unpin` |
| Project registry | `flax projects list\|add\|remove\|info\|size` |
| Editor launch | `flax open`, `flax play` |
| Project generation | `flax generate` |
| Flax.Build | `flax compile`, `flax clean` |
| Game Cooker | `flax build` |
| Diagnostics | `flax doctor`, `flax logs`, `flax env`, `flax status` |
| Configuration | `flax config`, `flax completion` |

Flax-specific advantages include deterministic custom-engine selection, source-fork pinning, Flax.Build target execution, and Game Cooker integration.

### Assets and content database

**Status:** Available through the one-shot Editor protocol

`flax assets` supports:

- `list`, `types`, and `info`;
- `create` and `mkdir`;
- `import`, `reimport`, and `export`;
- `duplicate`, `move`, and `rename`;
- confirmed `delete`;
- public property `get` and `set`;
- explicit `save`.

All asset operations resolve through Flax's content database and asset proxies rather than treating `.flax`, `.prefab`, or other content files as ordinary JSON.

### Typed command registry

**Status:** Available through one-shot and live Editor execution

| Capability | Commands/API |
| --- | --- |
| Discover catalog | `flax commands list` |
| Inspect exact schema | `flax commands info <name>` |
| Invoke command | `flax command <name>` |
| Complex input | `--arguments <json>` or `--input <file.json>` |
| Registration | `[CliCommand]`, `[CliOption]` |
| Structured result | `CliCommandResult` |
| Progress/warnings/cancellation | `CliCommandContext` |
| Access policy | read-only, mutating, destructive |

The same registry is used by one-shot and live execution. It is the future source for MCP tool generation.

### Running Editor bridge

**Status:** Available for the Development Editor; Player/runtime groups are capability-gated

| Capability | Current command |
| --- | --- |
| Discover authenticated Editors | `flax status` |
| Editor state | `flax editor status` |
| Play controls | `flax editor play\|pause\|resume\|stop\|step` |
| Focus | `flax editor focus` |
| Save | `flax editor save-all` |
| Explicit shutdown policy | `flax editor close --save\|--discard` |
| Script compile | `flax editor recompile` |
| Structured console read/clear | `flax console`, `flax console clear` |
| Performance snapshot | `flax performance` |
| Actor selection | `flax selection get\|set\|clear` |
| Typed live invocation | `flax command`, with `--instance` and `--live-only` |
| Development Player control | `flax player status\|pause\|resume\|step\|quit` |
| Virtual runtime input and diagnostics | `flax player input key\|pointer\|inspect\|reset` |
| Durable detached work | `--detach` on `compile`, `build`, and `command`; `flax jobs ...` |
| Signed local feeds | `flax feeds verify\|list\|install` |
| Arbitrary C# (opt-in) | `flax dev unlock-csharp`, `dev eval-csharp`, `dev eval-csharp-file` |
| Visject graphs | `flax visject groups\|asset\|validate\|node\|connect\|disconnect` |

Windows uses a current-user named pipe. Linux and macOS use a user-owned Unix domain socket. The manifest and authentication token are validated before connection.

The rebuilt Development Editor advertises the capability groups `authoring`,
`settings`, `bake`, `eval`, `evalCSharp`, `visject`, `player`, `runtimeInput`,
`playtest`, and `close` in addition to the core bridge controls. A standalone
Development Player publishes the compatible `player`, `runtimeInput`,
`playtest`, `performance`, and `close` subset when it starts successfully.
Clients must negotiate these manifest capabilities before invoking their
commands. Missing capability is a deterministic `FLX-BRIDGE-CAPABILITY-0004`,
not a silent fallback.

### Scene, Actor, Script component, and Prefab authoring

**Status:** Partial, core scene/Actor/active-scene/build-list slice integration-validated

| Area | Commands |
| --- | --- |
| Authoring root | `flax authoring-root get\|set` |
| Scenes | `flax scenes list\|create\|open\|close\|reload\|save\|dirty\|hierarchy` |
| Active authoring scene | `flax scenes active get\|set` |
| Cook/build scene list | `flax scenes build-list list\|add\|remove` |
| Actor queries | `flax actors find\|get` |
| Actor lifecycle | `flax actors create\|create-batch\|delete\|rename` |
| Actor state | `flax actors transform\|parent\|active\|tag\|layer` |
| Script components | `flax actors component add\|remove\|get\|set` |
| Prefabs | `flax prefabs create\|instantiate\|variant\|apply\|revert\|unpack\|save` |

Actor identity contains scene ID, Actor ID, hierarchy path, type, and name. Core scene mutations save affected scenes synchronously. Non-additive scene transitions save only genuinely dirty scenes and never invoke an interactive save prompt.

Integration validation on August 7, 2026 used a newly created sample project and a rebuilt Development Editor. One-shot catalog discovery, `cli.ping`, and scene creation succeeded; the visible Editor then registered an authenticated bridge, opened two scenes, reordered the primary authoring scene, created and saved an Actor, and returned a hierarchy containing that Actor. The same run validated build-list persistence, settings get/schema/diff/dry-run, every bake operation and status route, guarded eval/eval-file, performance capability negotiation, Actor selection, dirty-state inspection, scene reload, viewport/game capture, deterministic playtest begin/find/wait/assert/end, stdio MCP initialize/tools/list/tools/call, local project creation, and both explicit close policies. `editor close --save` and `editor close --discard` exited dirty scenes without the native save modal. Component and Prefab commands remain source-implemented pending equivalent end-to-end scenarios.

## Target capability catalog

### 1. Discovery, schemas, help, and capability negotiation

**Target status:** Partial

| Target Flax surface | Purpose |
| --- | --- |
| `commands list\|info` | Discover every built-in, plugin, project, Editor, and Player command. |
| `commands schema <name>` | Return complete nested JSON Schema with examples and constraints. |
| `capabilities` | Report transport, Editor/Player state, platform, build, permissions, and feature versions. |
| `instances list\|info` | Enumerate Editors and Players without guessing among matches. |
| `events list\|subscribe` | Discover and stream supported event types. |
| `mcp` | Run a stdio JSON-RPC MCP facade over the same typed command registry; client configuration remains deferred. |

Schemas must represent nested DTO properties, nullable “omitted means unchanged” channels, typed handles, enums, arrays, defaults, validation constraints, destructive access, dry-run support, execution location, and artifact types.

### 2. Shared authoring transaction

**Target status:** Partial; command-local behavior exists

Every mutating Editor command should execute inside a first-class transaction:

1. Resolve and validate every handle, path, type, and property.
2. Produce a dry-run change plan when requested.
3. Begin one Flax Undo group when supported.
4. Track exactly which scenes, Actors, scripts, assets, and settings changed.
5. Roll back on failure in atomic mode.
6. Save exactly the affected persistent resources synchronously.
7. Return before/after handles, revisions, saved state, warnings, and artifacts.

Target controls:

- `--dry-run` for useful previews;
- `--yes` for destructive operations;
- `--atomic` by default for batches;
- explicit `--best-effort` opt-in;
- `--no-save` only for an explicitly selected live interactive workflow;
- `transactions list\|info\|undo` for auditable live sessions.

### 3. Scene lifecycle and hierarchy

**Target status:** Partial

| Target commands | Status |
| --- | --- |
| `scenes list\|create\|open\|close\|reload\|save\|dirty\|hierarchy` | Available; create/open/close/reload/dirty/hierarchy integration-validated |
| `scenes active get\|set` | Available; Adapted to Flax: the first loaded scene is the primary authoring scene and `set` reloads it first while preserving the remaining loaded scenes |
| `scenes build-list list\|add\|remove` | Available; Adapted to Flax `GameSettings.FirstScene` plus ordered `BuildSettings.AdditionalScenes` |
| `scenes dirty` | Available; reports only genuinely edited loaded scenes |
| `scenes diff\|validate\|fix` | Planned |
| `scenes external-actors status\|convert` | Planned Flax-specific extension |

### 4. Actors, scripts, properties, selection, and search

**Target status:** Partial

| Target commands | Status |
| --- | --- |
| `actors find\|get\|create\|create-batch\|delete\|rename` | Available; create integration-validated |
| `actors transform\|parent\|active\|tag\|layer` | Available; broader integration coverage pending |
| `actors component add\|remove\|get\|set` | Implemented in source for Script components |
| `actors duplicate\|move-to-scene` | Planned |
| `actors properties get\|set\|schema` | Planned generalized reflected properties |
| `actors references\|dependencies` | Planned |
| `selection get\|set\|clear` | Available and live integration-validated |
| `search actors\|assets\|code\|references` | Planned unified search |

Flax should expose Actors and Scripts honestly. It should not rename every Flax Script an abstract “component” internally merely for Unity vocabulary parity.

### 5. Prefabs and inherited objects

**Target status:** Core implemented in source

`prefabs create`, `instantiate`, `variant`, `apply`, `revert`, `unpack`, and `save` are implemented in source. Remaining depth:

- inspect override and inheritance state;
- apply or revert selected properties/objects;
- add/remove inherited objects safely;
- compare an instance with its Prefab;
- validate broken links and missing nested Prefabs;
- edit Prefab contents without opening a GUI window.

### 6. Assets, importers, dependencies, and files

**Target status:** Partial

| Target commands | Status |
| --- | --- |
| Generic asset lifecycle | Available |
| `assets find` by path/name/type/ID/tag | Planned expansion |
| `assets dependencies\|references` | Planned |
| `assets batch` | Planned atomic multi-operation transaction |
| `assets import-settings get\|set\|schema` | Planned per importer |
| `assets diff\|validate\|fix` | Planned |
| `files read\|write` | Planned only under confined project roots |
| `content dirty\|save-all\|recompile\|status` | Partial |

Importer schemas should cover models, textures, audio, animation, skeletons, fonts, localization, and any plugin-defined importer with a stable Editor API.

### 7. Materials, shaders, and rendering assets

**Target status:** Visject material graph slice implemented and integration-validated; shader metadata remains planned

Available compatible surface:

- `visject groups list` discovers Flax node groups and archetypes;
- `visject asset inspect --asset <path> --kind material` loads a native `MaterialSurface`;
- `visject validate` loads and validates a graph without mutation;
- `visject node add\|remove\|set` edits nodes and serialized values;
- `visject connect\|disconnect` edits native box connections;
- `materials get\|set\|create\|validate` remains a possible higher-level facade;
- `shaders list\|info\|metadata\|recompile`;
- `material-graphs` aliases may be added after a stable product schema exists;
- `textures inspect\|convert\|compress` where Editor APIs are stable;
- renderer and post-process settings inspection.

The implemented graph routes use Flax's actual node archetypes, boxes, values,
and native surface serializer rather than copying Unity Shader Graph schemas.

### 8. Animation, Animation Graph, and sequence authoring

**Target status:** Animation Graph Visject slice implemented and integration-validated; clip/sequence authoring remains planned

Available compatible surface:

- `visject asset inspect --kind animation` and `visject validate`;
- `visject node add\|remove\|set` and `visject connect\|disconnect` on Animation Graph assets;
- animation clip create/get and curve set/remove;
- Animation Graph create/inspect/edit/validate higher-level wrappers;
- state machine, transition, parameter, and blend-space editing using Flax node types;
- skeleton, animation event, retarget, and root-motion settings;
- Scene Animation create/inspect/track/clip/key editing;
- animation playback and sampling for validation.

Unity Animator Controller and Timeline names are reference concepts, not Flax schemas.

### 9. Visject and specialized asset authoring

**Target status:** Core Material and Animation Graph routes implemented; other Visject assets planned

The implemented graph transaction model supports Material and Animation Graph
assets:

- Animation Graph;
- Material Graph;
- discovered groups/archetypes, node values, boxes, connections, validation, and persistence;

The same model should extend to:

- Particle Emitters and Particle Systems;
- Behavior Trees;
- Visual Scripts;
- Scene Animation;
- other Visject-backed plugin assets.

Generic operations currently include graph schema discovery, nodes, boxes/pins,
connections, values, validation, and native serialization. Layout metadata,
compilation, graph diffing, and additional Visject asset types remain follow-up
work. Specialized commands may wrap common high-level patterns without
replacing the generic graph API.

### 10. Project and engine settings

**Target status:** Available for the reflected stable groups; platform-specific groups remain capability-gated

Target typed settings include:

- game/player and product settings;
- graphics and renderer settings;
- quality/scalability settings;
- physics and collision settings;
- audio settings;
- time settings;
- input settings;
- layers and tags;
- navigation settings;
- build and Cooker settings;
- platform settings when the installed platform package exposes them.

Each settings group needs `get`, `set`, `schema`, `diff`, and dry-run behavior. Omitted fields remain unchanged.

The current stable groups are `game`, `time`, `audio`, `layers`, `physics`, `input`,
`graphics`, `network`, `navigation`, `localization`, `build`, and `streaming`.
`flax settings list` reports the reflected Flax settings type and asset path. `set`
validates public writable fields, applies a partial patch through the Editor-owned
settings object, and saves the correct settings asset; `--dry-run` returns the same
before/after diff without writing. Complex patches should use `flax command ...
--input <file.json>` (or the equivalent typed group command) to avoid shell JSON
quoting differences.

When the installed API exposes them, `settings.list` also advertises concrete
platform groups (`platform.windows`, `platform.uwp`, `platform.linux`,
`platform.android`, `platform.web`, `platform.mac`, `platform.ios`, and
`platform.gdk`). These are runtime-discovered rather than hard-coded requirements,
so a minimal Editor can omit unavailable platform packages honestly.

### 11. Builds, compilation, and target switching

**Target status:** Partial

| Target commands | Status |
| --- | --- |
| Generate projects | Available |
| Compile/clean Flax.Build targets | Available |
| Run Game Cooker build | Available |
| Build status/progress/cancel | Partial |
| List targets/configurations/platforms/architectures | Planned discovery expansion |
| Build preset get/set/schema | Planned |
| Switch build target/status | Planned where meaningful in Flax |
| Script compile/status/wait | Partial |
| Shader/content compile/status/wait | Planned |

### 12. Baking and derived scene data

**Target status:** Adapted; core Editor bake operations are available and capability-gated

Every long-running operation needs `start`, `status`, `wait`, `cancel`, and `clear` where supported:

- lighting and probes;
- navigation/NavMesh;
- occlusion;
- collision and physics data;
- CSG;
- environment and reflection captures;
- shader and content preprocessing.

Collision rebuild is a first-class Flax requirement for deterministic template playtests.

The implemented Flax surface is:

| Operation | Commands | Notes |
| --- | --- | --- |
| Aggregate state | `flax bake status` | Reports active/status/progress for scenes, lighting, NavMesh, probes, CSG, and SDF. |
| Static lighting | `flax bake lighting start\|cancel\|clear` | Uses Flax lightmap bake APIs and saves clear operations. |
| NavMesh | `flax bake navmesh start\|clear` | Uses the Navigation builder and clears native NavMesh data. |
| Environment probes | `flax bake probes start` | Uses active Environment Probes and CaptureScene sky lights. |
| CSG | `flax bake csg start` | Uses the Editor CSG build path. |
| Scene data | `flax bake scenes start\|cancel` | Uses the Editor scene build state machine. |
| Mesh SDF | `flax bake sdf start` | Uses the Editor SDF build path. |

The Editor rejects overlapping scene-data operations with a structured busy error;
device/quality choices remain the native Flax Editor/project settings rather than
Unity lightmapper options.

### 13. Editor lifecycle, UI, navigation, and observability

**Target status:** Partial

Target surface:

- Editor status/focus/play/pause/resume/stop/step;
- save-all and dirty-resource inspection;
- explicit non-interactive shutdown: `flax editor close --save` or `flax editor close --discard`;
- Actor selection is available; content-browser navigation remains planned;
- invoke a registered menu action by stable ID;
- structured console read/follow/clear;
- compile/domain-reload status;
- live performance statistics are available; bounded profiler samples remain planned;
- editor window and viewport enumeration;
- health checks and support bundles.

UI automation should be the fallback for UI-only behavior, not the primary way to manipulate engine state.

### 14. Capture and evidence

**Target status:** Adapted; viewport and game PNG capture are available through the authenticated Editor bridge

Target commands:

- `capture game`;
- `capture viewport`;
- `capture editor-window`;
- `capture ui-element`;
- `capture runtime-ui`;
- frame sequences and bounded video capture where supported.

The implemented core is `flax capture viewport|game --to <project-relative-path>`. Paths are
confined beneath the selected project root and the Editor owns the screenshot operation.
Editor-window/UI-element, sequences/video, dimensions metadata, and bounded inline image
data for MCP remain capability-gated follow-up work.

### 15. Tests, assertions, and deterministic playtesting

**Target status:** Partial; deterministic live-Editor playtest observation is available

Target surface:

- list/run/cancel/status/wait for native and managed tests;
- enter/exit play mode with readiness barriers;
- find runtime Actors by typed handle/query;
- wait for properties, logs, events, collisions, or Actor lifecycle conditions;
- invoke typed gameplay actions;
- inject deterministic keyboard, pointer, gamepad, and action-map input;
- capture evidence;
- assert structured conditions and return reproducible failures;
- restore time scale, focus, cursor, and input state after a run.

The compatible core is `flax playtest begin|end|status|find|wait|assert|capture`.
`begin` opens the persisted startup scene when no scene is loaded, waits for the real
play-mode transition, and `find`/`wait`/`assert` operate on runtime Actor IDs, names,
types, and active state. Raw keyboard/pointer/gamepad injection, collision/event
instrumentation, durable test jobs, and Player-side control remain explicit gaps.

### 16. Development Player bridge

**Target status:** Development Editor embedded-Player control is available; standalone Player bridge is implemented in Development builds and requires a valid running Player for integration

A development Player should expose the same registry subset through authenticated local IPC. It must be compiled out or disabled in shipping builds unless a product explicitly supplies a hardened remote-development policy.

Available compatible capabilities:

- runtime discovery and status;
- logs and event streams;
- Actor/component inspection;
- input simulation;
- time scale, frame rate, pause, step, and quit;
- capture and performance counters;
- typed game-specific commands;
- pause, resume, single-step, quit, and performance status;
- raw virtual keyboard and pointer input;
- optional managed eval under explicit development policy;
- durable jobs remain owned by the standalone CLI and survive Player disconnects.

The standalone bridge is compiled under `FLAX_GAME`, uses authenticated local
named-pipe discovery on Windows, and is disabled from shipping builds. The
sample project used for this integration pass is not a cooked standalone
runtime, so its direct Player launch cannot publish a manifest; the FlaxGame
target itself builds successfully and the embedded Editor Player path is
smoke-tested.

### 17. Runtime input

**Target status:** Raw keyboard and absolute/relative pointer injection plus runtime input diagnostics are available through the Development Editor/Player bridge; gamepad/action synthesis remains unsupported

Available commands:

- `runtime input key` with down/up/press and frame duration;
- `runtime input pointer` with absolute move, relative mouse deltas, button, and scroll;
- `runtime input inspect` with optional repeated `--key`, `--axis`, and `--action` samples;
- `runtime input gamepad` with buttons/axes (reserved; returns unsupported);
- `runtime input action` for project-defined input actions (reserved; returns unsupported);
- `runtime input reset` to guarantee cleanup;
- input recording/replay with deterministic timestamps or frames.

Keyboard and pointer events are injected through Flax's `Keyboard` and `Mouse`
device APIs, below gameplay bindings and without moving the user's physical
cursor. Relative pointer injection queues Flax's relative mouse-move event so
mouse-look axes receive a delta rather than only an absolute cursor position.
The probe also exposes mapping names/details and sampled device/virtual-input
state, allowing a failure to be classified as unavailable device, missing
mapping, absent event state, or a later gameplay-script issue. The bridge exposes
virtual input only; it does not claim OS-level raw hardware control.

### 18. Logs, events, performance, and tracing

**Target status:** Partial; live Editor snapshots available

Target surface:

- cursor-based Editor and Player logs;
- follow/stream with severity, category, object handle, and stack trace filters;
- structured engine events;
- job progress events;
- frame CPU/GPU/memory/render/physics statistics through `flax performance`;
- bounded profiler captures;
- command audit and transaction logs;
- redacted support bundles.

`flax diagnose status` reuses the health checks and `flax diagnose bundle --to
<project-relative.zip>` creates a project-confined, redacted ZIP containing a
manifest and available local log files. Cursor-based follow and rich event
streams remain future work.

### 19. Managed hot reload

**Target status:** Planned and opt-in

The Unity reference compiles edited C# through Roslyn, loads a sibling assembly, and routes tagged methods through an injected registry. Flax should first inspect its managed runtime, assembly load context, scripting reload, and AOT constraints before selecting a design.

Possible Flax surface:

- `dev hotreload apply <file>`;
- `dev hotreload status`;
- `dev hotreload clear`;
- method-level opt-in attributes;
- allowed source roots;
- Mono/CoreCLR-only capability reporting;
- portable PDB support for debugging;
- explicit incompatibility for AOT/unsupported Players.

Hot reload is not the same as ordinary `editor recompile`: it aims to change selected running behavior without restarting play or replacing the whole scripting domain.

### 20. Eval and open-world automation

**Target status:** Opt-in; guarded expression evaluation and explicit arbitrary C# execution are available in Development Editor builds

Eval is the escape hatch that prevents the fixed catalog from becoming the limit of what an agent can do. A client can discover ordinary typed commands first, then use live C# for a missing operation instead of waiting for a new built-in tool.

Flax exposes the bounded `flax dev unlock-eval`, `flax dev eval --code`, and
`flax dev eval-file --path` flow on a live authenticated Development Editor. It
also exposes `flax dev unlock-csharp`, `flax dev eval-csharp --code`, and
`flax dev eval-csharp-file --path`. The latter compiles a short-lived in-memory
assembly with the loaded Flax references, runs it synchronously in-process,
records a source hash in `.flax/cli-csharp-eval.audit.log`, and requires a
short-lived unlock token. It is arbitrary code execution with Editor
privileges, not a sandbox or a replacement for typed mutating commands.

The installed Unity Pipeline implementation:

- exposes `eval` and `eval_file` as discoverable commands;
- wraps caller C# in a generated static `Execute()` method;
- compiles it into an in-memory assembly with Roslyn;
- loads the assembly into the running AppDomain;
- runs it synchronously on Unity's main thread;
- automatically imports `System`, collections, LINQ, `UnityEngine`, and `UnityEditor` in the Editor;
- serializes the return value and compiler diagnostics;
- is compiled only for the Editor or standalone desktop development builds.

This is effectively arbitrary code execution with the privileges of the Editor or Player process. Authentication proves which local client sent the request; it does not sandbox what the code can access. The Unity implementation validates a nominal timeout range, but the inspected execution path invokes compiled code synchronously and cannot safely preempt an infinite loop running on the main thread.

Recommended Flax tiers:

| Tier | Surface | Default |
| --- | --- | --- |
| Typed | Discovered versioned commands and schemas | Enabled |
| Transaction script | A constrained, declarative batch of registered commands | Enabled |
| File script | `flax dev run-script <file.cs>` under allowed roots | Disabled until explicitly enabled |
| Raw eval | `flax dev eval --code ...` | Disabled; session-scoped unlock required |
| Arbitrary C# | `flax dev eval-csharp --code ...` | Disabled; explicit token and Development capability required |

Required Flax eval controls:

- development Editor or development Player only;
- advertised only as a locked opt-in capability group; invocation still requires project/session policy and an explicit unlock;
- live authenticated bridge only by default, never an unnoticed shipping endpoint;
- explicit `flax dev unlock-eval` with expiration and visible Editor/Player indication;
- audit source hash, caller/instance, duration, result, and touched transaction resources;
- source/file size limits and confined `eval_file` roots;
- separate compile and execution budgets;
- execute outside the main thread unless the submitted entry point explicitly requests a bounded main-thread section;
- cooperative cancellation, with process isolation for truly enforceable execution timeouts;
- no claim of sandboxing when running in-process;
- optional namespace/assembly allowlists only as guardrails, not as a security boundary;
- return structured diagnostics and stable handles;
- require explicit mutation transaction APIs for Undo/save tracking when eval changes persistent Editor state.

Eval should fill long-tail holes, prototype future commands, and diagnose unusual projects. Repeated eval workflows should graduate into typed `[CliCommand]` methods so they become discoverable, testable, permissionable, and stable.

### 21. Packages, plugins, projects, templates, and distribution

**Target status:** Mixed; local project/template operations and signed local feed verification/install are available, remote distribution remains deferred

Target surface:

- plugin/package list, search, add, remove, resolve, and status;
- project list/create/clone/open/pin/upgrade/export/import;
- template list/info/create/edit/delete/location;
- engine release and platform package list/install/update/remove;
- offline caches, signatures, hashes, and provenance.

Local engine/project registry operations exist. Remote release and package mutation
must wait for stable Flax service and plugin contracts. The compatible local feed
surface is:

- `flax feeds verify --manifest <file> --signature <detached-signature> --public-key <RSA-PEM>`;
- `flax feeds list` after successful verification;
- `flax feeds install --id <entry> --to <directory> --yes` after verification.

Manifests are canonicalized recursively and verified with RSA-SHA256. Archive
SHA-256 entries are checked before extraction, ZIP traversal is rejected, and
installing always requires `--yes`. Network transport, feed discovery, key
rotation, and remote package resolution remain deferred.

The compatible local lifecycle is deliberately small and safe: `flax projects create`
and `flax new` create an empty project only in a missing or empty directory,
`flax templates list|info` reports the built-in empty template, and existing content
is never overwritten. Feed-backed engine/platform install, package resolution, and
template authoring remain deferred.

### 22. MCP and agent integration

**Target status:** Adapted; stdio MCP is available over the typed command registry

MCP must be a generated facade over the registry:

- commands become tools;
- projects, instances, assets, logs, captures, and jobs may also become resources;
- schemas and access metadata remain identical to CLI JSON;
- destructive confirmation and eval policy cannot be bypassed through MCP;
- progress and cancellation map to MCP primitives;
- large images and artifacts use bounded resources rather than giant JSON payloads.

An agent should need only a small orientation skill: discover capabilities, inspect a schema, invoke it, and use eval only when the catalog has a genuine hole.

The current implementation supports MCP `initialize`, `ping`, `tools/list`, and
`tools/call` over stdin/stdout. A generic `flax_command` tool is always available;
when a project is selected, each valid typed command is also projected as
`flax.command.<dotted-name>`. MCP sessions do not append a normal CLI envelope and
closing stdin does not close the Editor or alter detached state.

## Unity Pipeline 0.4.0-exp.1 source-registration inventory

This appendix records all 151 command names found in the installed package source. It is reference evidence, not a requirement to transliterate Unity concepts into Flax.

| Area | Count | Unity command registrations |
| --- | ---: | --- |
| Animation | 14 | `add_animator_layer`, `add_animator_parameter`, `add_animator_state`, `add_animator_transition`, `add_timeline_clip`, `add_timeline_track`, `create_animation_clip`, `create_animator_controller`, `create_timeline`, `get_animation_clip`, `get_animator_controller`, `get_timeline`, `remove_animation_curve`, `set_animation_curve` |
| Assets | 12 | `copy_asset`, `create_asset`, `create_folder`, `delete_asset`, `find_assets`, `get_import_settings`, `import_asset`, `move_asset`, `read_text_file`, `rename_asset`, `set_import_settings`, `write_text_file` |
| Authoring | 2 | `get_authoring_root`, `set_authoring_root` |
| Baking | 17 | `bake_lighting`, `bake_navmesh`, `bake_navmesh_surfaces`, `bake_occlusion_culling`, `cancel_lighting_bake`, `cancel_navmesh_bake`, `cancel_occlusion_bake`, `clear_baked_lighting`, `clear_navmesh`, `clear_occlusion_culling`, `get_lighting_settings`, `get_navmesh_settings`, `lighting_bake_status`, `navmesh_bake_status`, `occlusion_bake_status`, `set_lighting_settings`, `set_navmesh_settings` |
| Build | 8 | `build`, `build_status`, `get_build_settings`, `list_build_profiles`, `list_build_targets`, `set_build_settings`, `switch_build_target`, `switch_build_target_status` |
| Capture | 2 | `capture_game_view`, `capture_scene_view` |
| Editor core/tests | 15 | `cancel_tests`, `capture_editor_element`, `editor_focus`, `editor_pause`, `editor_play`, `editor_status`, `editor_stop`, `list_tests`, `menu`, `recompile`, `recompile_status`, `run_tests`, `screenshot`, `set_autotick`, `test_status` |
| GameObjects/components | 14 | `add_component`, `create_gameobject`, `create_gameobjects`, `delete_gameobject`, `find_gameobjects`, `get_component_properties`, `remove_component`, `rename_gameobject`, `set_active`, `set_component_properties`, `set_layer`, `set_parent`, `set_tag`, `set_transform` |
| Materials/shaders | 4 | `get_material_properties`, `get_shader_properties`, `list_shaders`, `set_material_properties` |
| Navigation/search | 3 | `get_selection`, `search`, `set_selection` |
| Observability | 3 | `clear_console`, `get_console_logs`, `get_performance_stats` |
| Packages | 6 | `package_add`, `package_list`, `package_remove`, `package_resolve`, `package_search`, `package_status` |
| Prefabs | 7 | `apply_prefab_overrides`, `create_prefab`, `create_prefab_variant`, `instantiate_prefab`, `revert_prefab_overrides`, `save_prefab_contents`, `unpack_prefab` |
| Project settings | 16 | `get_audio_settings`, `get_graphics_settings`, `get_input_settings`, `get_physics_settings`, `get_player_settings`, `get_quality_settings`, `get_tags_layers`, `get_time_settings`, `set_audio_settings`, `set_graphics_settings`, `set_input_settings`, `set_physics_settings`, `set_player_settings`, `set_quality_settings`, `set_tags_layers`, `set_time_settings` |
| Runtime | 15 | `capture_runtime_element`, `cleanup_hotreload`, `console`, `eval`, `eval_file`, `hotreload_status`, `log`, `quit`, `reload_file`, `reload_file_override`, `runtime_status`, `set_target_framerate`, `set_timescale`, `simulate_key`, `simulate_pointer` |
| Scenes | 9 | `add_scene_to_build`, `create_scene`, `get_scene_hierarchy`, `list_open_scenes`, `open_scene`, `remove_scene_from_build`, `save_all`, `save_scene`, `set_active_scene` |
| Scripts | 4 | `attach_script`, `create_script`, `get_serialized_fields`, `set_serialized_field` |

## Flax-beyond-parity priorities

Flax should not stop at matching the Unity list. The highest-value first-party extensions are:

1. One native authoring transaction spanning Undo, dirty tracking, synchronous saves, rollback, audit, and result handles.
2. Stable typed Actor, Script, asset, graph node, job, and runtime handles with revisions.
3. A development Player bridge integrated below gameplay input bindings and above platform hardware.
4. A generic Visject graph API shared by Materials, Animation Graphs, Particles, Behavior Trees, and Visual Scripts.
5. Explicit collision/physics/CSG/navigation rebuild operations for deterministic playtests.
6. Streaming engine events and profiler evidence rather than polling only final state.
7. Custom-engine fork pinning and Flax.Build/Game Cooker control through the same automation plane.
8. Opt-in eval with honest arbitrary-code-execution semantics and a safer declarative transaction script below it.

## Completion criteria

The integration layer reaches parity-and-beyond when:

- the catalog can be queried from CLI and MCP rather than existing only as Markdown;
- every row above is implemented, explicitly deferred, or deliberately excluded with rationale;
- a caller can build content, author scenes/assets, run deterministic gameplay, collect evidence, and package a build without GUI interaction;
- one-shot Editor, live Editor, and development Player use compatible schemas and handles;
- mutations never produce interactive prompts in non-interactive CLI execution;
- unsafe capabilities such as eval are impossible to enable accidentally;
- capability and integration tests verify the advertised surface against a rebuilt Editor and development Player.
