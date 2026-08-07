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

**Status:** Implemented in source

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

**Status:** Partial, implemented in source

| Capability | Current command |
| --- | --- |
| Discover authenticated Editors | `flax status` |
| Editor state | `flax editor status` |
| Play controls | `flax editor play\|pause\|resume\|stop\|step` |
| Focus | `flax editor focus` |
| Save | `flax editor save-all` |
| Script compile | `flax editor recompile` |
| Structured console read | `flax console` |
| Typed live invocation | `flax command`, with `--instance` and `--live-only` |

Windows uses a current-user named pipe. Linux and macOS use a user-owned Unix domain socket. The manifest and authentication token are validated before connection.

### Scene, Actor, Script component, and Prefab authoring

**Status:** Implemented in source

| Area | Commands |
| --- | --- |
| Authoring root | `flax authoring-root get\|set` |
| Scenes | `flax scenes list\|create\|open\|save\|hierarchy` |
| Actor queries | `flax actors find\|get` |
| Actor lifecycle | `flax actors create\|create-batch\|delete\|rename` |
| Actor state | `flax actors transform\|parent\|active\|tag\|layer` |
| Script components | `flax actors component add\|remove\|get\|set` |
| Prefabs | `flax prefabs create\|instantiate\|variant\|apply\|revert\|unpack\|save` |

Actor identity contains scene ID, Actor ID, hierarchy path, type, and name. Core scene mutations save affected scenes synchronously. Non-additive scene transitions save only genuinely dirty scenes and never invoke an interactive save prompt.

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
| `mcp serve\|configure` | Project the same command registry as MCP tools/resources. |

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
| `scenes list\|create\|open\|save\|hierarchy` | Implemented in source |
| `scenes close\|reload` | Planned |
| `scenes active get\|set` | Planned; define Flax multi-scene semantics first |
| `scenes build-list list\|add\|remove` | Planned; requires a stable Flax persistence contract |
| `scenes dirty` | Planned |
| `scenes diff\|validate\|fix` | Planned |
| `scenes external-actors status\|convert` | Planned Flax-specific extension |

### 4. Actors, scripts, properties, selection, and search

**Target status:** Partial

| Target commands | Status |
| --- | --- |
| `actors find\|get\|create\|create-batch\|delete\|rename` | Implemented in source |
| `actors transform\|parent\|active\|tag\|layer` | Implemented in source |
| `actors component add\|remove\|get\|set` | Implemented in source for Script components |
| `actors duplicate\|move-to-scene` | Planned |
| `actors properties get\|set\|schema` | Planned generalized reflected properties |
| `actors references\|dependencies` | Planned |
| `selection get\|set\|clear` | Planned live-Editor surface |
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

**Target status:** Planned

Target surface:

- `materials get\|set\|create\|validate`;
- `materials parameters list\|get\|set`;
- `shaders list\|info\|metadata\|recompile`;
- `material-graphs create\|get\|node add\|remove\|connect\|disconnect\|set`;
- `textures inspect\|convert\|compress` where Editor APIs are stable;
- renderer and post-process settings inspection.

Flax Material Graph and shader metadata must use Flax's actual graph model rather than copying Unity Shader Graph schemas.

### 8. Animation, Animation Graph, and sequence authoring

**Target status:** Planned, adapted from Unity

Target surface:

- animation clip create/get and curve set/remove;
- Animation Graph create/inspect/edit/validate;
- state machine, transition, parameter, and blend-space editing using Flax node types;
- skeleton, animation event, retarget, and root-motion settings;
- Scene Animation create/inspect/track/clip/key editing;
- animation playback and sampling for validation.

Unity Animator Controller and Timeline names are reference concepts, not Flax schemas.

### 9. Visject and specialized asset authoring

**Target status:** Planned; Flax-specific advantage

The same graph transaction model should support:

- Animation Graph;
- Material Graph;
- Particle Emitters and Particle Systems;
- Behavior Trees;
- Visual Scripts;
- Scene Animation;
- other Visject-backed plugin assets.

Generic operations should include graph schema discovery, nodes, boxes/pins, connections, values, parameters, validation, layout metadata, compilation, and diffing. Specialized commands may wrap common high-level patterns without replacing the generic graph API.

