# Flax 3D Transform Gizmo Redesign Specification

> Canonical behavior specification for scene-editor translation, rotation, and scale manipulation.

Status: Proposed target behavior and acceptance criteria
Source: [`Docs/Flax_3D_Transform_Gizmo_Redesign_Specification.docx`](../../Docs/Flax_3D_Transform_Gizmo_Redesign_Specification.docx)
Audit basis: Flax source audit at commit `9e3a89300`

This document contains the normative product and interaction specification. The implementation sequence, work-package ledger, code ownership, and progress contract are intentionally maintained separately in [`implementation.md`](implementation.md).

## Document map

The implementation documents are split by responsibility so an implementation can be delegated without making this specification a second task ledger:

- [`implementation.md`](implementation.md) — phases, dependencies, work packages, ownership, gates, invariants, and handoff format.
- [`01-foundation-lifecycle.md`](01-foundation-lifecycle.md) — baseline seams, transaction origin, interaction anchors, state transitions, cancellation, undo, duplication, and invalidation.
- [`02-rendering-depth.md`](02-rendering-depth.md) — self-depth pass, scene occlusion, visibility treatment, geometry, and active drawing.
- [`03-feedback.md`](03-feedback.md) — Phase 1 handle state, HUD, annotations, measurements, and active-operation feedback.
- [`04-picking-sizing.md`](04-picking-sizing.md) — projection-derived sizing, semantic screen-space targets, arbitration, hysteresis, and overlap cycling.
- [`05-transform-math.md`](05-transform-math.md) — translation, rotation, scale, spaces, pivots, and multi-selection solvers.
- [`06-snapping-settings.md`](06-snapping-settings.md) — total-result snapping, precision, numeric entry, settings, and input presets.
- [`07-integration-validation.md`](07-integration-validation.md) — scene/viewport integration, reliability, performance, tests, acceptance evidence, and rollout.

Every implementation document links back to this specification and to the implementation index. If an implementation note conflicts with this document, this specification wins; record the conflict and resolution in the implementation ledger.

## Normative language and scope

`MUST` is a required behavior, `SHOULD` is the default behavior unless a documented product decision says otherwise, and `MAY` is optional.

This specification covers scene-editor 3D transform manipulation: translation, rotation, scale, gizmo rendering and acquisition, active feedback, snapping, transform spaces and pivots, hierarchy-safe multi-selection, input customization, undo, cancellation, reliability, performance, and validation.

It does not specify animation-curve editing, 2D UI rect transforms, VR manipulation, a general CAD constraint solver, runtime/game gizmos, or a wholesale editor renderer replacement.

## 1. Executive decisions

The redesign is a hybrid. Preserve Flax's useful geometric manipulation primitives, but replace the transaction model, visual depth model, scale mathematics, handle acquisition, and active-operation feedback.

| Keep from Flax | Replace or redesign |
| --- | --- |
| Ray-plane translation and camera-favorable axis planes | Incremental-only mutation as the source of truth |
| Signed geometric angle for axis and screen rotation | Additive scale and mouse-delta uniform scale |
| Front-half axis rings and a dedicated screen ring | Fixed draw order that destroys internal depth |
| View-plane center translation | 3D ray-hit selection as the sole motor-target model |
| Parent-child de-duplication for selection | Approximate distance-based apparent size |
| Mouse capture outside the viewport | Sparse and inconsistent manipulation feedback |
| One undo item for a normal drag | No general cancel and unsafe mid-drag state changes |

The primary design principle is that the gizmo is a screen-space control surface that manipulates 3D constraints. It is not merely a small 3D object rendered over the scene.

The target system MUST make every operation predictable, reversible, visually truthful, and accessible at hostile camera angles. It SHOULD remain fast enough to feel immediate at high refresh rates and with large selections.

## 2. Current Flax baseline and observed depth failure

The audited gizmo is a polling, incremental transform system. Each editor update computes a delta and immediately applies it to selected objects. Original transforms are captured only after the first non-zero delta. A normal release creates one undo action, but there is no general transform-cancel path. The current source exposes World and Local bases, Translate, Rotate, and Scale modes, and limited pivot policies.

