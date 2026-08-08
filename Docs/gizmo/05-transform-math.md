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

Scale is a ratio and uses an anchor-based exponential mapping. Testing showed that a symmetric 120-pixel response made positive growth explode near a screen edge, while the shrink response felt correct. The applied pointer mapping therefore keeps 120 pixels per halving when shrinking and uses 1,200 pixels per doubling when growing:

```text
projectedDeltaPx = dot(cursor - anchorCursor, projectedHandleDirection)
gain = projectedDeltaPx > 0 ? 0.1 : 1.0
factor = exp(projectedDeltaPx * gain * ln(2) / 120)
```

Dragging 1,200 px in the growth direction doubles scale; dragging 120 px in the shrink direction halves it. Returning to the anchor still produces exactly factor one. Precision applies another 0.1 gain after re-anchoring.

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

- [x] W01: extract pure math helpers and establish deterministic test seams.
- [x] P3-01: replace additive scale with exponential multiplicative factor mapping.
- [ ] P3-02: axis/plane/uniform factors and group-pivot position scaling are implemented; individual bounds-face and bounds-corner cage policy remains.
- [ ] P3-03: ordinary pointer scaling cannot cross numerical zero; explicit mirroring controls and handedness warning remain.
- [x] P4-01: implement closest-axis and frozen-plane translation with degeneracy fallback.
- [x] P4-02: implement stable ring rotation, unwrap, quaternion normalization, and arcball.
- [x] P4-03: make camera/modifier changes re-anchor with no result jump.
- [ ] P5-03: existing World/Local spaces, existing pivots, top-level hierarchy filtering, and shared-pivot group behavior are explicit; Parent/View/Custom spaces and additional pivot policies remain.

## Implemented core slice

- Translation is solved from the replaceable interaction anchor. Axis movement uses closest-line math with a screen-projected degeneracy fallback; plane and center movement use frozen anchor planes.
- Ring rotation uses an anchor angle with unwrap for multiple revolutions. Trackball rotation uses an anchor-based virtual arcball, including the antipodal case.
- Axis, plane, and uniform scaling use the tuned exponential mapping: 1,200 pixels per positive doubling and 120 pixels per negative halving. Scale snapping acts on total factors and leaves inactive components untouched.
- Shared-pivot scaling updates top-level selected-object positions in the captured gizmo basis. Actor transforms remain TRS-only.
- The Armed-to-Dragging transition preserves the click-time anchor, and modifier/camera re-anchors preserve the current result.
- Pointer interaction starts where the handle was grabbed; it does not warp the cursor to the pivot. Continuous wrapped pointer coordinates remain symmetric when crossing and returning over viewport edges.

## Completion evidence

Pure tests cover exponential scale symmetry and composition, positive minimum factors, axis and plane translation, ring unwrap, regular and antipodal arcball rotation, and basis-aware group-pivot scaling. They have not been executed in this pass because local build/test execution requires explicit authorization.

Manual pointer replays at 30/60/120/240 FPS, ten full rotations, precision/snap/camera transitions, scale-toward-zero, group/individual selection, parent-child selection, and rotated-bounds measurements remain required. Cross-check `INV-02`, `INV-03`, `INV-05`, `INV-06`, and `INV-07` in the [implementation index](implementation.md).

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
