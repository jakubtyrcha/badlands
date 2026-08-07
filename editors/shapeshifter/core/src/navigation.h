#pragma once
#include <simd/simd.h>

#include "camera.h" // Camera::kFar, Ray
#include "frame.h"  // NodePlacement

namespace sq {

class SceneDocument;
struct Node;

// Which rung of resolve_focus's chain produced the point. Callers treat these
// differently: the focus-preview dot lights only on Scene, because a dot
// floating in space on a fallback is noise rather than information.
enum class FocusSource { Scene, Ground, TargetPlane };

struct FocusPoint {
    simd_float3 point;
    FocusSource source;
};

// A ground hit further than this is discarded. Rays near the horizon meet y=0
// at an enormous t — exactly-parallel ones at infinity — and a pivot placed
// out there would make the next orbit swing around a point effectively at
// infinity. Camera::kFar is the natural bound: past it, nothing is drawn.
inline constexpr float kFocusGroundMaxT = Camera::kFar;

// The world point under a cursor ray — the single primitive every camera
// gesture is built on, resolved ONCE per gesture (never per event) so that a
// gesture's frame of reference cannot shift under the user mid-drag.
//
// Chain, in order:
//   1. raycast_scene hit                          -> Scene
//   2. y = 0 intersection within kFocusGroundMaxT  -> Ground
//   3. the fallback target's depth, under the ray  -> TargetPlane
//   4. the fallback target itself                  -> TargetPlane
//
// NEVER returns a non-finite point: a non-finite candidate falls through to
// the next rung, and rung 4 is finite by construction. See is_finite3 (math.h)
// for why that matters more here than on the picking path this shares code
// with.
FocusPoint resolve_focus(const SceneDocument& doc, const Ray& ray, simd_float3 fallback_target);

// World-space bounding-sphere radius of a node, for frame-selection. Local
// geometry is the unit cube [-0.5,0.5]^3 or the radius-0.5 sphere (picking.h),
// so both come to half the scale per axis: the cube's bound is its half-
// diagonal, the ellipsoid's its longest semi-axis. A bounding SPHERE is
// rotation-invariant, so no basis is involved at any depth of parenting.
//
// The half-extents come from `placement` rather than from the node's own scale,
// because a node's world box is its own scale times whatever uniform scale it
// inherits -- and only the document can answer that.
float node_bounding_radius(const Node& node, const NodePlacement& placement);

// Orbit radius that frames a bounding sphere of `bound`: the vertical fit
// bound*kFrameMargin/sin(fov_y/2), floored at kMinFrameRadius.
//
// The floor is the load-bearing part. Without it, framing a nostril-scale node
// fills the screen with featureless surface and strands you with no landmarks
// — the opposite of what framing is for. A single floor achieves what scaling
// the padding by the node's share of the model would, and needs no notion of a
// parent the scene document does not have.
inline constexpr float kFrameMargin = 1.6f;
inline constexpr float kMinFrameRadius = 0.6f;
float frame_radius_for_bound(float bound, float fov_y_radians);

} // namespace sq
