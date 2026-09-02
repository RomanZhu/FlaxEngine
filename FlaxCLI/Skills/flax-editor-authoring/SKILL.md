---
name: flax-editor-authoring
description: Author and inspect Flax scenes, Actors, Script components, Prefab assets, Prefab instances, settings, baked scene data, assets, Visject graphs, captures, and deterministic playtests through typed Flax CLI commands. Use when Codex needs to edit a Prefab directly without scene instantiation, create or inspect scenes and Actors, wire component or asset references, manage Prefab instances, control a development Player, capture evidence, or automate a running Flax Editor.
---

# Flax Editor Authoring

Perform authoring through the typed command registry so the content database, scene state, Undo, asset IDs, and save behavior remain authoritative.

## Establish the route

1. Resolve the CLI, project, and engine as described by the companion `flax-cli` skill when it is available.
2. Run `flax status --project <path> --engine <path> --json`.
3. Use `--live-only` when the user expects changes in an open Editor. Use `--one-shot` for deterministic headless work. Omit both only when automatic live-first fallback is acceptable.
4. Use `flax commands info <dotted-name> --json` before invoking an unfamiliar command or passing structured values.

Read [references/authoring.md](references/authoring.md) for the supported groups and validated command forms.

## Author safely

- Keep paths Content-relative and use named typed options such as `--path`; do not assume positional arguments.
- Set `flax authoring-root` when automation must be confined below a generated-content folder.
- Open or create a scene before commands whose schema reports `requiresScene: true`.
- When a change belongs to a Prefab asset, use `prefab-assets`; its commands report `requiresScene: false` and must not require opening, creating, or dirtying a Scene. Reserve `prefabs instantiate|apply|revert|unpack` for actual scene instances and overrides.
- Capture returned scene, Actor, component, Prefab, and asset IDs and use those stable IDs for follow-up operations.
- Across separate `prefab-assets` invocations, prefer hierarchy paths or `PrefabObjectID` values. The ordinary Actor and component IDs belong to the transient spawned hierarchy and may change after save or reload.
- Use `flax capture viewport|game --to <path>` for project-confined Editor evidence; the Editor owns the screenshot and the CLI returns the saved path.
- Use `flax playtest begin` to load the persisted startup scene and wait for play mode, then `playtest find|wait|assert` with stable Actor IDs or exact name/type filters. Use `playtest end` before authoring mutations.
- Raw input and Player control require an explicitly advertised `player`/`runtimeInput` capability. `player input` sends virtual Flax device events, not OS hardware; gamepad/action synthesis and collision/gameplay event observation are not silently emulated.
- Use `visject groups list` to discover real Flax node groups before editing Material or Animation Graph assets. Inspect/validate first, create graph parameters with `visject parameter add`, then mutate with typed node/connect commands so Visject serialization remains authoritative.
- For open-world trusted development work, use `dev unlock-csharp` and the returned token with `dev eval-csharp`; this is audited in-process code, not a sandbox, and should not replace a reusable typed command.
- Pass `--yes` only when the requested destructive action and exact target have been verified.
- Use `--arguments <json>` or `--input <file.json>` for complex objects and arrays instead of fragile shell quoting.
- Use `prefab-assets batch --input <request.json> --yes` when several Prefab edits must resolve against one transient hierarchy and apply once. This permits adding a component and wiring it to another object in the same operation without exposing an intermediate saved state.
- Do not directly edit `.scene`, `.prefab`, `.flax`, or content database files.
- Before an Editor shutdown, run `flax scenes save` followed by `flax editor close --save`, or deliberately choose `flax editor close --discard`. Never terminate the Editor process with dirty scenes because the native save dialog blocks the CLI.

## Verify persistence

After mutation, check `success`, `saved`, and `dirty`. Query the affected object with `actors get`, `actors find`, `scenes hierarchy`, `assets info`, or the corresponding typed command. For Prefab assets, query `prefab-assets hierarchy`, `actor get`, or `component get` after reload; verify `sceneTouched: false` and that `scenes dirty` did not change. For important persistence checks, reopen the scene or restart the route and query again.

When testing a live workflow, confirm the selected instance ID and project match before mutation. Multiple matching instances must be resolved explicitly with `--instance` rather than guessed.