Current translation uses consecutive ray-plane intersections. Axis and screen rotation already use a valid signed-angle model. Scale is additive: a small value is added to transform scale rather than applying a ratio. Picking uses analytic 3D boxes, spheres, and annuli. Existing active feedback is limited to handle focus, translation distance, trackball graphics, and vertex markers.

Runtime evidence shows that gizmo parts do not always preserve truthful depth ordering against one another. In the supplied overlap, +Z is farther from the camera yet blue renders over red. The visual system therefore communicates false spatial depth exactly when axes overlap.

Required correction: the gizmo MUST remain visible through scene geometry without sacrificing correct depth ordering among its own handles. Nearer red X geometry must cover farther blue Z geometry at their projected overlap.

## 3. Target interaction architecture

### 3.1 Transaction origin and interaction anchor

The replacement MUST use two layers of state:

| State layer | Stored data | Purpose |
| --- | --- | --- |
| Transaction origin | Original transforms, selection, hierarchy, pivot policy, duplicate set, and original bounds | Exact cancel and one coherent undo item |
| Interaction anchor | Anchor cursor/ray, camera matrices, current result, frozen plane/ring, active constraint, gain, and snap state | Stable manipulation with safe re-anchoring |

The transaction origin is immutable for the full action. The interaction anchor may be recreated whenever input semantics change. Snap, precision, camera clutching, and keyboard constraint changes SHOULD re-anchor at the current result without ending the transaction. Tool mode, transform space, and pivot-policy changes SHOULD be queued until release unless a safe same-transaction transition is explicitly implemented and tested.

### 3.2 Explicit state machine

The minimum state graph is:

```text
Inactive -> Hovering -> Armed -> Dragging -> Committing -> Inactive
                                      |-> Clutched -> Dragging
                                      |-> NumericEntry -> Dragging
                                      |-> Cancelling -> Inactive
```

| State | Required behavior |
| --- | --- |
| Hovering | Evaluate candidates, apply hysteresis, and display cursor and hover treatment. |
| Armed | Latch the semantic handle at mouse-down and capture the transaction origin. |
| Dragging | Solve from the current interaction anchor; hidden handles are not interactive. |
| Clutched | Freeze the result while the user orbits/pans; re-anchor after camera movement. |
| NumericEntry | Freeze pointer-driven results while an inline numeric field owns input. |
| Committing | Finalize expensive systems and create exactly one undo action. |
| Cancelling | Restore exact transaction-origin transforms and delete drag-created duplicates. |

During gizmo dragging, mouse capture loops the physical cursor across every desktop edge and exposes the accumulated wrap offset to solvers as one continuous pointer coordinate. The cursor HUD follows the wrapped physical cursor. Opposite edge crossings must cancel their offsets exactly, so retracing to the pickup coordinate reproduces the origin result without preventing motion through or beyond zero.

### 3.3 Commit, cancel, and focus loss

- Mouse release and Enter commit.
- Escape and the configured cancel mouse button restore the exact transaction origin.
- Focus loss freezes the transaction. If focus returns with the drag button released, cancel by default rather than silently committing uncertain input.
- Selection deletion or invalidation cancels affected objects safely and never leaves a stuck drag state.
- No-op operations create no undo item.

See [`01-foundation-lifecycle.md`](01-foundation-lifecycle.md) for the state ownership and transition implementation contract.

## 4. Rendering and depth semantics

### 4.1 Dedicated gizmo depth layer

The preferred implementation is a dedicated gizmo color and depth pass. It must preserve self-depth among gizmo parts while allowing the composed widget to remain readable over scene geometry.

1. Clear a gizmo-only depth buffer.
2. Render all gizmo geometry with normal depth writes into that buffer. Do not sort whole axes by a fixed X/Y/Z draw order.
3. Compare gizmo depth to scene depth to classify fragments as scene-visible or scene-occluded.
4. Composite scene-visible fragments at full treatment and scene-occluded fragments using the occluded treatment.
5. During an active drag, the active semantic handle MAY receive stronger scene-occluded treatment, but it must never violate gizmo self-depth.

