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
- Visject: `visject groups list`, `asset inspect`, `validate`, `node add|remove|set`, `connect`, `disconnect`
- Agent bridge: `mcp` (stdio `initialize`, `ping`, `tools/list`, `tools/call`)
- Diagnostics: `diagnose status|bundle --to <project-relative.zip>`

Only the local empty-project/template subset is stable. Remote engine/platform
installation, package resolution, template editing, and managed updates remain
deferred; signed local feed verification/install is available and requires an
explicit confirmation. Inspect current `--help` and the repository roadmap before
assuming remote services.

## Editor-owned commands

- Assets: `assets list|types|info|create|mkdir|import|duplicate|move|rename|delete|reimport|export|get|set|save`
- Typed catalog: `commands list|info`, `command <name>`
- Live Editor: `editor status|play|pause|resume|stop|step|focus|save-all|recompile|close`, `console`, `performance`, `selection`
- Project settings: `settings list|get|schema|diff|set` (partial JSON patches; `--dry-run` is available on set)
- Scene-data builders: `bake status`, `bake lighting|navmesh|probes|csg|scenes|sdf start` plus supported cancel/clear actions
- Diagnostics: `dev unlock-eval`, then `dev eval` or `dev eval-file` in the same live Editor. Eval is bounded expression-only and cannot mutate project state.
- Arbitrary C#: `dev unlock-csharp`, then `dev eval-csharp` or `dev eval-csharp-file` with the returned token. This is audited, in-process development code, not a sandbox.
- Visject graphs: `visject groups list`, `asset inspect|validate`, node add/remove/set, and connect/disconnect for Material and Animation Graph assets.
- Player/runtime: `player status|pause|resume|step|quit` and virtual keyboard/mouse `player input`; use pointer `--state relative --dx ... --dy ...` for mouse-look deltas and `input inspect` for device/mapping/state diagnostics. Gamepad/action synthesis is unsupported.
- Authoring: `authoring-root`, `scenes`, `actors`, `prefabs`

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

To shut down a live Editor without a modal prompt, choose one explicit policy:

```powershell
dotnet $cli editor close --save --project $project --engine $engine --json
dotnet $cli editor close --discard --project $project --engine $engine --json
```

`editor close` returns only after the Editor has accepted the save/discard policy
and requested exit. Verify the process is gone before rebuilding the Editor.

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
