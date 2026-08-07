# Implementation: Integration, Reliability, and Validation

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note is the final implementation contract for R01, V01, V02, and D01. It owns editor settings/actions integration, viewport and scene boundaries, undo behavior, performance safeguards, automated/manual validation, compatibility rollout, and final evidence.

## Integration boundaries

| Area | Expected responsibility |
| --- | --- |
| Settings | Defaults, advanced thresholds, persisted choices, action bindings, presets |
| Scene integration | Selected-object snapshots, top-level filtering, preview application, undo, duplication, invalidation |
| Viewport integration | Pointer rays, capture/focus events, camera clutch, action routing, overlay controls |
| Undo | One action for committed drag; none for no-op/cancel; safe object references |
| Documentation | Defaults, shortcuts, migration/rollout notes, and evidence |

Reuse existing scene transforms, undo serialization, snap values, action-based shortcuts, and parent filtering unless a specified defect requires a change. Keep the existing public surface where possible.

## Reliability and performance

- Preview transform updates are cheap and run every frame.
- Defer or throttle expensive physics, navigation, lighting, procedural generation, and asset rebuilds until commit where safe.
- Capture original/final arrays once; do not write a history record for every preview update.
- Batch notifications for large selections and avoid unnecessary aggregate-bounds recomputation.
- Run semantic scene searches only while snap targeting is active and use spatial acceleration structures where available.
- Normalize quaternions, reject non-finite results, and protect scale from numerical zero.
- Deletion/unload, exceptions, focus/capture loss, scene close, and queued commands must not leave stale active state or undo references.

## Automated verification

Prefer pure helper and focused editor seams when dependency direction allows. Cover:

- origin/anchor state transitions and re-anchor continuity;
- update-rate independence at 30, 60, 120, and 240 FPS;
- exact cancel, no-op, one-undo commit, duplicate cleanup, focus loss, and invalidation;
- depth arbitration, semantic target geometry, hysteresis, overlap cycling, and ring/trackball precedence;
- translation degeneracy fallback, rotation unwrap/normalization, multiplicative scale, zero crossing, group/individual policy, and parent filtering;
- total-result grid/angle snapping, sticky semantic targets, precision gain, numeric entry, and measurement basis.

Do not label static inspection as a passing build. Every build/test claim records the exact authorized command and actual result.

## Manual acceptance evidence

V02 must include the section 16 evidence set:

| Area | Evidence |
| --- | --- |
| Depth | Supplied +Z-away overlap: nearer red X covers farther blue Z; scene occlusion remains readable |
| Accessibility | Rotation-ring acquisition, front/back orbit, dark/light/saturated backgrounds |
| Interaction | FPS replay, precision/snap/camera-clutch toggles, Escape restore, safe tool/pivot transition |
| Hierarchy | Parent and child selected without double transform |
| Rotation/scale | Ten full rotations; no zero-crossing mirror by default |
| Feedback | Translation totals, signed rotation, scale factor/dimensions, hidden-handle blocking, HUD placement |

Record screenshots or capture paths, scene/setup details, camera/DPI context, and the actual observed result. Record deviations instead of silently marking a test passed.

## Rollout and handoff

D01 must:

- remove obsolete additive-scale, fixed-order, and duplicate solve paths only after replacement evidence passes;
- preserve compatible serialized settings, action bindings, scene transforms, and undo data;
- document visible shortcut/default changes and any migration behavior;
- confirm no generated files, `Binaries/`, `Cache/`, or third-party changes are part of the implementation;
- fill the completion report in [`implementation.md`](implementation.md).

Per repository policy, do not run a build automatically. A build is permitted only when explicitly requested or immediately before a requested push; check for `FlaxEditor.exe` first, close it gracefully, and ask before force termination.

## Implementation checklist

- [ ] R01: profile preview, picking, snap search, large selection, and commit-time work.
- [ ] V01: add and run focused deterministic tests where the dependency direction permits.
- [ ] V02: collect the complete visual, interaction, and feedback acceptance evidence.
- [ ] D01: clean obsolete paths, confirm compatibility, document rollout, and prepare final handoff.

## Completion evidence

Attach test output, manual evidence, performance context, compatibility notes, and the exact diff/build scope. Re-check `INV-11` through `INV-14` and mark the work-package ledger only after the relevant gates in the [implementation index](implementation.md) pass.

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
