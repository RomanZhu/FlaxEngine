# Implementation: Picking and Projection Sizing

Parent documents: [`behavior specification`](Flax_3D_Transform_Gizmo_Redesign_Specification.md) · [`implementation index`](implementation.md)

This note decomposes P2-01 through P2-03. It owns constant-pixel sizing, screen-space semantic targets, candidate arbitration, hover hysteresis, and overlap cycling. It does not own the world-space transform solvers.

## Projection sizing

The gizmo is authored in pixels and converted to world scale from the active projection:

```text
Perspective:
worldUnitsPerPixel =
    2 * forwardDepth * tan(verticalFov / 2) / viewportHeightPx
gizmoWorldRadius = desiredRadiusPx * worldUnitsPerPixel

Orthographic:
worldUnitsPerPixel = orthographicViewHeight / viewportHeightPx
```

Use forward depth, not Euclidean camera distance. Reject pivots behind the camera and define near-camera clamping. Apply DPI exactly once through the viewport logical-to-physical conversion.

| Element | Visible default | Motor target |
| --- | --- | --- |
| Overall radius | 96 px | N/A |
| Axis shaft | 3 px plus outline | 16 px capsule |
| Arrowhead / cube cap | 14–16 px | 20 px envelope |
| Plane handle | 18 × 18 px | +6 px expansion |
| Center handle | 18 px | 24 px circle |
| Axis ring | 88 px radius, 4 px stroke | 16 px band |
| Screen ring | 95 px radius, 2.5 px stroke | 16 px band |

## Semantic target model

Render geometry and motor-target geometry are separate. Build projected semantic targets from the current gizmo basis and camera:

| Handle | Target primitive |
| --- | --- |
| Axis | Projected line segment plus cap polygon |
| Plane | Projected quadrilateral with expanded margin |
| Center | Circle or rounded rectangle |
| Axis ring | Projected ellipse/front arc |
| Screen ring | Screen-space circle |

Targets must not depend on rendered mesh triangle order or 3D collision volumes. Rebuild targets when the view/gizmo changes and latch the selected target for an active drag.

## Arbitration and hysteresis

1. Reject candidates outside their motor target.
2. A cursor at least 4 px inside a plane polygon gives the plane priority over edge axes.
3. A cursor inside a cap envelope gives the cap/axis priority over its shaft.
4. Otherwise choose the smallest normalized screen distance.
5. For projected overlap, prefer the candidate with the nearest cursor-local projected depth, consistent with rendered gizmo self-depth.
6. Retain the current hover until a new candidate improves the score by at least 20% or the cursor leaves an expanded retention zone.

A press latches the semantic handle until commit or cancel. Hidden handles are not selectable while another operation is active. Tab or mouse wheel MAY cycle exact-overlap candidates.

Hover treatment should appear in the same rendered frame; there is no artificial hover delay. Tooltips may be delayed by 500 ms. Cursor icons reflect the semantic operation.

## Implementation checklist

- [x] P2-01: implement perspective, orthographic, near-camera, behind-camera, and DPI-aware projection helpers.
- [x] P2-02: generate semantic targets independently from render mesh geometry.
- [x] P2-03: implement priority, normalized-distance arbitration, self-depth overlap, hysteresis, and candidate cycling.
- [x] P1-01: expose target state to the visual feedback layer without duplicating selection logic.

## Current implementation handoff

The first implementation pass is in `Source/Editor/Gizmo/TransformGizmoBase.Projection.cs` and is wired into the existing gizmo lifecycle:

- `UpdateMatrices` derives world scale from forward projection depth, orthographic view height, near-camera clamping, and one logical-to-physical DPI conversion.
- Cached semantic targets cover axis segments/caps, planes, center handles, the free-rotation trackball, and rotation arcs/rings. `Selection.cs` now acquires from these projected targets while active transactions keep the latched handle.
- Arbitration applies semantic priorities, normalized distance, cursor-local projected depth, hover retention, and Tab cycling. Axis and screen rings take precedence over the trackball within their motor bands.
- `TestTransformGizmoInteraction.TestProjectionSizingUsesForwardDepthAndDpiParity` covers forward-depth scaling, orthographic depth independence, near-camera clamping, behind-camera rejection, and DPI parity.

Runtime overlap, front/back, and hover-flicker checks remain open. The focused test and editor build were not run in this handoff because the repository instructions require explicit build authorization.

## Completion evidence

Run perspective/orthographic/DPI parity checks, pointer acquisition at overlap and near-edge positions, rotation-ring versus trackball acquisition, and front/back orbits. Record target flicker observations and confirm `INV-08` through `INV-10` in the [implementation index](implementation.md).

Return to the behavior source: [`Flax_3D_Transform_Gizmo_Redesign_Specification.md`](Flax_3D_Transform_Gizmo_Redesign_Specification.md).
