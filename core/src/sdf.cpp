#include "sdf.h"

#include <cassert>
#include <cfloat>
#include <cmath>

#include "scene.h"

namespace sq {

float sd_box(simd_float3 q, simd_float3 half_extents) {
    const simd_float3 d = simd_abs(q) - half_extents;
    const float outside = simd_length(simd_max(d, simd_float3{0.0f, 0.0f, 0.0f}));
    const float inside = std::fmin(simd_reduce_max(d), 0.0f);
    return outside + inside;
}

float sd_ellipsoid(simd_float3 q, simd_float3 radii) {
    const float k0 = simd_length(q / radii);
    const float k1 = simd_length(q / (radii * radii));
    if (k1 == 0.0f) {
        // q == 0: the approximation divides by k1, which is singular at the
        // center. The nearest surface point from the center lies along the
        // smallest radius, so the exact distance there is -min_component(radii).
        return -simd_reduce_min(radii);
    }
    return k0 * (k0 - 1.0f) / k1;
}

namespace {

// Evaluates one node's local SDF at a world-space point p.
//
// Node rotation is always identity in the current MVP (see scene.h), so we
// evaluate directly in the node's translation-only frame: q = p - node.position.
// If/when rotation becomes editable, q must be rotated by the inverse rotation
// before this dispatch (and before the box/ellipsoid math above, which assumes
// q already comes in axis-aligned with the shape).
float sd_node(const Node& node, simd_float3 p) {
    const simd_float3 q = p - node.position;
    const simd_float3 half = node.scale * 0.5f;
    return (node.shape == Shape::Cube) ? sd_box(q, half) : sd_ellipsoid(q, half);
}

} // namespace

std::optional<float> evaluate_scene_sdf(const SceneDocument& doc, simd_float3 p) {
    if (doc.nodes().empty()) {
        return std::nullopt;
    }

    float d = FLT_MAX;
    for (const Node& node : doc.nodes()) {
        const float d_node = sd_node(node, p);
        // Known approximation: min/max CSG combine is not a true distance
        // field near intersection curves (only the zero-set/surface is
        // exact). The DCSDD paper (Table 1) shows the reconstruction
        // tolerates comparable noise, so this is accepted rather than
        // worked around.
        d = (node.op == Op::Add) ? std::fmin(d, d_node) : std::fmax(d, -d_node);
    }
    return d;
}

namespace {

struct Aabb {
    simd_float3 min;
    simd_float3 max;
};

// Scene AABB = union over nodes of position +/- scale*0.5. Correct bound for
// both shapes: exact for the box, and a (loose but correct) bound for the
// ellipsoid, whose extent along each axis never exceeds its radius on that axis.
Aabb scene_aabb(const SceneDocument& doc) {
    const std::vector<Node>& nodes = doc.nodes();
    Aabb box{nodes[0].position - nodes[0].scale * 0.5f, nodes[0].position + nodes[0].scale * 0.5f};
    for (size_t i = 1; i < nodes.size(); ++i) {
        const simd_float3 half = nodes[i].scale * 0.5f;
        box.min = simd_min(box.min, nodes[i].position - half);
        box.max = simd_max(box.max, nodes[i].position + half);
    }
    return box;
}

} // namespace

SampleGrid sample_scene(const SceneDocument& doc, int32_t n) {
    if (doc.nodes().empty()) {
        return SampleGrid{};
    }
    assert(n >= 2 && "sample_scene: n must be >= 2 for a non-empty scene");

    const Aabb box = scene_aabb(doc);
    const simd_float3 extent = box.max - box.min;
    // Inflate to a cube around the AABB center: 10% margin, with a 0.1 floor
    // guarding degenerate/near-point scenes (e.g. a single node with zero scale).
    const float side = std::fmax(simd_reduce_max(extent), 0.1f) * 1.1f;
    const simd_float3 center = (box.min + box.max) * 0.5f;

    SampleGrid grid;
    grid.origin = center - simd_float3{side, side, side} * 0.5f;
    grid.spacing = side / static_cast<float>(n - 1);
    grid.n = n;
    grid.values.resize(static_cast<size_t>(n) * static_cast<size_t>(n) * static_cast<size_t>(n));

    for (int32_t z = 0; z < n; ++z) {
        for (int32_t y = 0; y < n; ++y) {
            for (int32_t x = 0; x < n; ++x) {
                const simd_float3 offset =
                    simd_float3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)} * grid.spacing;
                const simd_float3 p = grid.origin + offset;
                const size_t un = static_cast<size_t>(n);
                const size_t index = static_cast<size_t>(x) + un * (static_cast<size_t>(y) + un * static_cast<size_t>(z));
                grid.values[index] = evaluate_scene_sdf(doc, p).value(); // non-empty scene: always has a value
            }
        }
    }
    return grid;
}

} // namespace sq