If an offscreen composite is not immediately practical, clear or isolate depth before the gizmo pass and enable depth writes between gizmo parts. This is the minimum fix for the blue-over-red failure. A later scene-depth comparison pass may add through-object ghosting.

### 4.2 Visibility treatments

| Fragment condition | Default appearance | Interaction |
| --- | --- | --- |
| Visible and idle | 100% axis color with a dark 1–1.5 px outline | Normal screen-space hit target |
| Occluded by scene | 28% opacity with stipple or dashed treatment | Still selectable if its ghost is visible |
| Occluded by another gizmo part | Actually hidden by gizmo self-depth | Topmost semantic candidate wins |
| Hovered | Full color, white edge/halo, 8% enlargement | Candidate latched on press |
| Active drag | Full handle plus guide geometry | Only the active semantic handle remains interactive |
| Inactive during drag | Orientation lines at 18–22%; caps and plane targets hidden | Not interactive |

### 4.3 Backside and camera-angle semantics

Positive axis direction SHOULD remain stable. An arrow must not silently flip from +X to −X because that corrupts coordinate meaning. Plane handles have no directional sign and MAY mirror to the camera-facing quadrant.

The screen rotation ring remains available regardless of world-ring degeneracy.

See [`02-rendering-depth.md`](02-rendering-depth.md) for the render-pass and visibility contract.

## 5. Visual geometry, sizing, and accessibility

### 5.1 Projection-correct pixel sizing

The gizmo MUST be specified in pixels and converted to world scale from the active projection. The current camera-distance approximation should be removed.

For perspective projection:

```text
worldUnitsPerPixel =
    2 * forwardDepth * tan(verticalFov / 2) / viewportHeightPx
gizmoWorldRadius = desiredRadiusPx * worldUnitsPerPixel
```

For orthographic projection:

```text
worldUnitsPerPixel = orthographicViewHeight / viewportHeightPx
```

Use forward depth, not Euclidean camera distance. Clamp behavior near the camera and reject pivots behind the camera. DPI scaling MUST be included exactly once through the viewport's logical-to-physical coordinate conversion.

### 5.2 Recommended default dimensions

| Element | Visible default | Motor target | User range |
| --- | --- | --- | --- |
| Overall axis/ring radius | 96 px | N/A | 70–150 px |
| Axis shaft | 3 px plus outline | 16 px wide capsule | 2–6 px visual |
| Translate arrowhead | 16 × 16 px | 20 px envelope | 12–24 px |
| Scale cube cap | 14 px | 20 px envelope | 12–24 px |
| Plane handle | 18 × 18 px | Expanded by 6 px | 16–34 px |
| Center handle | 18 px | 24 px circle | 14–28 px |
| Axis rotation ring | 88 px radius, 4 px stroke | 16 px band | 70–140 px |
| Screen rotation ring | 95 px radius, 2.5 px stroke | 16 px band | 80–150 px |
| Axis labels | 11–12 px text | N/A | On hover / always / off |

### 5.3 Visual language

- X remains red, Y green, and Z blue. Every handle also has a neutral outline so color is not the only signal.
- Hover and active treatment must add thickness, outline, or size. Do not replace axis identity with a generic yellow highlight.
- Translation and scale MUST use the same established plane mapping: XY uses red, ZX uses blue, and YZ uses green. Plane borders SHOULD use the two included axis colors.
- The center free-move handle is neutral white/gray with a dark outline. Uniform scale and free rotation need distinct center glyphs rather than identical spheres.
- Optional X/Y/Z labels SHOULD appear at caps on hover and whenever perspective makes an axis ambiguous.

## 6. Picking and hover model

Picking MUST use projected screen-space primitives for intent acquisition, followed by world-space math for manipulation. Rendered meshes and their 3D collision volumes must not define the motor target.

| Handle | Screen-space primitive |
| --- | --- |
| Axis | Distance to projected line segment plus cap polygon |
| Plane | Point-in-projected-quadrilateral with expanded margin |
| Center | Circle or rounded rectangle |
| Axis ring | Distance to projected ellipse/front arc |
| Screen ring | Distance to screen-space circle |

