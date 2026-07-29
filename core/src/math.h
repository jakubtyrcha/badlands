#pragma once
#include <simd/simd.h>

// Small math helpers shared across core/. Keep this header-only and minimal —
// anything with real logic belongs in its own .h/.cpp.

namespace sq {

// TRS composition: translate ∘ rotate ∘ scale (i.e. world_from_local applies
// scale first, then rotation, then translation).
inline simd_float4x4 trs_matrix(simd_float3 position, simd_quatf rotation, simd_float3 scale) {
    simd_float4x4 t = matrix_identity_float4x4;
    t.columns[3] = (simd_float4){position.x, position.y, position.z, 1.0f};

    simd_float4x4 r = simd_matrix4x4(rotation);

    simd_float4x4 s = matrix_identity_float4x4;
    s.columns[0].x = scale.x;
    s.columns[1].y = scale.y;
    s.columns[2].z = scale.z;

    return simd_mul(t, simd_mul(r, s));
}

} // namespace sq
