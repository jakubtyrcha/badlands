#pragma once

// Test-only conveniences for the suites that describe ONE free node.
//
// A Node no longer determines its own world placement -- that is the document's
// answer to give (SceneDocument::placement) -- so the production signatures all
// take a resolved NodePlacement. The suites below predate that and are about
// geometry rather than about hierarchy: "a unit cube at the origin is hit from
// +X", "a sphere's bounding radius is its longest semi-axis". Restating a
// placement at each of their ~40 call sites would bury what they actually pin.
//
// So these overloads say the missing half once: place the node where its own
// fields say. That IS what a world-rooted node resolves to, which is why these
// expectations survive the storage becoming parent-local.
//
// RESOLVED THROUGH THE REAL RESOLVER, deliberately. A hand-written NodePlacement
// here would be a second implementation of resolution, and it would drift the
// moment SceneDocument's storage changed -- leaving these tests green while the
// thing they describe was broken.

#include <optional>
#include <vector>

#include "frame.h"
#include "gizmo.h"
#include "lines.h"
#include "navigation.h"
#include "picking.h"
#include "scene.h"
#include "sdf.h"

namespace sq {

inline NodePlacement placement_of(Node node) {
    SceneDocument doc;
    if (node.id == kInvalidNode) {
        node.id = 1; // find() keys on id, and the miss sentinel is not a key
    }
    doc.add(node);
    return doc.placement(node.id);
}

inline std::optional<RayHit> raycast_node(const Node& node, const Ray& world) {
    return raycast_node(node, placement_of(node), world);
}

inline SdfNode local_sdf_node(const Node& node) {
    return local_sdf_node(node, placement_of(node).half_extents);
}

inline float node_bounding_radius(const Node& node) {
    return node_bounding_radius(node, placement_of(node));
}

inline GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera, GizmoSlot slot) {
    return gizmo_frame_for_node(placement_of(node), camera, slot);
}

inline void append_node_wireframe(std::vector<LineVertex>& out, const Node& node,
                                  simd_float4 color, simd_float3 eye_world) {
    append_node_wireframe(out, node, placement_of(node), color, eye_world);
}

} // namespace sq