### 6.1 Candidate resolution

1. Reject candidates outside their motor target.
2. If the cursor is at least 4 px inside a plane polygon, the plane wins over its edge axes.
3. If the cursor is inside a cap envelope, the cap/axis wins over the shaft.
4. Otherwise choose the smallest normalized screen distance.
5. When projected controls overlap, prefer the nearest cursor-local projected depth, consistent with rendered gizmo self-depth.
6. Retain the current hover until a new candidate improves the score by at least 20% or the cursor leaves an expanded retention zone.

A press latches the semantic handle until commit or cancel. Hidden handles are not selectable during an active operation. Tab or mouse wheel MAY cycle exact-overlap candidates as an advanced escape hatch.

### 6.2 Response requirements

- Hover treatment appears in the same rendered frame whenever possible.
- There is no artificial hover delay. Tooltips MAY appear after 500 ms.
- Direct manipulation has no application-level acceleration.
- Cursor icons reflect axis move, plane move, rotate, scale, or numeric-entry state.

See [`04-picking-sizing.md`](04-picking-sizing.md) for the acquisition and projection implementation contract.

## 7. Translation specification

### 7.1 Axis translation

The normal path SHOULD use the closest point between the selected world-space axis and the current mouse ray. Let `u` be the unit selected axis, `v` the unit mouse-ray direction, `w = pivot − rayOrigin`, and:

```text
B = dot(u, v)
D = dot(u, w)
E = dot(v, w)
den = 1 − B * B
axisT = (B * E − D) / den
delta = u * (axisT − anchorAxisT)
```

When `den` falls below a degeneracy threshold, use a camera-facing fallback plane that contains the axis. Freeze that plane for the current interaction anchor; do not switch planes every frame.

### 7.2 Plane and free translation

```text
anchorHit = intersect(anchorRay, frozenPlane)
currentHit = intersect(currentRay, frozenPlane)
rawDelta = currentHit − anchorHit
result = anchorResult + constrain(rawDelta)
```

Named planes use the chosen transform basis. Center drag uses a view-facing plane through the pivot. Mouse wheel MAY adjust depth while center-dragging. Pressing X, Y, or Z during center drag re-anchors to that axis without ending the transaction.

### 7.3 Translation feedback

| Operation | Handle treatment | World annotation | Cursor HUD |
| --- | --- | --- | --- |
| Axis | Active axis full; inactive lines 20%; other caps/planes hidden | Extended axis line, start marker, current marker, midpoint distance | `X +2.450 m`; `Position X 14.250 m` |
| Plane | Two active axes; normal axis faded; other planes hidden | Finite translucent plane patch or local grid | `XY`; `X +2.450 m`; `Y −0.800 m`; `Distance 2.577 m` |
| View plane | Center glyph active; axes faint | Camera-facing crosshair plane | `View plane 1.820 m`; optional `Depth −0.400 m` |
| Snap target | Active handle unchanged | Source-target tether and highlighted target | `X +2.500 m`; `Snap 0.500 m` |

The primary measurement is net displacement from the transaction origin. Do not show per-frame delta or cursor path length. Plane operations MAY additionally show Euclidean net distance.

## 8. Rotation specification

### 8.1 Axis and screen rotation

Retain Flax's signed-angle ring math, but evaluate it from a stable anchor and unwrap it into a total angle:

```text
v0 = normalize(anchorRingPoint − pivot)
v1 = normalize(currentRingPoint − pivot)
stepAngle = atan2(dot(axis, cross(v0, v1)), dot(v0, v1))
totalAngle = unwrap(anchorAccumulatedAngle + stepAngle)
result = axisAngle(axis, snapped(totalAngle)) * anchorOrientation
```

Axis rings use a world/local/custom axis. The outer ring uses the camera direction captured by the interaction anchor. Quaternion results MUST be normalized. The user may rotate through any number of complete revolutions.

### 8.2 Free rotation

- Keep the screen ring permanently available.
- Replace the current mouse-delta trackball with a true virtual arcball where practical. Retain a mouse-delta fallback for pointer-lock workflows.

