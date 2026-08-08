# Flax 3D Transform Gizmo Implementation Plan

> Executable implementation ledger for the redesign specification.

Parent specification: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md)
Source work plan: [`Docs/Flax_3D_Transform_Gizmo_Redesign_Specification.docx`](../../Docs/Flax_3D_Transform_Gizmo_Redesign_Specification.docx)

The parent specification is normative. This document owns sequencing, dependencies, code ownership, progress tracking, proof requirements, phase gates, and handoff notes. Focused implementation contracts are numbered in dependency order:

1. [`01-foundation-lifecycle.md`](01-foundation-lifecycle.md) — W00/W01 baseline seams and Phase 0 session state, origin/anchor lifecycle, cancellation, undo, duplication, and invalidation.
2. [`02-rendering-depth.md`](02-rendering-depth.md) — H01 rendering hotfix and Phase 1 self-depth, scene occlusion, visibility, geometry, and drawing.
3. [`03-feedback.md`](03-feedback.md) — Phase 1 handle state, world annotations, cursor HUD, measurements, and active-operation feedback.
4. [`04-picking-sizing.md`](04-picking-sizing.md) — Phase 2 projection sizing, screen-space targets, candidate arbitration, hysteresis, and proxies.
5. [`05-transform-math.md`](05-transform-math.md) — Phase 3/4 translation, rotation, scale, spaces, pivots, and multi-selection solvers.
6. [`06-snapping-settings.md`](06-snapping-settings.md) — Phase 4/5 total-result snapping, precision, numeric entry, settings, and input presets.
7. [`07-integration-validation.md`](07-integration-validation.md) — hardening, automated/manual validation, compatibility rollout, and final evidence.

Each focused document links back to this index and to the parent specification. Read them in numeric order when implementing. If an implementation note conflicts with the parent specification, stop, record the deviation, and resolve it explicitly.

## Progress contract

Status legend: `☐` not started, `◐` in progress, `☑` complete, `⚠` blocked, `↺` implemented but failed validation or requiring rework.

- Treat this document as the durable handoff ledger. At the beginning of a session, read the active package, its dependencies, and the latest evidence/deviation note.
- Normally only one package is `◐`. A second package is allowed only for a clearly recorded external blocker and non-overlapping files.
- When starting a package, update its marker, date, intended files, and owner. When stopping, record what changed, validation performed, remaining work, and the next safe action.
- A checked item means the result exists and has been inspected. It does not mean an attempt was made. Do not check an item when code is stubbed, unreachable, or failing its completion gate.
- Record a commit hash when commits are being made; otherwise record the exact diff scope and key symbols changed.
- If repository reality differs from a planned file or symbol, locate the current owner, record the deviation, and preserve the behavioral boundary. Do not create duplicate infrastructure merely because a name moved.
- After a failure, set the package to `↺` or `⚠`, preserve the failure output or reproduction steps, and state the next hypothesis.

## Scope and safety rules

In scope: scene-editor 3D translation, rotation, and scale manipulation; gizmo rendering and selection; active-operation feedback; snapping; spaces and pivots; hierarchy-safe multi-selection; editor input/options; undo and cancellation; reliability, performance, and validation described by the parent specification.

Out of scope unless a dependency proves unavoidable: animation-curve editing, UI rect transforms, VR manipulation, a general CAD constraint solver, runtime/game gizmos, wholesale editor renderer replacement, third-party code changes, and unrelated viewport UX redesign.

- Preserve existing scene-transform serialization, undo serialization, configured snap values, action-based shortcut infrastructure, and top-level parent filtering unless a completion gate proves they cause a specified defect.
- Do not modify `Source/ThirdParty/`, `Binaries/`, `Cache/`, generated solutions, or generated project files.
- Do not run project generation as part of this work unless explicitly requested.
- Do not run a build automatically. A build is permitted only when explicitly requested or immediately before a requested push. Before every permitted build, check for `FlaxEditor.exe`, close it gracefully, and wait for exit. Ask before force termination.
- Keep preview computation origin-based and deterministic. The transaction origin is immutable; the interaction anchor may change.
- Keep rendering identity, motor-target geometry, and transform math separate.
- Treat cancellation, capture loss, invalid selection, tool changes, and exceptions as first-class transitions.
- Prefer small, reviewable changes behind the existing public surface. Record compatibility consequences for any public or serialized change.

## Delivery phases