### 10. Project and engine settings

**Target status:** Planned

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

**Target status:** Planned

Every long-running operation needs `start`, `status`, `wait`, `cancel`, and `clear` where supported:

- lighting and probes;
- navigation/NavMesh;
- occlusion;
- collision and physics data;
- CSG;
- environment and reflection captures;
- shader and content preprocessing.

Collision rebuild is a first-class Flax requirement for deterministic template playtests.

### 13. Editor lifecycle, UI, navigation, and observability

**Target status:** Partial

Target surface:

- Editor status/focus/play/pause/resume/stop/step;
- save-all and dirty-resource inspection;
- selection and content-browser navigation;
- invoke a registered menu action by stable ID;
- structured console read/follow/clear;
- compile/domain-reload status;
- performance statistics and profiler samples;
- editor window and viewport enumeration;
- health checks and support bundles.

UI automation should be the fallback for UI-only behavior, not the primary way to manipulate engine state.

### 14. Capture and evidence

**Target status:** Planned

Target commands:

- `capture game`;
- `capture viewport`;
- `capture editor-window`;
- `capture ui-element`;
- `capture runtime-ui`;
- frame sequences and bounded video capture where supported.

Results should return dimensions, format, timestamp/frame, source instance, saved path, and optionally bounded inline image data for MCP.

### 15. Tests, assertions, and deterministic playtesting

**Target status:** Planned

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

### 16. Development Player bridge

**Target status:** Planned

A development Player should expose the same registry subset through authenticated local IPC. It must be compiled out or disabled in shipping builds unless a product explicitly supplies a hardened remote-development policy.

Target capabilities:

- runtime discovery and status;
- logs and event streams;
- Actor/component inspection;
- input simulation;
- time scale, frame rate, pause, step, and quit;
- capture and performance counters;
- typed game-specific commands;
- jobs and cancellation;
- optional managed hot reload and eval under explicit development policy.

### 17. Runtime input

**Target status:** Planned Flax extension

Target commands:

- `runtime input key` with down/up/press and frame duration;
- `runtime input pointer` with move/button/scroll;
- `runtime input gamepad` with buttons/axes;
- `runtime input action` for project-defined input actions;
- `runtime input reset` to guarantee cleanup;
- input recording/replay with deterministic timestamps or frames.

Input must be injected below gameplay bindings but above platform hardware where possible, so tests exercise the real Flax input stack without moving the user's physical cursor.

### 18. Logs, events, performance, and tracing

**Target status:** Partial

Target surface:

- cursor-based Editor and Player logs;
- follow/stream with severity, category, object handle, and stack trace filters;
- structured engine events;
- job progress events;
- frame CPU/GPU/memory/render/physics statistics;
- bounded profiler captures;
- command audit and transaction logs;
- redacted support bundles.

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

**Target status:** Proposed opt-in development capability; excluded from the default trusted surface

Eval is the escape hatch that prevents the fixed catalog from becoming the limit of what an agent can do. A client can discover ordinary typed commands first, then use live C# for a missing operation instead of waiting for a new built-in tool.

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

Required Flax eval controls:

- development Editor or development Player only;
- not advertised unless enabled by project and current session policy;
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

**Target status:** Mixed; package/feed operations deferred

Target surface:

- plugin/package list, search, add, remove, resolve, and status;
- project list/create/clone/open/pin/upgrade/export/import;
- template list/info/create/edit/delete/location;
- engine release and platform package list/install/update/remove;
- offline caches, signatures, hashes, and provenance.

Local engine/project registry operations exist. Remote release and package mutation must wait for stable signed Flax feed and plugin contracts.

### 22. MCP and agent integration

**Target status:** Planned

MCP must be a generated facade over the registry:

- commands become tools;
- projects, instances, assets, logs, captures, and jobs may also become resources;
- schemas and access metadata remain identical to CLI JSON;
- destructive confirmation and eval policy cannot be bypassed through MCP;
- progress and cancellation map to MCP primitives;
- large images and artifacts use bounded resources rather than giant JSON payloads.

An agent should need only a small orientation skill: discover capabilities, inspect a schema, invoke it, and use eval only when the catalog has a genuine hole.

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