### 8.3 Rotation feedback

- Hide unrelated rings rather than leaving three overlapping ellipses.
- Show the active ring, starting radius, current radius, and a translucent swept-angle wedge.
- Show angular snap ticks only while snapping is active and emphasize the chosen tick.
- Display signed, unwrapped delta as the primary value, such as `Local Z +407.5°`.
- A resulting Euler component MAY be secondary only when unambiguous. Do not imply that an arbitrary quaternion has one meaningful Euler result.

## 9. Scale specification

### 9.1 Multiplicative scale

Scale is a ratio. Remove the additive direct-manipulation model. A symmetric exponential mapping gives a consistent feel across tiny and huge existing scales:

```text
projectedDeltaPx = dot(cursor − anchorCursor, projectedHandleDirection)
factor = exp(projectedDeltaPx * ln(2) / pixelsPerDoubling)
Default pixelsPerDoubling = 120 px
```

Dragging 120 px in one direction doubles scale; dragging 120 px back halves it. Precision mode re-anchors and changes `pixelsPerDoubling` rather than multiplying all movement since mouse-down.

### 9.2 Axis, plane, uniform, and bounds behavior

Scaling MUST preserve Flax's existing TRS actor representation. Scale factors update local `Transform.Scale` components and MUST NOT introduce affine shear/skew, a shear field, or a matrix transform override. World mode may orient the scale controls in the world basis, but it does not imply sheared world-axis deformation of a rotated actor. Group operations may scale object positions around a shared pivot while each object's own shape remains representable by translation, quaternion orientation, and component scale.

| Operation | Factors | Position behavior |
| --- | --- | --- |
| Axis scale | `(f, 1, 1)` in gizmo basis | Individual mode leaves object position; group mode scales offset around pivot |
| Plane scale | `(f1, f2, 1)` | Same individual/group policy |
| Uniform scale | `(f, f, f)` | Preserves existing axis ratios |
| Bounds face | Factor from fixed opposite face | Moves object center so the opposite face remains anchored |
| Bounds corner | Three factors from opposite corner | Scales around the fixed opposite corner or selected pivot |

For group-position scaling in basis `B`:

```text
localOffset = inverse(B) * (objectPosition − pivot)
scaledOffset = componentMultiply(localOffset, factors)
newPosition = pivot + B * scaledOffset
```

### 9.3 Negative scale policy

- Prevent crossing zero by default; clamp to a small positive minimum.
- Allow mirroring only through an explicit setting or modifier.
- When mirroring is enabled, require a deliberate threshold beyond zero and show a handedness-flip warning in the HUD.

### 9.4 Scale feedback

Show the original bounds as a ghost and current bounds normally. Mark the fixed anchor face, edge, corner, or pivot. Display scale factor and resulting physical dimension; transform component scale alone is not sufficient feedback.

## 10. Active manipulation feedback

Every drag needs three coordinated layers:

| Layer | Location | Question answered |
| --- | --- | --- |
| Handle state | At the gizmo | What degree of freedom is active? |
| World annotation | Path, ring, target, or bounds | What changed spatially? |
| Cursor HUD | 18–24 px from cursor | What is the exact numeric result? |

The HUD prefers upper-right, then upper-left, lower-right, and lower-left of the cursor. It is clamped inside the viewport with at least 12 px edge margin, avoids the active handle and snap target, and keeps its chosen quadrant stable until invalid. Use a compact dark translucent panel with high-contrast text and update values every rendered frame.

Use the project unit formatter. Match decimal precision to snap step, show signs on deltas rather than absolute dimensions, trim meaningless trailing zeros, and keep the inspector secondary to the cursor and object.

