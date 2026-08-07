# Implementation: Rendering and Depth

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note decomposes H01, P1-01, and P1-03. It owns gizmo self-depth, scene-occluded treatment, active visual states, and the geometry/drawing contract. It does not own semantic hit arbitration or transform solving.

## Render-pass contract

The preferred implementation is a dedicated gizmo color and depth pass:

1. Clear a gizmo-only depth buffer.
2. Render all gizmo geometry with normal depth writes into that buffer.
3. Never sort complete axes by fixed X/Y/Z draw order.
4. Compare gizmo depth with scene depth to classify scene-visible and scene-occluded fragments.
5. Composite visible fragments at full treatment and occluded fragments with the occluded treatment.
6. During a drag, emphasize the active semantic handle only through its scene-occlusion style; never bypass gizmo self-depth.

The minimum acceptable hotfix is to clear or isolate depth before the gizmo pass and enable depth writes between gizmo parts. This must correct the supplied blue-over-red overlap. A later scene-depth comparison pass may add through-object ghosting.

## Visibility and state styles

| Condition | Default style | Input behavior |
| --- | --- | --- |
| Visible and idle | 100% axis color; dark 1–1.5 px outline | Normal semantic target |
| Occluded by scene | 28% opacity; stipple/dash treatment | Selectable when its ghost is visible |
| Occluded by gizmo part | Hidden by gizmo depth | Topmost candidate wins |
| Hovered | Full color; white edge/halo; 8% enlargement | Latch on press |
| Active drag | Full active handle plus guides | Only active semantic handle is interactive |
| Inactive during drag | Orientation lines 18–22%; caps/planes hidden | Not interactive |

X, Y, and Z remain red, green, and blue. Every handle needs a neutral outline so color is not the only signal. Hover and active states add thickness, outline, or size rather than replacing the axis identity with yellow. Translation and scale use the same established plane mapping: XY is red, ZX is blue, and YZ is green. Plane borders use the two included axis colors.

## Camera-angle behavior

Positive axis direction remains semantically stable; do not flip an arrow from +X to −X to improve visibility. Plane handles may mirror to the camera-facing quadrant because planes have no directional sign.

Keep the screen rotation ring available when world-ring geometry is degenerate.

## Geometry and active overlays

The renderer consumes `SemanticHandle` and `FeedbackModel` data. It must not perform hit testing or mutate scene transforms. Active visuals include:

- translation: extended axis, start/current markers, midpoint distance, finite plane patch or local grid;
- rotation: active ring, starting/current radius, swept-angle wedge, and optional snap ticks;
- scale: ghost original bounds, current bounds, and the fixed face/edge/corner/pivot;
- snapping: source marker, target marker, tether, and target label.

Do not animate continuously. A one-time activation emphasis is acceptable; pulsing, breathing, and moving gradients are not appropriate for precision manipulation.

## Implementation checklist

- [x] W00: locate the current gizmo draw path, material state, viewport composition, and depth resources.
- [x] H01: correct internal gizmo depth without relying on axis draw order; capture the supplied overlap before/after.
- [ ] P1-01: implement idle, hover, pressed, dragging, snapped, precision, and cancelled styles.
- [ ] P1-03: render mode-specific translation/rotation/scale guides from feedback data.
- [ ] Verify dark, light, saturated, scene-occluded, and front/back orbit cases.

## Current work package

Status: `☑ H01 complete` (completed 2026-08-06)
Owner: Codex
Intended files: `EditorPrimitives.cs`, `TransformGizmoBase.Draw.cs`, `DirectionGizmo.cs`, the mesh/draw-call plumbing, `ForwardMaterialShader.h/.cpp`, and this implementation ledger.

Completed in this pass:

- traced the post-process composition and isolated gizmo depth target;
- confirmed the gizmo base material is transparent and disables depth testing;
- routed transform-gizmo meshes through an explicit depth prepass and a non-writing, depth-tested color pass;
- isolated that depth prepass from immediate debug drawing and kept intentionally layered feedback meshes out of it;
- removed the legacy X-axis positional offset used to manufacture draw order.
- aligned translate plane colors with scale and changed the direction widget to sort by signed view depth.

Runtime review completed on 2026-08-06. The supplied orbit views confirmed stable transform-gizmo self-depth in translate and scale modes, with nearer axis geometry covering farther geometry without fixed axis ordering. The direction widget also uses camera-depth ordering. Scene-depth comparison and occluded treatment remain deliberately deferred to the later pass described above.

## Completion evidence

H01 evidence: reviewer-supplied translate and scale orbit captures plus explicit runtime confirmation that depth looks correct on 2026-08-06. The implementation uses an isolated depth prepass followed by a non-writing depth-tested color pass, confirming the self-depth portion of `INV-08`. Scene-visible/scene-occluded treatment, `INV-09`, and the remaining section 16 visual matrix continue in their later work packages.

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
