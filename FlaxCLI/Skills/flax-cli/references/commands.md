# Flax CLI command map

## Invocation

```powershell
$cli = ".\FlaxCLI\Flax.CLI\bin\Debug\net8.0\flax.dll"
dotnet $cli <command> --project <project> --engine <engine> --json
```

Use `flax` directly when a published executable is installed.

## Local commands

- Engines: `engines list|add|remove|default|info`, `engine pin|unpin`
- Projects: `projects list|add|remove|info|size`
- Lifecycle: `open`, `play`, `generate`, `compile`, `clean`, `build`
- Diagnostics: `doctor`, `logs`, `env`, `config`, `status`, `completion`
- Local lifecycle: `projects create`, `new`, `templates list|info`, `test list|run`
- Evidence/playtest: `capture viewport|game`, `playtest status|begin|pause|resume|step|find|wait|assert|capture|end`
- Durable jobs: `jobs list|info|status|wait|cancel|prune`; add `--detach` to `compile`, `build`, or `command`
- Signed local feeds: `feeds verify|list|install --manifest <file> --signature <file> --public-key <RSA-PEM>`
- Development Player: `player status|pause|resume|step|quit|input`; `runtime input` is an alias. Input supports virtual `key`, absolute/relative `pointer`, `inspect`, and `reset`
- Visject: `visject groups list`, `asset inspect`, `validate`, `parameter add`, `node add|remove|set`, `connect`, `disconnect`. Graph mutations require the asset editor window to be closed; saves are rollback-protected and reload-verified.
- Agent bridge: `mcp` (stdio `initialize`, `ping`, `tools/list`, `tools/call`)
- Diagnostics: `diagnose status|bundle --to <project-relative.zip>`

Only the local empty-project/template subset is stable. Remote engine/platform
installation, package resolution, template editing, and managed updates remain
deferred; signed local feed verification/install is available and requires an
explicit confirmation. Inspect current `--help` and the repository roadmap before
assuming remote services.

## Editor-owned commands

- Assets: `assets list|types|info|create|mkdir|import|duplicate|move|rename|delete|reimport|export|get|set|save|refresh|verify|material-instance|batch`. Asset commands prefer a matching live Editor; use `assets batch --input <manifest.json> --verify-reload` to execute many operations in one Editor session.
- Prefab assets: `prefab-assets hierarchy`, `actor get|add|set|delete`, `component get|add|set|remove`, `reference set`, and `batch`. These commands spawn a transient hierarchy with no Scene parent, persist through the native Prefab serializer, and report `sceneTouched: false`. Use them instead of scene instantiation when the requested change belongs to the Prefab asset.
- Typed catalog: `commands list|info`, `command <name>`
- Live Editor: `editor status|play|pause|resume|stop|step|focus|save-all|recompile|close`, `console`, `performance`, `selection`
- Project settings: `settings list|get|schema|diff|set` (partial JSON patches; `--dry-run` is available on set)
- Scene-data builders: `bake status`, `bake lighting|navmesh|probes|csg|scenes|sdf start` plus supported cancel/clear actions
- Diagnostics: `dev unlock-eval`, then `dev eval` or `dev eval-file` in the same live Editor. Eval is bounded expression-only and cannot mutate project state.
- Arbitrary C#: `dev unlock-csharp`, then `dev eval-csharp` or `dev eval-csharp-file` with the returned token. This is audited, in-process development code, not a sandbox.
- Visject graphs: `visject groups list`, `asset inspect|validate`, parameter add, node add/remove/set, and connect/disconnect for Material and Animation Graph assets.
- Player/runtime: `player status|pause|resume|step|quit` and virtual keyboard/mouse `player input`; use pointer `--state relative --dx ... --dy ...` for mouse-look deltas and `input inspect` for device/mapping/state diagnostics. Gamepad/action synthesis is unsupported.
- Authoring: `authoring-root`, `scenes`, `actors`, `prefabs`, `prefab-assets`
- FMOD audio: `audio.authoring.inspect|diagnose|run` for contained JavaScript migrations, clean diagnostics, bank builds, and typed-asset synchronization; use the companion `flax-fmod-audio` skill for complete authoring and gameplay hookup workflows.

The stable settings groups are `game`, `time`, `audio`, `layers`, `physics`,
`input`, `graphics`, `network`, `navigation`, `localization`, `build`, and
`streaming`; platform groups are added when the installed API exposes them.
`scenes active` follows Flax's first-loaded-scene authoring
semantics; `scenes build-list` edits `GameSettings.FirstScene` and ordered
`BuildSettings.AdditionalScenes`.

The bridge manifest advertises capability groups. The CLI checks them before
invocation and returns `FLX-BRIDGE-CAPABILITY-0004` instead of silently falling
back to another transport. A standalone Player only appears after a valid
Development Player starts and publishes its local manifest.

Typed commands support `--one-shot`, `--live-only`, and `--instance`. Editor control commands are inherently live and do not accept a redundant `--live-only` option.

`prefab-assets` accepts a project Content-relative `.prefab` path or asset GUID.
Actor selectors accept `.`, a hierarchy path, a transient Actor ID from the same
open operation, or a stable `PrefabObjectID`; component selectors accept type
name, transient ID, or stable `PrefabObjectID`. Runtime Actor/component IDs may
change each time the transient hierarchy is spawned, so use paths or prefab
object IDs across separate invocations. Mutations are rejected while that
Prefab is open in the Prefab editor.

Editors started by `flax open` or `flax play` run in CLI automation mode. Loaded
scenes changed on disk are reloaded automatically in this mode, without the
interactive scene-reload prompt that can block automation.

To shut down a live Editor without a modal prompt, choose one explicit policy:

```powershell
dotnet $cli editor close --save --project $project --engine $engine --json
dotnet $cli editor close --discard --project $project --engine $engine --json
```

`editor close` returns only after the Editor has accepted the save/discard policy
and requested exit. Verify the process is gone before rebuilding the Editor.

Canonical texture import settings can be changed through a reload-verified batch
without editing generated metadata. Use `"importer": "texture"`; `importOptions`
is a partial overlay on the texture's existing tracked settings. The operation
waits for the exact texture build before it reports success:

```json
{
  "schemaVersion": 1,
  "verifyReload": true,
  "operations": [
    {
      "action": "reimport",
      "path": "Textures/UI/Journal/Paper.png",
      "importer": "texture",
      "importOptions": {
        "sRGB": true,
        "AlphaIsTransparency": true,
        "GenerateMipMaps": true
      }
    }
  ]
}
```

Playtest example:

```powershell
dotnet $cli playtest begin --project $project --engine $engine --json
dotnet $cli playtest find --name Camera --project $project --engine $engine --json
dotnet $cli playtest assert --name Camera --active true --project $project --engine $engine --json
dotnet $cli playtest capture game --to Artifacts/playtest.png --project $project --engine $engine --json
dotnet $cli playtest end --project $project --engine $engine --json
```

MCP is line-delimited JSON-RPC on stdin/stdout. `tools/list` always includes the
generic `flax_command`; with a project it also projects typed commands as
`flax.command.<name>`. MCP closes without appending a normal CLI result envelope.

## Common validation

```powershell
dotnet $cli command cli.ping --project $project --engine $engine --one-shot --json
dotnet $cli status --project $project --engine $engine --json
dotnet $cli commands list --project $project --engine $engine --live-only --json
```