| State | Active handle | Other handles | Extra feedback |
| --- | --- | --- | --- |
| Hover | 8% larger, white outline, semantic cursor | Normal | Optional delayed tooltip |
| Pressed | Latched with brief 80 ms emphasis | Begin fade | Constraint guide appears |
| Dragging translate | Full color and extended line/plane | Axes 20%; caps/planes hidden | Start/current markers and delta |
| Dragging rotate | Only active ring full | Other rings hidden; axes 10–15% | Wedge, radii, angle, snap ticks |
| Dragging scale | Active cap/center full | Unused targets hidden | Original/current bounds, anchor, factor, dimension |
| Snapped | Active handle unchanged | Unchanged | Target marker, tether, snap label |
| Precision | Unchanged | Unchanged | HUD badge such as `Precision 0.1×` |
| Cancelled | Restore immediately | Return to idle | Optional brief `Canceled` toast |

Do not animate continuously. A one-time activation emphasis is acceptable; pulsing, breathing, and moving gradients make a precision editor feel unstable.

## 11. Snapping, precision, and numerical input

### 11.1 Snap the total result

Snapping MUST use the value solved from the interaction anchor or transaction origin so sampling frequency cannot change the final result:

```text
rawTotal = solve(anchor, currentPointer)
snappedTotal = roundToStep(rawTotal, step, referenceFrame)
result = anchorResult + snappedTotal
```

### 11.2 Semantic snapping

Grid, vertex, edge, face, surface, bounds, socket, and pivot targets use the same candidate framework. Targets remain sticky until another candidate is meaningfully better or the pointer leaves a release radius. Show source and target markers, a tether line, and the semantic target name. Allow cycling close candidates instead of silently hopping every update. Orientation alignment to a face normal is a distinct option and must be previewed before commit.

### 11.3 Precision and gain

Direct translation and rotation remain linear; do not add application-level acceleration. Precision mode changes gain after re-anchoring, so entering or leaving precision causes no jump.

| Control | Default behavior |
| --- | --- |
| Precision modifier | 0.1× translation/rotation gain; 10× pixels per scale doubling |
| Fine precision chord | Optional 0.01× |
| Snap modifier | Temporary snap using the configured step |
| Mouse wheel during free move | Adjust camera depth or cycle snap candidates, context-dependent |

### 11.4 Numeric entry

Typing during a drag opens an inline numeric field in the cursor HUD. Axis translation accepts a distance; plane translation accepts two components; rotation accepts degrees; scale accepts factor, percent, or resulting dimension. Tab moves between components. Enter commits. Escape exits numeric entry first, then a second Escape cancels the whole transaction. Expressions MAY be supported later, but exact signed numeric input is required.

## 12. Spaces, pivots, hierarchy, and multi-selection

### 12.1 Transform spaces

| Space | Requirement |
| --- | --- |
| World | Required |
| Local | Required; active object defines shared basis for multi-selection |
| Parent | Required for hierarchy-heavy scene work |
| View | Required for camera-relative placement and screen rotation |
| Custom/workplane | Required; create from face, edge, socket, or saved basis |
| Gimbal | Recommended for animation-oriented workflows |

### 12.2 Pivot policies

Support active object origin, selection world-bounds center, average selected pivots, individual origins, world origin or scene cursor, a temporary movable pivot, and the opposite-side bounds pivot during cage scaling.

### 12.3 Multi-selection behavior

Retain Flax's top-level parent filtering so a selected child is not directly transformed when its selected ancestor already carries the transformation. Make group and individual behavior explicit rather than implicit.

| Operation | Group pivot | Individual origins |
| --- | --- | --- |
| Translate | Same world delta to top-level selected objects | Equivalent for ordinary translation |
| Rotate | Rotate orientations and positions around shared pivot | Rotate each orientation around its own pivot |
| Scale | Scale components and positions around shared pivot | Scale each object around its own pivot |

The dimension HUD MUST label its measurement basis: active object local size, aggregate world bounds, or group scale factor. A rotated world AABB must never be presented as intrinsic object width without qualification.

## 13. Input model and customization

### 13.1 Required input actions

| Action | Recommended default | Notes |
| --- | --- | --- |
| Translate / Rotate / Scale | W / E / R | Preserve Flax muscle memory |
| Axis constraint | X / Y / Z during operation | Re-anchor without ending transaction |
| Plane exclusion | Shift+X / Shift+Y / Shift+Z | Select the other two-axis plane |
| Temporary snap | Ctrl | Preserve current convention |
| Precision | Alt or rebindable | Must coexist with camera navigation |
| Duplicate while dragging | Shift or rebindable | Duplicate once at first valid motion |
| Cancel | Escape / configured mouse button | Exact restore |
| Commit | Mouse release / Enter | One undo item |
| Camera clutch | Space or middle mouse | Freeze, navigate, re-anchor |

