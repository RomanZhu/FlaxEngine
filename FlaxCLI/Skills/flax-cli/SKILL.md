---
name: flax-cli
description: Build, launch, inspect, and automate Flax Engine projects with the standalone Flax CLI. Use when Codex needs to select or pin an engine checkout, create a safe local project, register projects, generate project files, compile or clean targets, launch or close the Editor, edit assets or Prefab assets without scene instantiation, run Game Cooker or bake operations, edit typed settings, capture evidence, run deterministic playtests, control a development Player, detach jobs, invoke MCP, inspect CLI health, or troubleshoot typed command routing.
---

# Flax CLI

Use the standalone CLI as the control plane and keep engine-owned work inside Flax tools.

## Resolve the executable and context

1. Prefer an installed `flax` executable when it is on `PATH`.
2. In a Flax Engine source checkout, use `dotnet FlaxCLI/Flax.CLI/bin/Debug/net8.0/flax.dll`; build `FlaxCLI/Flax.CLI/Flax.CLI.csproj` first if the binary is missing or stale.
3. Pass `--project <project-root-or-flaxproj>` and `--engine <engine-root-or-selector>` explicitly for reproducible automation.
4. Add `--json` for machine-readable results. Treat `success`, `errors`, `warnings`, and the process exit code as authoritative.
5. Run contextual `--help` before relying on an unfamiliar or newly added option.

Read [references/commands.md](references/commands.md) when selecting commands or troubleshooting routing.

## Choose the execution path

- Use local host commands for engine discovery, project registration, generation, compilation, cleaning, and process launch.
- Use typed one-shot Editor execution for CI or when no Editor should remain open.
- Use the authenticated live bridge when a matching Editor is already open and repeated operations should avoid startup cost.
- Use `flax assets batch --input <manifest.json>` for bulk imports, creation, material-instance configuration, refresh, and persistence checks. Add `--verify-reload` when the result must prove that changed assets reload from disk.
- Use `flax prefab-assets` for Prefab hierarchy, Actor, Script component, property, and reference edits that belong in the Prefab asset. It operates on a transient off-scene hierarchy and must be preferred over instantiating a Prefab into a gameplay scene merely to edit the asset. Use a JSON input file for atomic batches and reload verification.
- Never mutate Flax asset, scene, or project state by editing serialized engine files when an Editor-owned CLI operation exists.
- Use `flax projects create`/`flax new` only for a missing or empty directory; the local empty template never overwrites existing content. Feed-backed install/update and non-empty template migration are not available.
- Use `flax capture viewport|game --to <project-relative-path>` for Editor-owned PNG evidence. Output is confined below the selected project root.
- Use `flax playtest begin|status|find|wait|assert|capture|end` for live deterministic observation. `begin` waits for the real play-state transition and uses the persisted startup scene when no scene is loaded.
- Use `flax player status|pause|resume|step|quit` and `flax player input key|pointer|inspect|reset` (or the `flax runtime input ...` alias) for a development Editor's embedded Player or a registered Development Player. Pointer `relative` injects mouse deltas; `inspect` reports devices, mappings, and sampled key/axis/action state. Input is virtual Flax device input, not OS hardware; gamepad/action synthesis is intentionally unsupported.
- Use `--detach` on compile/build/command for a durable process-backed job, then `flax jobs list|status|wait|cancel|prune`. Job records and stdout/stderr logs survive the invoking shell.
- Use `flax feeds verify|list|install` only with a signed local manifest, detached signature, RSA PEM key, and explicit `--yes` for archive installation. Verification occurs before extraction and ZIP paths are confined.
- Use `flax visject groups list`, `visject asset inspect|validate`, and the node/connect routes for material and Animation Graph assets. These are live Editor, graph-native operations and mutations persist through Flax's Visject serializer.
- Use `flax dev unlock-csharp` followed by `dev eval-csharp|eval-csharp-file` only for trusted development work. The token is short-lived, the source hash is audited, and execution is in-process with Editor privileges; it is not a sandbox.
- Use `flax mcp --project <path> --engine <path>` as a stdio MCP server. It exposes the same typed command schemas and confirmation rules; do not create a parallel mutation path.
- Use `flax test list|run` for the local native/managed/build adapters. Test targets must live under the selected engine or project root; use detached `command`/`compile`/`build` jobs when a test wrapper needs durable process state.
- Use `flax diagnose status` for health checks and `flax diagnose bundle --to <project-relative.zip>` for a confined, redacted support bundle. Do not include arbitrary files or secrets in a bundle.
- Before closing a live Editor, resolve dirty scenes explicitly: run `flax editor close --save` to persist them or `flax editor close --discard` to discard them. Do not close `FlaxEditor.exe` directly while scenes may be dirty; that invokes a modal save prompt and can hang automation.

## Build safely

Before building an Editor target, check for `FlaxEditor`. Ask it to close and wait for exit. Do not force-terminate it without approval. Use the narrowest relevant build target and report compiler warnings separately from failures.

If scenes may be dirty, choose exactly one shutdown policy before the build:
`flax editor close --save` or `flax editor close --discard`. This prevents the
native "Save before closing?" dialog from blocking automation.

For CLI-only changes, prefer:

```powershell
dotnet test .\FlaxCLI\Flax.CLI.Tests\Flax.CLI.Tests.csproj --nologo
```

For Editor-side CLI changes, rebuild the Development Editor using the repository's documented build command, then validate both one-shot and live execution.

## Validate the outcome

Exercise the narrowest real operation, not just `--help`. For Editor integration,
verify `flax status --json`, a one-shot `cli.ping`, a relevant live command,
`player input` or `player status`, and `visject groups list`. A standalone Player
requires a valid Development Player/cooked project before its manifest can be
tested. For Prefab-asset mutations, query the saved component or hierarchy after
reload and confirm `sceneTouched: false` and unchanged `scenes dirty` state.
Inspect the project log for bridge startup or manifest warnings. Leave the
Editor running only when requested.
