# FMOD authoring and gameplay integration

Use this workflow for a sound that has already been copied or imported into an FMOD Studio project, for revisions to existing events, and for adding the resulting typed event to gameplay.

## Capture the delivery contract

Before authoring, resolve these facts from the request and repository:

- Source audio files and their paths inside the linked FMOD project.
- Target event path, bank, bus, 2D/3D mode, one-shot or persistent lifetime, and expected polyphony.
- Parameters, ranges, initial values, automation, randomization, loops, release tails, and stop behavior.
- Existing event path or GUID when changing a sound. Preserve it unless the request explicitly renames or replaces the event.
- Gameplay trigger and its existing source of truth: animation/contact, local input, predicted action, authoritative replicated state, scene trigger, or persistent Actor state.
- Expected typed `AudioEvent` field, prefab/scene assignment, and deterministic validation action.

Do not invent missing middleware structure when it changes the audible result materially. Do inspect established sibling events and gameplay code before asking for information already present in the project.

## Inspect before mutation

Resolve the current schemas and linkage:

```powershell
flax command audio.authoring.inspect --project $project --engine $engine --one-shot --json
flax commands info audio.authoring.run --project $project --engine $engine --one-shot --json
flax command audio.events.find --arguments '{"search":"event:/Expected/Path"}' --project $project --engine $engine --one-shot --json
```

Inspect sibling FMOD events for bank, bus, spatializer, parameter, and naming conventions. Inspect the gameplay class or prefab that owns the trigger. Avoid editing FMOD metadata XML directly.

## Structure repeatable FMOD scripts

FMOD evaluates `.js` files under the project `Scripts` directory when the project loads. Use these boundaries:

- `Scripts/lib/*.js`: reusable functions or CommonJS modules with no top-level mutation.
- `Scripts/*.js`: optional menu registration only.
- `Automation/*.js`: explicit runners invoked by `audio.authoring.run`; keep these outside `Scripts` so opening the project cannot rerun a migration.

An authoring migration must:

- Resolve objects by exact event path, GUID, asset path, bank, bus, and parameter identity.
- Validate required sources before changing anything.
- Update existing objects in place and preserve stable IDs.
- Create missing objects only when the requested event does not exist.
- Be safe to run again. A schema marker may skip reconstruction only after the script verifies all required postconditions.
- Save the FMOD project explicitly after successful mutation.
- Throw an actionable error naming the missing or invalid object. Never swallow an authoring failure.

Keep sound selection, volume/pitch automation, tails, and weight/material profiles data-driven inside the FMOD authoring module or a project-owned specification. Gameplay code selects semantic event/profile assets and supplies parameters; it should not reproduce the middleware mix.

## Run the end-to-end authoring stage

```powershell
flax command audio.authoring.run --arguments '{"script":"Automation/CreateOrUpdateMyEvent.js"}' --project $project --engine $engine --one-shot --json
```

The command performs a preflight diagnostic, runs only a `.js` contained by the linked FMOD project, requires a clean post-migration diagnostic, builds banks, and synchronizes typed Flax assets. A dirty preflight is reported but does not prevent a repair migration; a dirty postflight stops before build. Script, diagnostic, build, and synchronization failures are fail-fast.

For setup-only bank changes that require no authoring script, use `audio.setup.build-banks`. Never copy generated banks manually when synchronization commands are available.

## Connect the typed event to gameplay

After synchronization, resolve the resulting typed asset with `audio.events.find` and inspect its parameters with `audio.events.parameters`. Assign it through a typed `JsonAssetReference<AudioEvent>` field, `AudioEmitter`, physics-audio profile, or another established semantic component. Use Flax scene, prefab, Actor, component, and asset commands for serialized state rather than editing serialized files directly.

Choose the trigger model deliberately:

- Local contact, animation, or cosmetic feedback: play locally from that presentation callback.
- Predicted gameplay action: use the action already being predicted, but emit audio only outside resimulation. Reuse the project's presentation deduplication key or edge detection when prediction can revisit the same action.
- Authoritative replicated state: play on the local transition of the replicated state or replicated gameplay event. Do not add an audio-specific RPC.
- Persistent Actor state: use owner-managed `(AudioEvent, Actor)` playback and stop with fade when the state ends.
- Scene-authored trigger: use an `AudioEmitter` or the existing trigger component when its activation/lifetime rules match.
- Physics material/contact: select a typed profile by material and weight class, then drive normalized intensity parameters from physics. Keep contact hysteresis, minimum hold time, smoothing, and release tails in the standardized physics-audio layer.

Audio is presentation, not simulation state. It must not affect deterministic outcomes, and resimulation must not start, stop, or retrigger voices.

## Validate the complete delivery

Run the narrowest applicable checks:

1. `audio.authoring.diagnose`
2. `audio.banks.validate`
3. `audio.setup.validate`
4. `audio.events.find` and `audio.events.parameters` for every new or changed event
5. `audio.references.validate` after loading the affected scene
6. Project script compilation with the intended target, platform, and architecture specified explicitly; do not let a multi-platform target fan out into unrelated or unavailable toolchains
7. A deterministic playtest of the actual trigger
8. `audio.output.assert-audible` or AudioGym/physics diagnostics when measured output is applicable

For a changed sound, verify both the new behavior and every existing typed reference that should retain its identity. For predicted behavior, exercise an ordinary prediction path and a resimulation path; the latter must produce no new audio presentation.

Stop and report the exact failed stage when diagnostics remain dirty, an event is absent or duplicated, IDs unexpectedly change, synchronization is incomplete, compilation fails, or the gameplay source of truth cannot be determined safely. Do not continue stacking speculative repairs.

The final report should name the FMOD script, event paths, retained/new GUIDs, bank/sync result, typed asset paths, gameplay owners and triggers, networking/resimulation behavior, compilation result, and playtest evidence.
