---
name: flax-fmod-audio
description: Configure, validate, troubleshoot, and author Flax FMOD Studio audio through the Flax CLI and engine gameplay APIs. Use for FMOD project setup, bank synchronization, audio settings, listener/spatial timing, runtime audibility diagnostics, AudioGym validation, or owner-managed AudioEvent playback without an AudioEmitter.
---

# Flax FMOD Audio

Use typed Flax CLI commands and typed `AudioEvent` assets so the project settings, content database, middleware IDs, and runtime diagnostics remain authoritative.

## Choose the workflow

- For backend selection, FMOD project linking, banks, audio settings, listeners, spatial responsiveness, output metering, and AudioGym validation, read [references/setup-and-validation.md](references/setup-and-validation.md).
- For gameplay code using handles or the `(AudioEvent, owner Actor)` persistent identity API, read [references/gameplay-api.md](references/gameplay-api.md).

Resolve `flax`, `--project`, and `--engine` as described by the companion `flax-cli` skill when available. Run `flax status --json` and inspect `flax commands info <name> --json` before relying on an unfamiliar typed command.

## Preserve these invariants

- Prefer typed `AudioEvent` and `AudioBank` assets over string paths. Use path fallbacks only for migration or explicit low-level testing.
- Apply initial parameters before starting an event when they affect instruments, routing, or virtualization.
- Submit final listener and emitter transforms before the middleware update. Camera rigs that finalize in `LateUpdate` require the engine's late spatial flush.
- Treat Doppler velocity and left/right panning as separate diagnostics. Disable inferred listener velocity while isolating spatial orientation problems.
- Prove audibility with backend state and measured output, not merely a successful play call.
- Keep destructive bank repair, scene mutation, and Editor shutdown behind the same explicit target and save/discard checks used by the Flax CLI skills.