| Phase | Deliverables | Reason |
| --- | --- | --- |
| Hotfix | Self-depth-correct gizmo rendering; fix blue-over-red ordering | Immediate visual truth and easy runtime verification |
| Phase 0 | Explicit transaction/state machine, Escape cancel, focus handling, safe undo | Prevent lost work before expanding behavior |
| Phase 1 | Active handle states, cursor HUD, world annotations, rotation wedge, scale dimensions | Make existing operations understandable and precise |
| Phase 2 | Projection-correct sizing and screen-space picking with hysteresis | Fix accessibility and camera-angle reliability |
| Phase 3 | Multiplicative scale, group scaling, bounds cage, negative-scale policy | Replace the weakest manipulation subsystem |
| Phase 4 | Re-anchoring, camera clutch, numeric input, axis keyboard constraints | Increase speed and advanced control |
| Phase 5 | Semantic snapping, custom workplanes/pivots, presets, telemetry | Add power features after the core is stable |

## Work-package ledger

All packages begin as `☐`. Update the status, owner, date, evidence, and deviation note in the implementation change that advances a package.

| ID | Work package | Phase | Status | Depends on | Required proof |
| --- | --- | --- | --- | --- | --- |
| W00 | Baseline, safeguards, and change map | Foundation | ☑ | — | Inventory and baseline evidence |
| W01 | Testable math and session seams | Foundation | ☑ | W00 | Pure helpers and test strategy |
| H01 | Self-depth-correct rendering hotfix | Hotfix | ☑ | W00 | Runtime orbit captures and draw/depth audit |
| P0-01 | Transaction/session state model | Phase 0 | ☑ | W00, W01 | Explicit state transitions |
| P0-02 | Begin, anchor, preview, re-anchor, commit, cancel | Phase 0 | ☑ | P0-01 | Origin-authoritative lifecycle |
| P0-03 | Undo, duplication, invalidation, capture safety | Phase 0 | ☑ | P0-02 | Exactly one undo / exact restore |
| P1-01 | Semantic handle visual state | Phase 1 | ◐ | P0-02, H01 | `FeedbackHandleState`, latched handle rendering, and active-handle visibility |
| P1-02 | World annotations and cursor HUD | Phase 1 | ◐ | P1-01 | `FeedbackModel`, transaction totals, measurement basis, and quadrant HUD |
| P1-03 | Mode-specific active feedback | Phase 1 | ◐ | P1-02 | Translation/rotation/scale/snap overlays; runtime capture pending |
| P2-01 | Projection-derived constant-pixel sizing | Phase 2 | ◐ | H01 | Perspective/orthographic/DPI parity |
| P2-02 | Screen-space semantic target generation | Phase 2 | ◐ | P2-01 | Targets independent of mesh geometry |
| P2-03 | Hit arbitration, hysteresis, overlap cycling | Phase 2 | ◐ | P2-02 | Stable acquisition |
| P3-01 | Multiplicative scale core | Phase 3 | ☑ | P0-02, W01 | Exponential factor mapping |
| P3-02 | Group, individual, and bounds-cage scaling | Phase 3 | ◐ | P3-01 | Axis/plane/uniform and group pivot done; cage pending |
| P3-03 | Zero crossing and mirror policy | Phase 3 | ◐ | P3-01, P1-02 | Safe default done; explicit mirroring UX pending |
| P4-01 | Anchor-based translation solvers | Phase 4 | ☑ | P0-02, W01 | Frame-rate-independent move |
| P4-02 | Stable ring rotation and true arcball | Phase 4 | ☑ | P0-02, W01 | Unwrapped normalized rotation |
| P4-03 | Modifier re-anchoring and camera clutch | Phase 4 | ☑ | P4-01, P4-02, P3-01 | No jumps on semantic change |
| P4-04 | Keyboard constraints and inline numeric entry | Phase 4 | ☐ | P4-03, P1-02 | Exact values in one transaction |
| P5-01 | Total-result grid and angle snapping | Phase 5 | ☐ | P4-03 | Update-rate-independent snap |
| P5-02 | Sticky semantic scene snapping | Phase 5 | ☐ | P5-01, P2-03 | Stable target identity |
| P5-03 | Spaces, pivots, hierarchy, multi-selection | Phase 5 | ◐ | P4-01, P4-02, P3-02 | Existing spaces/pivots and group behavior done; expanded policies pending |
| P5-04 | Settings, actions, and input presets | Phase 5 | ☐ | P4-04, P5-03 | Rebindable and persisted |
| R01 | Reliability and performance hardening | Hardening | ☐ | P0-03 through P5-04 | Failure-safe and profiled |
| V01 | Automated verification | Validation | ☐ | W01 through R01 | Deterministic test coverage |
| V02 | Manual acceptance matrix | Validation | ☐ | V01 | Section 16 evidence set |
| D01 | Compatibility rollout, cleanup, final handoff | Delivery | ☐ | V02 | Review-ready implementation |

