---
name: flax-fmod-audio
description: Author, integrate, configure, validate, and troubleshoot Flax FMOD Studio audio through typed Flax CLI commands and engine gameplay APIs. Use when importing sounds into an FMOD project, creating or changing events and parameters, synchronizing banks, connecting typed AudioEvent assets to gameplay or physics triggers, handling predicted/networked presentation, diagnosing audibility, or validating AudioGym.
---

# Flax FMOD Audio

Use typed Flax CLI commands and typed `AudioEvent` assets so the project settings, content database, middleware IDs, and runtime diagnostics remain authoritative.

## Choose the workflow

- For imported sounds, new or changed FMOD events, repeatable JavaScript migrations, typed asset synchronization, gameplay trigger hookup, or end-to-end delivery, read [references/authoring-workflows.md](references/authoring-workflows.md).
- For backend selection, FMOD project linking, banks, audio settings, listeners, spatial responsiveness, output metering, and AudioGym validation, read [references/setup-and-validation.md](references/setup-and-validation.md).
- For gameplay code using handles or the `(AudioEvent, owner Actor)` persistent identity API, read [references/gameplay-api.md](references/gameplay-api.md).

Resolve `flax`, `--project`, and `--engine` as described by the companion `flax-cli` skill when available. Run `flax status --json` and inspect `flax commands info <name> --json` before relying on an unfamiliar typed command.

## Preserve these invariants

- Prefer typed `AudioEvent` and `AudioBank` assets over string paths. Use path fallbacks only for migration or explicit low-level testing.
- Preserve existing FMOD event GUIDs and Flax asset IDs. Update an event by exact path or GUID; do not delete and recreate it merely to simplify a migration.
- Keep FMOD mutation in explicit, idempotent scripts. Project `Scripts` files may register menus or export helpers at load time but must not perform authoring mutations at top level.
- Apply initial parameters before starting an event when they affect instruments, routing, or virtualization.
- Derive audio from the gameplay action or replicated state that already owns the behavior. Do not introduce an audio-only RPC. During prediction, emit audio presentation only when the simulation is not resimulating and apply the project's existing presentation deduplication policy.
- Submit final listener and emitter transforms before the middleware update. Camera rigs that finalize in `LateUpdate` require the engine's late spatial flush.
- Treat Doppler velocity and left/right panning as separate diagnostics. Disable inferred listener velocity while isolating spatial orientation problems.
- Prove audibility with backend state and measured output, not merely a successful play call.
- Keep destructive bank repair, scene mutation, and Editor shutdown behind the same explicit target and save/discard checks used by the Flax CLI skills.