Bindings MUST be action-based and fully rebindable. Ship Flax, Blender-like, Maya-like, and Unreal-like presets rather than forcing one muscle-memory model.

### 13.2 User-facing settings

| Category | Settings |
| --- | --- |
| Appearance | Radius, line thickness, opacity, occluded opacity, brightness, labels, plane opacity |
| Acquisition | Hit-target scale, hover hysteresis, overlap cycling |
| Manipulation | Scale pixels-per-doubling, precision factor, negative-scale policy, free-move depth gain |
| Feedback | Cursor HUD, world labels, dimensions, result value, snap target text, decimal/unit mode |
| Behavior | Default space, pivot, group/individual mode, camera clutch, focus-loss policy |
| Input | Bindings and editor presets |

Do not expose every engineering threshold in the primary settings UI. Put motor-target sizes, degeneracy thresholds, and advanced depth style under an Advanced section.

## 14. Reliability and performance requirements

### 14.1 Determinism and numerical stability

- Given the same transaction origin, anchor, camera, and final pointer position, the result must not depend on update frequency.
- Use double precision for interaction calculations where available, even if final scene transforms are floats.
- Normalize quaternions and clamp scale away from numerical zero.
- Never accumulate the object transform itself per frame as the only authoritative state.
- Modifier changes re-anchor at the current result, preserving continuity.

### 14.2 Scene-system updates

- Cheap transform preview updates occur every frame.
- Expensive physics, navigation, lighting, procedural generation, and asset rebuilds SHOULD be deferred or throttled until commit where safe.
- Undo captures original and final arrays once, not a history record for every update.
- Large selections use batched notifications and avoid recomputing aggregate bounds more than necessary.
- Semantic snap scene searches run only while snap targeting is active and SHOULD use spatial acceleration structures.

### 14.3 Failure handling

- An exception during preview restores or safely preserves the transaction and exits input capture.
- Deleting or unloading an affected scene node cannot leave a stale pointer in undo data.
- Switching tools, pivot, or space during a drag cannot silently discard undo state.
- Mouse capture loss cannot leave the gizmo visually active.

## 15. Implementation handoff

Detailed implementation steps are externalized to [`implementation.md`](implementation.md) and its focused subdocuments. The behavior specification remains the source of truth for all implementation decisions.

The implementation must preserve existing scene transforms, undo serialization, snap settings, keyboard action infrastructure, and parent-child filtering wherever they are not responsible for a specified UX defect. This redesign is not permission to fork every editor subsystem at once.

## 16. Acceptance test matrix

### 16.1 Visual depth and accessibility

| Test | Pass condition |
| --- | --- |
| Provided +Z-away screenshot orientation | Nearer red X covers farther blue Z at overlap; blue remains visible where not covered |
| Scene occlusion | Occluded handle is ghosted but readable and selectable; internal gizmo depth stays correct |
| Front/back orbit | Axis direction remains semantically stable; plane handles may mirror without ambiguity |
| Dark, light, and saturated backgrounds | Outline and axis identity remain readable |

### 16.2 Interaction and reliability

| Test | Pass condition |
| --- | --- |
| 30, 60, 120, and 240 FPS pointer replay | Final transform differs by less than numerical tolerance |
| Precision toggled mid-drag | No jump; gain changes from current result |
| Snap toggled mid-drag | No jump; HUD explains snapped value |
| Camera clutch mid-drag | Object freezes during navigation and resumes continuously |
| Drag across each screen edge and retrace to the anchor | Cursor loops in every direction without a result jump; the object returns exactly to its origin result |
| Escape after move/rotate/scale/duplicate | Exact starting state restored and no undo item remains |
| Tool or pivot shortcut mid-drag | Queued, safely transitioned, or explicitly blocked; never loses undo |
| Parent and child selected | No double transform |
| Ten full rotations | Angle unwraps; no sudden sign flip or large drift |
| Scale toward zero | Default policy prevents accidental mirror |

