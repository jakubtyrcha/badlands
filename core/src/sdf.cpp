#include "sdf.h"

#include <cassert>
#include <cmath>

#include "scene.h"

namespace sq {

// sq::Shape (the interop enum) and SDF_SHAPE_* (sdf_scene.h's MSL-visible
// copy) are two spellings of one numbering, and the shader reads the second
// out of pos_shape.w. This is where they are welded together: the packing
// below casts straight from one to the other, so a mismatch would repaint
// every scene silently rather than failing to build.
static_assert(static_cast<int32_t>(Shape::Cube) == SDF_SHAPE_CUBE, "shape id mismatch: Cube");
static_assert(static_cast<int32_t>(Shape::Sphere) == SDF_SHAPE_SPHERE, "shape id mismatch: Sphere");
static_assert(static_cast<int32_t>(Shape::Cone) == SDF_SHAPE_CONE, "shape id mismatch: Cone");
static_assert(static_cast<int32_t>(Shape::Capsule) == SDF_SHAPE_CAPSULE, "shape id mismatch: Capsule");
static_assert(static_cast<int32_t>(Shape::Octahedron) == SDF_SHAPE_OCTAHEDRON, "shape id mismatch: Octahedron");
static_assert(static_cast<int32_t>(Shape::Pyramid) == SDF_SHAPE_PYRAMID, "shape id mismatch: Pyramid");
static_assert(static_cast<int32_t>(Shape::Prism) == SDF_SHAPE_PRISM, "shape id mismatch: Prism");
static_assert(static_cast<int32_t>(Shape::Vesica) == SDF_SHAPE_VESICA, "shape id mismatch: Vesica");

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

SdfNode local_sdf_node(const Node& node) {
    const simd_float3 half = node.scale * 0.5f;
    SdfNode sn;
    sn.pos_shape = sdf_make4(0.0f, 0.0f, 0.0f,
                             static_cast<float>(static_cast<int32_t>(node.shape)));
    sn.half_extents_op = sdf_make4(half.x, half.y, half.z, 0.0f); // op is not read by sdf_eval_node
    sn.inv_rotation = sdf_make4(0.0f, 0.0f, 0.0f, 1.0f);          // identity
    sn.params = sdf_make4(node.shape_param, 0.0f, 0.0f, 0.0f);
    return sn;
}

void pack_scene(const SceneDocument& doc, std::vector<SdfNode>& out) {
    out.clear();
    const std::vector<Node>& nodes = doc.nodes();
    const size_t count = nodes.size();

    out.reserve(count); // no-op once out's capacity already covers count
    for (size_t i = 0; i < count; ++i) {
        const Node& node = nodes[i];
        const simd_float3 half = node.scale * 0.5f;
        // Conjugate, not a general inverse: that identity holds only for a
        // UNIT quaternion, which Node::rotation is expected to be. The drag
        // that writes it normalizes on every update (editor.cpp), so the
        // precondition is maintained at the one place rotation is produced
        // rather than re-established here per node per frame.
        const simd_float4 inv = simd_conjugate(node.rotation).vector;
        SdfNode sn;
        sn.pos_shape = sdf_make4(node.position.x, node.position.y, node.position.z,
                                  static_cast<float>(static_cast<int32_t>(node.shape)));
        sn.half_extents_op =
            sdf_make4(half.x, half.y, half.z, (node.op == Op::Add) ? 0.0f : 1.0f);
        sn.inv_rotation = sdf_make4(inv.x, inv.y, inv.z, inv.w);
        sn.params = sdf_make4(node.shape_param, 0.0f, 0.0f, 0.0f);
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

// Scene AABB = union over nodes of position +/- scale*0.5. A correct bound for
// every shape: each one is inscribed in its own half-extent box (exactly so for
// the cube, loosely for the rest), which is what the cross-section contraction
// in sdf_eval_node guarantees. Rotation is NOT accounted for -- a pre-existing
// looseness, and the reason this is only used by the dormant DCSDD sampler.
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
