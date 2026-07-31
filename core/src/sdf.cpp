#include "sdf.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "scene.h"

namespace sq {

// Both thin wrappers over sdf_scene.h's dual-compile math (sq_float3 ==
// simd_float3 in plain C++ builds, so no conversion is needed) -- kept as
// free functions here because sdf_tests.cpp pins them directly by this exact
// name/signature (the refactor guard: those expectations are unchanged).
float sd_box(simd_float3 q, simd_float3 half_extents) {
    return sdf_sd_box(q, half_extents);
}

float sd_ellipsoid(simd_float3 q, simd_float3 radii) {
    return sdf_sd_ellipsoid(q, radii);
}

void pack_scene(const SceneDocument& doc, std::vector<SdfNode>& out) {
    out.clear();
    const std::vector<Node>& nodes = doc.nodes();
    const size_t count = std::min(nodes.size(), static_cast<size_t>(kMaxRaymarchNodes));

    out.reserve(count); // no-op once out's capacity already covers count
    for (size_t i = 0; i < count; ++i) {
        const Node& node = nodes[i];
        const simd_float3 half = node.scale * 0.5f;
        SdfNode sn;
        sn.pos_shape = sdf_make4(node.position.x, node.position.y, node.position.z,
                                  (node.shape == Shape::Cube) ? 0.0f : 1.0f);
        sn.half_extents_op =
            sdf_make4(half.x, half.y, half.z, (node.op == Op::Add) ? 0.0f : 1.0f);
        out.push_back(sn);
    }
}

std::vector<SdfNode> pack_scene(const SceneDocument& doc) {
    std::vector<SdfNode> packed;
    pack_scene(doc, packed);
    return packed;
}

std::optional<float> evaluate_scene_sdf(const SceneDocument& doc, simd_float3 p) {
    if (doc.nodes().empty()) {
        return std::nullopt;
    }
    // Single-point convenience query: packs once, folds once. sample_scene
    // below is the perf-sensitive path (N^3 fold evaluations) and packs
    // once itself rather than calling this function per sample point.
    const std::vector<SdfNode> packed = pack_scene(doc);
    return sdf_fold(packed.data(), static_cast<int>(packed.size()), p);
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

    // Pack once, fold N^3 times: evaluate_scene_sdf would re-pack (allocate)
    // per sample point, which is the exact per-call cost this loop cannot
    // afford at n=64 (262144 samples).
    const std::vector<SdfNode> packed = pack_scene(doc);
    const int node_count = static_cast<int>(packed.size());

    for (int32_t z = 0; z < n; ++z) {
        for (int32_t y = 0; y < n; ++y) {
            for (int32_t x = 0; x < n; ++x) {
                const simd_float3 offset =
                    simd_float3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)} * grid.spacing;
                const simd_float3 p = grid.origin + offset;
                const size_t un = static_cast<size_t>(n);
                const size_t index = static_cast<size_t>(x) + un * (static_cast<size_t>(y) + un * static_cast<size_t>(z));
                grid.values[index] = sdf_fold(packed.data(), node_count, p);
            }
        }
    }
    return grid;
}

} // namespace sq
