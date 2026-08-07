# Implementation: Snapping, Precision, and Settings

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note is the Phase 4/5 implementation contract for P4-04, P5-01, P5-02, and the settings portion of P5-04. It follows the core solvers in [`05-transform-math.md`](05-transform-math.md) and consumes the feedback model from [`03-feedback.md`](03-feedback.md).

## Total-result snapping

Snap the solved total, not the latest frame delta:

```text
rawTotal = solve(anchor, currentPointer)
snappedTotal = roundToStep(rawTotal, step, referenceFrame)
result = anchorResult + snappedTotal
```

Grid, vertex, edge, face, surface, bounds, socket, and pivot targets use the same candidate framework. Targets remain sticky until another candidate is meaningfully better or the pointer leaves a release radius. Allow cycling close candidates. Face-normal orientation alignment is a distinct option and must be previewed before commit.

## Precision and numeric entry

Direct translation and rotation remain linear; application-level acceleration is not allowed. Precision changes gain after re-anchoring:

| Control | Default |
| --- | --- |
| Precision modifier | 0.1× translation/rotation gain; 10× pixels per scale doubling |
| Fine precision chord | Optional 0.01× |
| Snap modifier | Temporary use of current snap step |
| Mouse wheel during free move | Camera depth or snap-candidate cycling, context-dependent |

Typing during a drag opens an inline numeric field in the cursor HUD. Axis translation accepts a distance; plane translation accepts two components; rotation accepts degrees; scale accepts factor, percent, or resulting dimension. Tab moves between components, Enter commits, and Escape exits numeric entry first. A second Escape cancels the transaction. Exact signed numeric input is required; expression support is optional.

## User-facing settings

| Category | Settings |
| --- | --- |
| Appearance | Radius, line thickness, opacity, occluded opacity, brightness, labels, plane opacity |
| Acquisition | Hit-target scale, hover hysteresis, overlap cycling |
| Manipulation | Scale pixels-per-doubling, precision factor, negative-scale policy, free-move depth gain |
| Feedback | Cursor HUD, world labels, dimensions, result value, snap target text, decimal/unit mode |
| Behavior | Default space, pivot, group/individual mode, camera clutch, focus-loss policy |
| Input | Bindings and editor presets |

Do not expose every engineering threshold in the primary settings UI. Put motor-target sizes, degeneracy thresholds, and advanced depth style under an Advanced section.

Required action defaults are W/E/R for Translate/Rotate/Scale, X/Y/Z axis constraints, Shift+X/Y/Z plane exclusion, Ctrl temporary snap, Alt precision, Shift duplication, Escape cancel, release/Enter commit, and Space or middle mouse camera clutch. All bindings are action-based and rebindable. Provide Flax, Blender-like, Maya-like, and Unreal-like presets.

## Implementation checklist

- [ ] P4-04: add keyboard constraints and inline numeric entry inside one transaction.
- [ ] P5-01: implement update-rate-independent grid/angle snapping from total result.
- [ ] P5-02: implement sticky semantic targets, source/target markers, tether, and cycling.
- [ ] P5-04: persist settings, action bindings, and input presets through existing editor infrastructure.

## Completion evidence

Capture precision, snapping, numeric-entry, settings, and input-preset cases. Verify snap candidates do not hop every update, changing precision/snap causes no jump, numeric Entry/Enter/Escape behavior preserves one transaction, and `INV-04`, `INV-05`, and `INV-10` remain true in the [implementation index](implementation.md).

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