## Ownership and change surface

| Area | Primary locations | Responsibility after redesign |
| --- | --- | --- |
| Core lifecycle | `Source/Editor/Gizmo/TransformGizmoBase.cs`; `TransformGizmoBase.Types.cs` | Session state, origin/anchor ownership, mode/axis state, transition API, deterministic preview orchestration |
| Settings | `Source/Editor/Gizmo/TransformGizmoBase.Settings.cs`; `Source/Editor/Options/VisualOptions.cs`; `InputOptions.cs` | Defaults, advanced thresholds, persisted choices, action bindings |
| Picking | `Source/Editor/Gizmo/TransformGizmoBase.Selection.cs` and `TransformGizmoBase.Projection.cs` | Projected semantic targets, cursor-local depth arbitration, hysteresis, ring/trackball precedence, overlap cycling |
| Drawing | `Source/Editor/Gizmo/TransformGizmoBase.Draw.cs` and the gizmo material/render path located by W00 | Self-depth pass, visible/occluded styling, active overlays, labels, and guides |
| Scene integration | `Source/Editor/Gizmo/TransformGizmo.cs` | Selected-object snapshots, top-level filtering, preview application, undo, duplication, invalidation |
| Viewport integration | `Source/Editor/Viewport/MainEditorGizmoViewport.cs` and viewport owners found by W00 | Pointer rays, capture/focus events, camera clutch, action routing, overlay controls |
| Undo | Existing `Source/Editor/Undo` and History actions used by `TransformGizmo` | One action for committed drag; none for no-op/cancel; safe object references |
| Tests | Existing managed/native targets when dependency direction permits, otherwise a focused editor test seam | Pure math, transitions, selection arbitration, hierarchy, and undo invariants |
| Documentation | This document, the parent specification, and affected editor transform workflow docs | Defaults, shortcuts, migration/rollout notes, and final evidence |

File-boundary rule: the exact split may evolve, but do not turn `TransformGizmoBase.cs` into a single giant replacement. If a concern exceeds a focused region, add a clearly named partial such as `TransformGizmoBase.Interaction.cs`, `.Feedback.cs`, `.Projection.cs`, or a small internal helper. Avoid abstract frameworks used only once.

## Required architecture before feature work

| Concept | Required content | Lifetime / owner |
| --- | --- | --- |
| `TransactionOrigin` | Top-level selected identities; exact original transforms; original bounds; initial pivot/basis/mode; duplication provenance; undo metadata | Immutable from begin until commit/cancel; scene integration owns application |
| `InteractionAnchor` | Current result at re-anchor; pointer position/ray; frozen camera and fallback plane; active semantic handle; accumulated rotation/scale context | Replaceable inside one transaction when input semantics change |
| `InteractionResult` | Net translation, normalized rotation, scale factors, derived object transforms, annotations, snap identity | Recomputed from anchor/origin and current input; never inferred only from last frame |
| `SemanticHandle` | Mode, axis/plane/ring/center/cage identity, display geometry, target geometry, depth, availability, projected direction | Rebuilt when view/gizmo changes; latched for active drag |
| `InteractionState` | Idle, Hover, Armed/Pressed, Dragging, CameraClutch, NumericEntry, Committing, Cancelling | One explicit state machine with entry/exit actions |
| `FeedbackModel` | Active handle state, HUD rows, world markers, measurement basis, warning/snap badges | Pure data produced by interaction; renderer/UI consume it |

The UI and renderer may read session state and the feedback model but must not mutate object transforms. Picking chooses a semantic handle; manipulation solves the selected degree of freedom; scene integration applies a result; undo records origin/final arrays. Keep those four responsibilities separable in code and tests.

## Phase gates and stop conditions

