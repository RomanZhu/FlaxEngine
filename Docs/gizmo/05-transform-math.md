# Implementation: Transform Math

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note decomposes W01, P3-01 through P3-03, P4-01 through P4-03, and P5-03. It owns pure translation, rotation, and scale solvers plus spaces, pivots, hierarchy filtering, and group/individual policy.

## Solver contract

Solvers consume an immutable `TransactionOrigin`, a replaceable `InteractionAnchor`, the latched semantic handle, and current pointer/input data. They return an `InteractionResult`; they do not mutate scene objects or accumulate from the last applied object transform.

Given the same origin, anchor, camera, and final pointer position, the result must be independent of update frequency. Use double precision where available, normalize quaternions, reject non-finite results, and protect scale from accidental numerical zero.

## Translation

For a selected world-space axis, use the closest point between the axis and the current mouse ray:

```text
u = unit selected axis
v = unit mouse-ray direction
w = pivot - rayOrigin
B = dot(u, v)
D = dot(u, w)
E = dot(v, w)
den = 1 - B * B
axisT = (B * E - D) / den
delta = u * (axisT - anchorAxisT)
```

When `den` is below a degeneracy threshold, use a camera-facing fallback plane containing the axis and freeze it for the anchor. For plane/free movement:

```text
anchorHit = intersect(anchorRay, frozenPlane)
currentHit = intersect(currentRay, frozenPlane)
rawDelta = currentHit - anchorHit
result = anchorResult + constrain(rawDelta)
```

Named planes use the transform basis. Center drag uses a view-facing plane through the pivot. Mouse wheel MAY adjust depth. X/Y/Z during a center drag re-anchors to the selected axis without ending the transaction.

## Rotation

Retain signed-angle ring math, but solve from a stable anchor and unwrap the accumulated result:

```text
v0 = normalize(anchorRingPoint - pivot)
v1 = normalize(currentRingPoint - pivot)
stepAngle = atan2(dot(axis, cross(v0, v1)), dot(v0, v1))
totalAngle = unwrap(anchorAccumulatedAngle + stepAngle)
result = axisAngle(axis, snapped(totalAngle)) * anchorOrientation
```

World, local, and custom axes are supported. The outer ring captures camera direction in the anchor. Normalize every quaternion and allow multiple complete revolutions. Use a true virtual arcball where practical; retain a mouse-delta fallback for pointer-lock workflows.

## Multiplicative scale

Scale is a ratio and uses a symmetric exponential mapping:

```text
projectedDeltaPx = dot(cursor - anchorCursor, projectedHandleDirection)
factor = exp(projectedDeltaPx * ln(2) / pixelsPerDoubling)
pixelsPerDoubling = 120 px by default
```

Dragging 120 px doubles scale and dragging back halves it. Precision changes `pixelsPerDoubling` after re-anchoring.

| Operation | Factors | Position behavior |
| --- | --- | --- |
| Axis | `(f, 1, 1)` in gizmo basis | Individual leaves position; group scales offset around pivot |
| Plane | `(f1, f2, 1)` | Same individual/group policy |
| Uniform | `(f, f, f)` | Preserves axis ratios |
| Bounds face | Factor from opposite face | Keeps the opposite face anchored |
| Bounds corner | Three factors from opposite corner | Keeps the opposite corner or selected pivot anchored |

Scale storage policy is deliberately TRS-only. Axis, plane, and uniform factors update the actor's existing local `Transform.Scale` components; they MUST NOT create, serialize, or approximate affine shear/skew. World mode may present world-aligned scale handles, but it does not promise a sheared world-axis deformation for a rotated actor. Group scaling may still move object positions around a shared pivot while every individual actor remains representable by its existing translation, quaternion orientation, and component scale.

For group-position scaling in basis `B`:

```text
localOffset = inverse(B) * (objectPosition - pivot)
scaledOffset = componentMultiply(localOffset, factors)
newPosition = pivot + B * scaledOffset
```

Prevent crossing zero by default. Mirroring requires an explicit setting/modifier, a deliberate threshold beyond zero, and a handedness warning.

## Spaces, pivots, and selection

Required spaces are World, Local, Parent, View, and Custom/workplane. Gimbal is recommended for animation-oriented workflows. Supported pivot policies include active origin, selection bounds center, average pivots, individual origins, world origin/scene cursor, temporary movable pivot, and the opposite-side bounds pivot for cage scale.

Accepted scale-space policy: the scale gizmo renders and picks in the World-space identity basis when the toolbar reports World, while application continues to multiply local `Transform.Scale` components. This preserves Flax's existing TRS actor representation and intentionally avoids skew. P3-02 and P5-03 must retain that policy while making factor, pivot, group-position, and feedback behavior explicit; they must not add an affine/shear transform path.

Retain top-level parent filtering: a selected child must not receive a second direct transform when its selected ancestor already carries the operation.

| Operation | Group pivot | Individual origins |
| --- | --- | --- |
| Translate | Same world delta to top-level objects | Equivalent for ordinary translation |
| Rotate | Rotate orientations and positions around shared pivot | Rotate each orientation around its own pivot |
| Scale | Scale components and positions around shared pivot | Scale each object around its own pivot |

The measurement basis used by feedback must be explicit: active local size, aggregate world bounds, or group factor. A rotated world AABB is not intrinsic object width without qualification.

## Implementation checklist

- [ ] W01: extract pure math helpers and establish deterministic test seams.
- [ ] P3-01: replace additive scale with exponential multiplicative factor mapping.
- [ ] P3-02: implement axis/plane/uniform, group/individual, bounds-face, and bounds-corner policy while preserving the TRS-only, no-skew scale decision.
- [ ] P3-03: enforce zero-crossing default and explicit mirroring warning.
- [ ] P4-01: implement closest-axis and frozen-plane translation with degeneracy fallback.
- [ ] P4-02: implement stable ring rotation, unwrap, quaternion normalization, and arcball.
- [ ] P4-03: make camera/modifier changes re-anchor with no result jump.
- [ ] P5-03: implement spaces, pivots, hierarchy filtering, and explicit multi-selection behavior; keep toolbar state, rendered/picked scale basis, and solver basis consistent.

## Completion evidence

Use pointer replays at 30/60/120/240 FPS, ten full rotations, precision/snap/camera transitions, scale-toward-zero, group/individual selection, parent-child selection, and rotated-bounds measurements. Cross-check `INV-02`, `INV-03`, `INV-05`, `INV-06`, and `INV-07` in the [implementation index](implementation.md).

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
