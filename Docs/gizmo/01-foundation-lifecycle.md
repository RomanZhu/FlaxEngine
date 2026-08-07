# Implementation: Transaction Lifecycle

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note decomposes W00, W01, P0-01, P0-02, and P0-03. It owns the interaction session model and the rules that make preview, cancel, undo, duplication, focus loss, and invalidation safe.

## Responsibility boundary

The lifecycle layer owns session state and result orchestration. It does not own screen-space hit testing, renderer styling, or the details of translation/rotation/scale solvers.

| Concept | Required owner | Consumer |
| --- | --- | --- |
| `TransactionOrigin` | Scene integration/session | Solvers, cancel, undo |
| `InteractionAnchor` | Session/lifecycle | Solvers and re-anchoring |
| `InteractionResult` | Session orchestration | Scene integration, feedback, renderer |
| `InteractionState` | One explicit state machine | Viewport input and UI |
| `SemanticHandle` | Picking layer, latched by session | Active solver and feedback |

## State data

`TransactionOrigin` is immutable from begin until commit or cancel and contains:

- top-level selected object identities;
- exact original transforms and original bounds;
- initial transform space, basis, mode, pivot policy, and world-space pivot position;
- duplication provenance and the objects created for this drag;
- undo metadata and safe object references.

`InteractionAnchor` is replaceable within one transaction and contains the current result at the re-anchor point, pointer position/ray, frozen camera and fallback plane, active semantic handle, and accumulated rotation/scale context.

`InteractionResult` is recomputed from origin/anchor/current input. It contains net translation, normalized rotation, scale factors, derived object transforms, annotations, and snap identity. It must never be inferred only from the last frame's object transform.

## Required transitions

```text
Inactive -> Hovering -> Armed -> Dragging -> Committing -> Inactive
                                      |-> Clutched -> Dragging
                                      |-> NumericEntry -> Dragging
                                      |-> Cancelling -> Inactive
```

| Transition | Required action |
| --- | --- |
| Inactive → Hovering | Build candidates and feedback; do not mutate scene objects. |
| Hovering → Armed | Latch the semantic handle, capture origin, and capture pointer input. |
| Armed → Dragging | Create the initial anchor and compute a preview from origin/anchor. |
| Dragging → Clutched | Freeze the result while camera navigation owns the pointer. |
| Clutched → Dragging | Capture the new ray/camera and re-anchor at the unchanged result. |
| Dragging → NumericEntry | Freeze pointer solving and transfer input to the HUD field. |
| NumericEntry → Dragging | Apply exact entered values as the current result; re-anchor if pointer solving resumes. |
| Dragging → Committing | Apply final result, finalize deferred systems, and create one undo item. |
| Any active state → Cancelling | Restore exact origin, delete drag-created duplicates, clear capture, and create no undo item. |
| Any active state → Inactive | Clear transient state and feedback; no stale active handle may remain. |

Every transition method must be safe when called twice or out of order. Prefer a no-op plus diagnostic assertion over corrupting a transaction.

While a gizmo transaction owns the mouse, capture loops the physical cursor across every desktop edge. Solvers use the capture system's accumulated offset as a continuous pointer coordinate, while cursor UI remains attached to the wrapped physical position. Opposite edge crossings must cancel their offsets exactly, so retracing to the pickup coordinate reproduces the origin result without preventing motion through or beyond zero.

## Preview and re-anchoring contract

1. Capture `TransactionOrigin` before the first mutation.
2. Create an `InteractionAnchor` from the press ray, camera, semantic handle, and current result.
3. Solve the current input against the anchor and derive an `InteractionResult` from the origin.
4. Apply the preview through scene integration without replacing the origin.
5. If snap, precision, camera, or a keyboard constraint changes, capture the current result, create a new anchor, and solve from that anchor. The transition frame must be visually identical.
6. Commit or cancel using the immutable origin.

Tool mode, transform space, and pivot changes are queued until release by default. Same-transaction transitions are allowed only after they re-anchor and pass the no-jump and undo tests.

## Commit, cancel, and failure rules

- Mouse release and Enter commit one logical undo item.
- Escape and the configured cancel mouse button restore exact original transforms and create no undo item.
- No-op operations create no undo item.
- Focus loss freezes the transaction. If focus returns with the drag button released, cancel by default.
- Deleting or unloading an affected object cancels the affected transaction safely and removes stale references.
- An exception during preview restores or safely preserves the transaction, exits pointer capture, and records a diagnostic.
- Mouse-capture loss cannot leave the gizmo visually active.
- Duplicate once at the first valid motion, record created identities in the origin, and delete them on cancel.

## Implementation checklist

- [x] W00: inventory the current lifecycle, viewport capture, undo, duplication, and invalidation paths.
- [x] W01: create pure result/session seams without changing public behavior.
- [x] P0-01: implement the explicit states and entry/exit actions.
- [x] P0-02: separate origin capture from anchor creation and implement deterministic preview.
- [x] P0-03: cover commit, exact cancel, no-op, duplicate, focus loss, deletion, unload, and exception paths.
- [x] P4-03: route camera clutch and modifier changes through the same re-anchor API.

## Completion evidence

The managed fixture `Source/Engine/Tests/TestTransformGizmoInteraction.cs` records lifecycle transitions and covers:

- origin-derived translation and protected multiplicative scale previews across multiple updates, including zero-scale prevention;
- re-anchor continuity and exact cancellation without an undo item;
- clutch, numeric-entry, and mouse-capture-loss transitions;
- focus-loss clutching and transaction-aware duplicate rollback;
- commit/no-op undo counts; and
- transform-action replay against a recreated scene node.

Scene preview integration applies the accumulated result from the transaction start transforms, while foliage supplies a world-to-local restore hook so its custom undo action remains authoritative. `TransformObjectsAction` stores node IDs and resolves them on every Do/Undo replay. Cross-check `INV-01` through `INV-07` and `INV-11` in the [implementation index](implementation.md).

The fixture has been added but was not executed in this pass; the repository’s managed test command requires generated/build outputs and was intentionally left for an authorized validation run.

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
