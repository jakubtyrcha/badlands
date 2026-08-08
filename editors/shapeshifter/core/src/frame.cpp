#include "frame.h"

#include <cmath>

namespace sq {

namespace {

// Below this a length is treated as degenerate. Dividing by it would turn one
// bad matrix into a NaN that propagates silently into every descendant's frame,
// which is the failure mode this whole file is shaped to avoid.
constexpr float kMinLength = 1e-6f;

simd_float3 xyz(simd_float4 v) { return simd_make_float3(v.x, v.y, v.z); }

} // namespace

Frame compose(const Frame& parent, const Frame& local) {
    Frame out;
    // Normalized here rather than trusted from the inputs: pack_scene packs the
    // CONJUGATE as the inverse, and that identity holds only for a unit
    // quaternion. A chain of products drifts, so the chain's one composition
    // point is where the drift is stopped.
    out.rotation = simd_normalize(simd_mul(parent.rotation, local.rotation));
    // The parent's scale reaches the child's OFFSET, not merely its size --
    // scaling an assembly has to move its parts apart, or it only inflates them
    // where they stand.
    out.position =
        parent.position + simd_act(parent.rotation, parent.uniform_scale * local.position);
    out.uniform_scale = parent.uniform_scale * local.uniform_scale;
    return out;
}

simd_float3 transform_point(const Frame& f, simd_float3 p) {
    return f.position + simd_act(f.rotation, f.uniform_scale * p);
}

simd_float3 transform_direction(const Frame& f, simd_float3 d) {
    return simd_act(f.rotation, d);
}

simd_float3 inverse_transform_point(const Frame& f, simd_float3 p) {
    const float scale =
        (f.uniform_scale > kMinLength && std::isfinite(f.uniform_scale)) ? f.uniform_scale : 1.0f;
    return simd_act(simd_inverse(f.rotation), p - f.position) / scale;
}

simd_float3 inverse_transform_direction(const Frame& f, simd_float3 d) {
    return simd_act(simd_inverse(f.rotation), d);
}

Frame relative_to(const Frame& parent, const Frame& world) {
    // simd_inverse rather than simd_conjugate: the two agree only for a unit
    // quaternion, and this is a public entry point that cannot assume its
    // caller went through compose.
    const simd_quatf inv = simd_inverse(parent.rotation);
    // A zero or negative parent scale is degenerate and would divide the
    // position away. Treated as 1, so the result is merely wrong rather than
    // infinite -- and a scale that small cannot be reached through the UI.
    const float scale =
        (parent.uniform_scale > kMinLength && std::isfinite(parent.uniform_scale))
            ? parent.uniform_scale
            : 1.0f;

    Frame out;
    out.rotation = simd_normalize(simd_mul(inv, world.rotation));
    out.position = simd_act(inv, world.position - parent.position) / scale;
    out.uniform_scale = world.uniform_scale / scale;
    return out;
}

Frame frame_from_matrix(const simd_float4x4& m) {
    Frame out;
    out.position = xyz(m.columns[3]);

    const simd_float3 c0 = xyz(m.columns[0]);
    const simd_float3 c1 = xyz(m.columns[1]);
    const simd_float3 c2 = xyz(m.columns[2]);

    // One scalar for three axes: the mean of the basis lengths. Lossy, and
    // deliberately so -- a Frame has nowhere to put the other two, and neither
    // does SdfNode.
    const float len0 = simd_length(c0);
    const float len1 = simd_length(c1);
    const float len2 = simd_length(c2);
    const float mean = (len0 + len1 + len2) / 3.0f;
    if (!(mean > kMinLength) || !std::isfinite(mean)) {
        return out; // collapsed basis: identity rotation, unit scale, position kept
    }
    out.uniform_scale = mean;

    // Gram-Schmidt. What survives is a rotation by construction, which is the
    // whole point: shear enters here and does not leave.
    if (!(len0 > kMinLength)) {
        return out;
    }
    const simd_float3 u = c0 / len0;
    simd_float3 v = c1 - simd_dot(c1, u) * u;
    const float len_v = simd_length(v);
    if (!(len_v > kMinLength)) {
        return out; // c1 parallel to c0: no second axis to recover
    }
    v /= len_v;
    // NOT flipped to agree with c2 when the source basis is left-handed. A
    // mirror is no more representable here than a shear is -- a unit quaternion
    // and a positive scale cannot spell one -- so it is discarded by the same
    // rule, leaving a right-handed frame rather than a quaternion built from a
    // left-handed basis, which would be meaningless.
    const simd_float3 n = simd_cross(u, v);

    out.rotation = simd_normalize(simd_quaternion(simd_matrix(u, v, n)));
    if (!std::isfinite(simd_length(out.rotation.vector))) {
        out.rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);
    }
    return out;
}

} // namespace sq
