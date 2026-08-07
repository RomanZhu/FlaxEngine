# Implementation: Active Feedback

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note is the Phase 1 implementation contract for P1-01, P1-02, and P1-03. It owns the data contract for active handle state, world annotations, cursor HUD placement, and mode-specific feedback. Total-result snapping and numeric entry are intentionally deferred to [`06-snapping-settings.md`](06-snapping-settings.md).

## Feedback model

Every drag produces three coordinated layers:

| Layer | Location | Question answered |
| --- | --- | --- |
| Handle state | Gizmo | What degree of freedom is active? |
| World annotation | Path, ring, target, or bounds | What changed spatially? |
| Cursor HUD | 18–24 px from cursor | What is the exact numeric result? |

The feedback model is pure data. The solver produces net values, measurement basis, warnings, snap identity, and guide geometry. The renderer and HUD consume it; neither may mutate transforms or duplicate solver math.

## HUD contract

Prefer upper-right, then upper-left, lower-right, and lower-left of the cursor. Clamp inside the viewport with at least 12 px edge margin. Avoid the active handle, snap target, and selected geometry edge. Keep the chosen quadrant stable until invalid. Use a compact dark translucent panel with high-contrast text and update values every rendered frame.

Use the project unit formatter. Match decimal precision to snap step, show signs on deltas rather than absolute dimensions, trim meaningless trailing zeros, and keep the inspector secondary.

Measurements must be truthful:

- translation shows net displacement from transaction origin, not frame delta or cursor path length;
- rotation shows signed unwrapped degrees and snap step;
- scale shows the active factor by default; physical dimensions remain contextual rather than permanently occupying the drag HUD;
- any dimensions added by a dedicated bounds/cage workflow must identify active local size, aggregate world bounds, or group factor.

## Mode-specific feedback

| Mode | Required feedback |
| --- | --- |
| Translation axis | Origin-to-current line with the active arrow mirrored onto the signed-motion side, net axis delta |
| Translation plane | Active axes, translucent plane patch/local grid, two component deltas, optional Euclidean distance |
| View-plane move | Camera-facing crosshair plane, view-plane distance, optional depth |
| Rotation | Complete orientation frame, emphasized active ring or lit trackball sphere, signed unwrapped angle |
| Scale | Subtle current bounds cue and active factor |
| Semantic snap | Source/target markers, tether, target name, snapped value |

Hide unrelated translation and scale handles during an active drag. Keep the complete rotation frame visible so hovering or dragging never makes the orientation context disappear. A one-time 80 ms pressed emphasis is allowed; continuous pulsing is not.

Runtime presentation review on 2026-08-06 explicitly replaced the original wedge, endpoint-marker, original-bounds ghost, fixed-anchor, and multi-row measurement treatment. Those elements obscured the operation and made the tool feel diagrammatic rather than professional. The approved direction is one primary value and the smallest world cue that communicates motion. During an axis translation drag, the focused arrow is allowed to mirror to the signed-motion side and forms the visible tip of the origin-to-current line. Translation recenters the pointer on the pivot before solving so the displayed line and signed motion share a stable origin. Plane translation uses a yellow outline matching the rendered plane handle exactly; the larger acquisition box does not affect its visual size, and it does not expand into a grid. Axis-scale feedback replaces the world-space handle with a screen-space line constrained to the projected active axis; its arrowhead slides to the cursor's nearest position on that axis. Plane scale hides its constituent axis hands and bounds cage and uses an equivalently sized yellow cursor rectangle. Scale follows the selected transform space: local orientation in Local mode and identity/world orientation in World mode. Translation and scale leave a magenta point at the transaction origin after motion begins. Trackball hover/drag uses a subdued filled focus sphere with complete, solid RGB orientation rings; the sphere occludes their back-facing portions without depth-fighting them. During trackball drag, a translucent gray screen-space patch connects the pivot and clamped endpoints. Its outer edge is straight for a center press and curves progressively as the press approaches the rim; no wedge/dash construction graphics are used.

During an active free/center translation, Alt remains a precision modifier and must not also initiate the viewport's Alt+left orbit capture. Holding Shift gives the center handle temporary surface snapping: cast the current cursor ray through the scene, ignore the selected hierarchies, use the same screen-space geometry candidate filtering as V-snap, then place the pivot at that candidate's exact ray/surface intersection. Cameras, icons, generic actor bounds, and enclosing helper volumes are not candidates. A yellow target point marks a valid hit. In this center-handle context surface snapping takes precedence over Shift-to-duplicate; constrained translation handles retain the existing duplication gesture. Free translation has no camera-plane grid, and plane translation hides the two constituent axis arrows while the cursor-sized yellow plane rectangle is active. Where axis and plane acquisition regions overlap, compare their camera-ray intersection depths consistently and select the nearer handle.

Translation and scale use the same neutral crosshair for the transaction origin; the former magenta dot is not part of the presentation. Axis translation derives arrow mirroring from the snapped origin-relative result and preserves its distance visual across modifier re-anchoring. Rotation uses a 30-degree default snap step. Its cursor HUD keeps the quadrant chosen at drag start for the entire rotation instead of switching sides around moving ring/bounds obstacles. The trackball's live endpoint moves freely across the virtual sphere. Its virtual solution and acquisition radius extend to the inner edge of the gray outline, which is the clamp boundary rather than a forced endpoint path. The yellow hover sphere remains at the smaller visual radius and the RGB orientation rings remain at their original radius, keeping the rings visible and independently selectable before the interior trackball region.

Translation grid snapping rounds the transaction total. In world space it rounds the selected transform's final coordinate against the world grid; in local space it rounds the origin-relative signed distance. Re-anchoring for precision or temporary snapping preserves that total. The translation HUD continues to report signed net displacement from the transaction origin, so an absolute-grid correction may intentionally produce a value that is not itself a multiple of the displayed step. The trackball's live triangle point follows the cursor inside the projected sphere and clamps to its boundary outside it.

## Implementation checklist

- [x] P1-01: expose idle, hover, pressed, dragging, snapped, precision, and cancelled handle state.
- [x] P1-02: expose net result, measurement basis, and world-guide data to the feedback layer.
- [x] P1-03: render mode-specific overlays without duplicating transform solving.
- [x] Keep HUD placement and formatting independent of the transform solver.

## Completion evidence

The implementation adds `FeedbackModel` as the pure solver-to-renderer seam, latches the active `SemanticHandle`, and renders minimal translation, rotation, scale, snap, HUD, and cancellation annotations from transaction totals. Rotation keeps its full frame and uses a focused depth-occluding trackball with solid rings plus a cursor-driven screen-space drag patch rather than wedges or dashed construction marks; axis translation recenters the pointer and mirrors the focused arrow onto the signed-motion end of its line; active axis scaling places its screen-space arrowhead at the cursor's projected position on the active axis while retaining a single factor row and subdued current-bounds cue. Translation reports the origin-relative displacement even when absolute snapping applies a residual correction, while absolute rotation and scale report their final components. Static inspection confirms that hidden translation/scale handles are rejected by picking during an active drag and that HUD placement uses the required quadrant order, edge margin, obstacle checks, and project unit formatter.

Runtime drag capture and the focused build/test command remain pending an authorized validation run. Cross-check `INV-09` and `INV-10` in the [implementation index](implementation.md).

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
