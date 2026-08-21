# FMOD setup and validation

## Establish the route

Use the repository CLI when working from an engine checkout:

```powershell
$cli = ".\FlaxCLI\Flax.CLI\bin\Debug\net8.0\flax.dll"
$engine = "<engine-root>"
$project = "<project-root>"
dotnet $cli status --project $project --engine $engine --json
```

Prefer a matching live Editor for repeated inspection. Use `--one-shot` when no Editor should remain open. Query schemas before mutation:

```powershell
dotnet $cli settings schema --group audio --project $project --engine $engine --one-shot --json
dotnet $cli commands info audio.setup.apply --project $project --engine $engine --one-shot --json
```

## Configure the integration

Inspect before changing anything:

```powershell
dotnet $cli command audio.setup.inspect --project $project --engine $engine --one-shot --json
dotnet $cli command audio.setup.validate --project $project --engine $engine --one-shot --json
dotnet $cli command audio.banks.discover --project $project --engine $engine --one-shot --json
```

Use the setup wizard or project-linking UI when the FMOD Studio project path is per-user. Build and synchronize through typed commands rather than copying or editing `.flax` assets directly:

1. `audio.setup.build-banks`
2. `audio.banks.discover`
3. `audio.banks.sync`
4. `audio.setup.apply`
5. `audio.banks.validate`
6. `audio.setup.validate`

Run `commands info` for the current schemas and pass complex arguments with `--arguments <json>` or `--input <file.json>`.

The normal FMOD event-backend settings are:

```json
{
  "EventBackend": 1,
  "OutputOwner": 1,
  "FmodStudioUpdatePeriod": 5,
  "FmodDspBufferLength": 0,
  "FmodDspBufferCount": 0
}
```

Apply a partial patch only after a dry run:

```powershell
$values = '{"FmodStudioUpdatePeriod":5,"FmodDspBufferLength":0,"FmodDspBufferCount":0}'
dotnet $cli settings set --group audio --values $values --dry-run --project $project --engine $engine --one-shot --json
dotnet $cli settings set --group audio --values $values --project $project --engine $engine --one-shot --json
```

Do not enable the FMOD backend in a project that has no linked project or typed banks merely because another project uses FMOD. Preserve each project's backend and bank choices.

`FmodStudioUpdatePeriod = 5` requests processing at the shortest practical mixer quantum. FMOD quantizes it to the platform mixer block. Leave DSP buffer length/count at platform defaults initially. For desktop troubleshooting, test `512/4`, then `256/4` only when lower latency is needed and profiling shows no underruns.

## Spatial timing and Doppler

The engine must sample listener velocity after scene `LateUpdate`, update tracked emitter transforms, submit all spatial attributes, and only then call the backend update. An early audio service update can add one stale game frame before FMOD's mixer latency and causes obvious left/right lag during fast camera rotation.

For an `AudioListener`:

- Use the camera for orientation and ordinary panning.
- Use an attenuation actor only when distance should be measured from another Actor.
- Set `MaximumInferredVelocity = 0` to disable listener-motion Doppler while diagnosing panning.
- If camera-derived Doppler is wanted, start with a physically plausible cap such as `2000` cm/s and profile the result. Camera shake, spring arms, head bob, teleportation, and mismatched update phases must not become large velocity impulses.

Disabling the FMOD event's Doppler macro is a useful isolation test. If directional lag remains, inspect transform/update timing and mixer buffers. If the issue becomes pitch oscillation only, inspect inferred listener/emitter velocity.

## Runtime proof

Use the typed runtime diagnostics instead of relying on a play return value:

```text
audio.listeners.inspect
audio.listeners.validate
audio.emitters.inspect
audio.emitters.validate
audio.emitters.trace
audio.events.parameters
audio.output.meter
audio.output.assert-audible
audio.setup.runtime-validation.start
audio.setup.runtime-validation.status
```

For AudioGym, use `audio.gym.validate`, then enter play mode and run `audio.gym.playtest`, `audio.gym.focus`, `audio.gym.control`, or `audio.gym.self-test`. Confirm measured RMS, listener state, event playback state, listener mask, sample-loading state, and the reported silence cause.

When rebuilding the engine, close a live Editor through `editor stop`, `editor save-all`, and `editor close --save`; wait for process exit before building `FlaxEditor`. Recompile each project against the rebuilt engine afterward.

## Scene and Game overlays

Add one `FMODAudioManager` to the scene and enable `ShowEventLabels`. Its debug choices are serialized scene state: `ShowSceneOverlay` must remain stable across save/reload, and `ShowGameOverlay` must not reset merely because it was enabled outside Play mode.

The overlay follows Sonity's useful voice-pool model while retaining Flax's richer diagnostics:

- Treat the backend's live FMOD instances as the source of truth, not `AudioEmitter` Actors.
- Include persistent instances, direct `PlayOneShot` calls, and the common `CreateInstance -> Play -> ReleaseInstance` pattern. FMOD continues playing a released one-shot, so diagnostics must retain an observer record until FMOD reports that it actually stopped.
- Project the same world-space source through the Scene viewport camera and the gameplay camera independently.
- Use scene emitters only for the optional inactive-authoring view.
- Display event path, handle, playback/virtual state, volume, play count, timeline, and distance; keep short stale/start/stop fades so brief footsteps remain legible.

If player footsteps are audible but absent from the Scene overlay, inspect `FMODAudioManager.Snapshot.Events`. A healthy entry has the footstep path, `IsOneShot=true`, `Has3DAttributes=true`, the player's foot position, and a non-stopped playback state. If it exists there, the fault is projection/label filtering. If it does not, the backend discarded diagnostics at `ReleaseInstance` even though FMOD retained the voice.
