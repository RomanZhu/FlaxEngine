# Flax authoring command map

## Scenes

```powershell
flax scenes create --path Scenes\Automation.scene --open --project <project> --engine <engine> --one-shot --json
flax scenes open --scene Scenes\Automation.scene --project <project> --engine <engine> --live-only --json
flax scenes list --project <project> --engine <engine> --live-only --json
flax scenes hierarchy --project <project> --engine <engine> --live-only --json
flax scenes active get --project <project> --engine <engine> --live-only --json
flax scenes active set --scene <loaded-scene-id> --project <project> --engine <engine> --live-only --json
flax scenes build-list list --project <project> --engine <engine> --one-shot --json
flax scenes build-list add --scene Scenes\Automation.scene --project <project> --engine <engine> --one-shot --json
flax scenes build-list remove --scene Scenes\Automation.scene --project <project> --engine <engine> --one-shot --json
flax scenes save --project <project> --engine <engine> --live-only --json
```

Flax uses the first loaded scene (`Level.GetScene(0)`) as the primary authoring
scene and default Actor parent.
`scenes active set` reloads the requested scene first and preserves the other
loaded scenes. The build list is also Flax-native: `GameSettings.FirstScene` is
the required startup scene and `BuildSettings.AdditionalScenes` is the ordered
set of extra cooked roots.

## Actors and Script components

- Query: `actors find|get`
- Lifecycle: `actors create|create-batch|delete|rename`
- State: `actors transform|parent|active|tag|layer`
- Scripts: `actors component add|remove|get|set`

Example:

```powershell
flax actors create --type FlaxEngine.EmptyActor --name GeneratedActor --project <project> --engine <engine> --live-only --json
flax actors find --name GeneratedActor --project <project> --engine <engine> --live-only --json
```

Use `commands info actors.transform` and structured JSON input for vector values or batches.

## Settings and scene-data baking

Use `settings list` to discover stable Flax groups, `settings get|schema` to inspect them, and
`settings set --group <group> --values <json-object> [--dry-run]` for partial persisted edits.
For reliable cross-shell JSON, put `{ "group": "game", "values": { ... } }` in a file and
pass it with `settings set --input <file.json>`; omitted fields remain unchanged.
Platform settings are included when their concrete Flax API type is available in
the current Editor build.
Use `bake status` before starting a builder. The available native operations are
`bake lighting start|cancel|clear`, `bake navmesh start|clear`, `bake probes start`,
`bake csg start`, `bake scenes start|cancel`, and `bake sdf start`. Wait for idle
before starting another scene-data operation; the Editor returns a structured
busy error instead of overlapping builders.

For diagnostics, unlock the live Development Editor for a short session with
`dev unlock-eval`, then use `dev eval --code` or `dev eval-file --path`. Eval is
expression-only and read-oriented; it rejects arbitrary C#, assignment,
filesystem/process access, and open-world mutations.

For trusted development-only open-world work, use the separate explicit flow:

```powershell
$unlock = flax dev unlock-csharp --project <project> --live-only --json
flax dev eval-csharp --code "return Level.ScenesCount;" --token <token> --project <project> --live-only --json
```

The returned token is short-lived. Source hashes are written to the project audit
log. Execution occurs inside the Editor process with its privileges and is not a
sandbox.

## Visject, Material, and Animation Graphs

```powershell
flax visject groups list --project <project> --engine <engine> --live-only --json
flax visject asset inspect --asset Materials\Player.flax --kind material --project <project> --live-only --json
flax visject validate --asset Animations\Locomotion.flax --kind animation --project <project> --live-only --json
flax visject node add --asset Materials\Player.flax --kind material --group <group-id> --type <type-id> --x 0 --y 0 --project <project> --live-only --json
flax visject node set --asset Materials\Player.flax --node <node-id> --index 0 --value-json 1 --project <project> --live-only --json
flax visject connect --asset Materials\Player.flax --from-node <node-id> --from-box <box-id> --to-node <node-id> --to-box <box-id> --project <project> --live-only --json
```

These routes load the actual Flax `MaterialSurface` or `AnimGraphSurface`, use
the discovered node archetypes, and save through the native asset serializer.
`visject.node.remove` is destructive and requires `--yes`.

## Development Player and runtime input

```powershell
flax player status --project <project> --json
flax player pause --project <project> --json
flax player input key --key A --state press --project <project> --json
flax player input pointer --state move --x 640 --y 360 --project <project> --json
flax player input pointer --state relative --dx 12 --dy -4 --project <project> --json
flax player input inspect --key W --axis "Mouse X" --axis "Mouse Y" --project <project> --json
flax player input reset --project <project> --json
flax player quit --project <project> --json
```

`flax runtime input ...` is an equivalent alias when a caller models input as a
runtime subsystem rather than Player control.

The same authenticated actions work against the Editor's embedded Player and a
valid standalone Development Player manifest. Key/pointer events are injected
into Flax's input devices without moving the user's cursor. Relative pointer
events feed Flax's mouse-delta path; `input inspect` reports device availability,
input mappings, and requested virtual-input samples. Gamepad and action-map
injection currently returns a stable unsupported error.

## Durable detached work and save-aware shutdown

```powershell
flax compile <project> --target FlaxEditor --detach --json
flax jobs list --project <project> --json
flax jobs wait <job-id> --project <project> --json
flax jobs cancel <job-id> --yes --project <project> --json
```

Job records and stdout/stderr logs live under `<project>\.flax\jobs` and survive
the calling shell. Before rebuilding or closing an Editor, use exactly one of
`flax editor close --save` or `flax editor close --discard`; never terminate a
dirty Editor directly because the native save modal can block automation.

## Captures and deterministic playtests

```powershell
flax capture viewport --to Artifacts\viewport.png --project <project> --engine <engine> --live-only --json
flax playtest begin --project <project> --engine <engine> --json
flax playtest find --name Camera --project <project> --engine <engine> --json
flax playtest wait --name Player --timeout-seconds 5 --project <project> --engine <engine> --json
flax playtest assert --name Camera --active true --project <project> --engine <engine> --json
flax playtest capture game --to Artifacts\playtest.png --project <project> --engine <engine> --json
flax playtest end --project <project> --engine <engine> --json
```

`playtest begin` uses `GameSettings.FirstScene` when no scene is loaded and waits
for the Editor's actual play-state transition. Runtime queries return stable
scene/Actor IDs and exact name/type/active filters. The built-in layer intentionally
does not emulate OS keyboard/mouse input or invent collision events.

## Prefabs

Use `prefabs create|instantiate|variant|apply|revert|unpack|save`. Resolve Actor and Prefab IDs before mutation. `revert` and `unpack` are destructive and require `--yes`.

## Assets and project commands

Use `assets` for content lifecycle operations. Use `commands list`, `commands info`, and `command <dotted-name>` for built-in, plugin, or project-registered typed commands. Prefer `--input <file.json>` for complex payloads.

## Safety and persistence

- Authoring paths are confined to project Content and the configured authoring root.
- Successful scene mutations save affected scenes synchronously.
- Non-additive scene transitions save genuinely dirty scenes without an interactive prompt.
- Actor identity includes scene ID, Actor ID, hierarchy path, type, and name.
- Destructive commands require verified targets and `--yes`.
- Before closing a live Editor, use exactly one of `editor close --save` or
  `editor close --discard`; do not terminate the process directly while scenes
  may be dirty because the native save dialog is modal.