### 16.3 Feedback correctness

- Translation shows net delta from transaction origin, not per-update delta.
- Rotation shows signed accumulated degrees and the correct snap step.
- Scale shows factor and old-to-new dimension in the declared measurement basis.
- Hidden handles are not clickable while another handle is active.
- HUD never covers the cursor, active cap, or snap target when another quadrant is available.

## Appendix A. Current Flax versus target system

| Area | Current Flax | Target |
| --- | --- | --- |
| State | Scattered booleans; incremental application | Explicit state machine, immutable transaction origin, re-anchorable interaction |
| Cancel | No general restore path | Escape/configured cancel exact restore |
| Depth | Fixed ordering can show farther axis over nearer axis | Dedicated self-depth; optional through-scene ghost pass |
| Sizing | Distance/24 approximation; separate orthographic multiplier | Projection-derived constant pixel size |
| Picking | 3D boxes/spheres/annuli and fixed ordering | Screen-space semantic targets, hysteresis, depth-aware overlap |
| Translate | Incremental consecutive ray-plane deltas | Anchor-based closest-axis or frozen-plane solve |
| Rotate | Good signed-angle rings; mouse-delta trackball | Keep rings, unwrap total, normalize, true arcball |
| Scale | Additive 0.01 delta; can cross zero | Multiplicative exponential factor; explicit mirror policy |
| Feedback | Focus material, axis distance, trackball and vertex graphics | Handle state + world annotation + cursor HUD |
| Snapping | Incremental residual; vertex target reacquired each update | Snap total result; sticky semantic candidates |
| Multi-select | Parent de-duplication; shared rotation; no group position scale | Retain de-duplication; explicit group/individual rotation and scale |
| Customization | Size/brightness/opacity/snap and bindings | Add acquisition, depth, feedback, gain, scale, and policy settings |

## Appendix B. Recommended defaults and audit references

### B.1 Default values

| Setting | Default |
| --- | --- |
| Gizmo radius | 96 px |
| Axis visual width / hit width | 3 px / 16 px |
| Plane visual size / hit expansion | 18 px / 6 px |
| Center visual / hit size | 18 px / 24 px |
| Rotation ring / screen ring radius | 88 px / 95 px |
| Occluded-by-scene opacity | 0.28 |
| Inactive-during-drag opacity | 0.20 |
| Hover enlargement | 8% |
| Hover hysteresis advantage | 20% |
| Scale sensitivity | 120 px per 2× |
| Precision factor | 0.1× |
| HUD cursor offset | 20 px nominal, 12 px viewport margin |

### B.2 Audit references

- Audit basis: commit `9e3a89300`; no runtime build was performed in the audit.
- Polling/incremental model and undo: `TransformGizmoBase.Update` L787–985; `StartTransforming` L141–172; `TransformGizmo.OnEndTransforming` L346–352.
- Current sizing: `TransformGizmoBase.UpdateMatrices` L220–256; `GizmoScaleFactor = 24`.
- Current picking: `TransformGizmoBase.Selection.cs SelectAxis` L215–356.
- Current translation: `TransformGizmoBase.UpdateTranslateScale` L258–434.
- Current rotation: `UpdateRotateTrackball` L493–532; `UpdateRotateRing` L621–648; `UpdateRotateScreen` L664–711; `UpdateRotate` L719–781.
- Current additive scale: `UpdateTranslateScale` L351–378 and L423–433; viewport `ApplyTransform` L1195–1201.
- Current feedback: `TransformGizmoBase.Draw.cs` L407–637 and L665–839.
- Selection hierarchy filtering: `SceneGraphTools.BuildNodesParents` L58–78.
- Observed depth-order failure: runtime screenshots supplied with the source document.

Runtime validation is still required for material depth state, exact imported mesh dimensions, DPI behavior, extreme-angle usability, and frame-rate equality. The supplied screenshots prove the internal visual depth-order failure in the shown configuration.