| Gate | May begin when | May exit when | Stop and return when |
| --- | --- | --- | --- |
| Foundation | W00 starts | W00 and W01 complete | Test dependency or ownership is unclear enough to risk architectural inversion |
| Hotfix | W00 rendering trace exists | H01 visual/depth evidence passes | Renderer-wide change is required beyond gizmo scope without direction |
| Phase 0 | Origin/anchor seams exist | Commit/cancel/undo/focus/invalidation gates pass | Any path can lose work or leave a stuck capture |
| Phase 1 | Feedback reads session state | All three modes communicate exact totals | HUD/drawing mutates transforms or duplicates solve logic |
| Phase 2 | Projection helper and depth are stable | Sizing and picking acceptance passes | Targets flicker, steal unrelated hits, or diverge from display |
| Phase 3 | Transaction and HUD support factor results | Scale/mirror/group/cage gates pass | Zero crossing is ambiguous or an implementation would introduce shear/skew |
| Phase 4 | Mode solvers use anchors | Translation/rotation/re-anchor/numeric gates pass | Modifier/camera transitions jump or split undo |
| Phase 5 | Stable targets/actions/spaces exist | Snap/semantic/pivot/preset gates pass | Scene snap is unstable or too expensive |
| Validation | All implementation packages complete | V01/V02 pass | A MUST acceptance case fails |
| Delivery | Validation evidence complete | D01 complete | Required authorized build is unavailable or failing |

## Cross-package invariants

Re-check these after every phase:

- `INV-01` — Transaction origin is captured before mutation and never replaced during the action.
- `INV-02` — Current preview is derivable from stored origin/anchor/current input; current object transform is not the sole accumulator.
- `INV-03` — Same origin, anchor, camera, and final pointer yields the same result independent of update frequency.
- `INV-04` — Release/Enter commits one logical undo item; configured cancel exactly restores and creates none; no-op creates none.
- `INV-05` — Re-anchor transitions preserve the exact current result on the transition frame.
- `INV-06` — Parent-child filtering prevents selected descendants from receiving a second direct transform.
- `INV-07` — Quaternions are normalized, non-finite results are rejected, and scale is protected from accidental numerical zero.
- `INV-08` — Gizmo internal depth follows geometry, not draw order; scene-occluded styling is a distinct policy.
- `INV-09` — Motor targets are semantic and screen-space accessible; hover is stable; active handles latch.
- `INV-10` — Feedback reports transaction-total values and truthful measurement basis.

## Review follow-up evidence

The Phase 0 follow-up is implemented in the origin/anchor lifecycle, scene integration, foliage gizmo, and transform undo action. The focused managed fixture is `Source/Engine/Tests/TestTransformGizmoInteraction.cs`; it records state transitions and checks re-anchoring, exact cancellation, no-op/commit undo counts, focus/capture loss, transaction-aware duplicate rollback, multiplicative scale, and recreated-node replay. The fixture has not yet been executed because the repository’s test command requires generated/build outputs and no build was authorized for this pass.
- `INV-11` — Capture/focus loss, deletion/unload, exceptions, scene close, and queued commands cannot leave stale active state or undo references.
- `INV-12` — Per-frame work stays cheap; expensive scene rebuilds and semantic searches occur only when needed.
- `INV-13` — Existing compatible settings, action bindings, scene transforms, and undo serialization remain usable.
- `INV-14` — Every automated/build claim identifies the exact authorized command and actual result; static inspection is never mislabeled as a passing build.

## Current package handoff

The P2 picking/sizing pass has implementation coverage for P2-01 through P2-03. Projection sizing, semantic target caching, cursor-local depth arbitration, hover retention, ring-over-trackball precedence, and overlap cycling are wired into `TransformGizmoBase`.

The focused projection regression test was added to `Source/Engine/Tests/TestTransformGizmoInteraction.cs` but was not executed. Runtime acceptance remains open for DPI/view-mode parity, overlap ordering, ring/trackball acquisition, front/back depth, and hover stability. No build was run because build validation was not authorized.

## Completion report

Use this template in the final implementation note or pull request:

```text
Overall status: [☐/◐/☑/⚠/↺]
Implementation revision or diff scope:
Completed work-package IDs:
Remaining or blocked work-package IDs and exact reasons:
Normative MUST requirements not satisfied:
Behavior changes visible to users:
Compatibility/migration behavior:
Automated tests added and exact results:
Manual acceptance evidence location:
Performance measurements and hardware/context:
Build command/result, only if authorized or required before requested push:
Known limitations and deliberately deferred MAY/SHOULD items:
Recommended next action for reviewer/user:
```

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
