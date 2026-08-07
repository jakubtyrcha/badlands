// The reconstructed DCSDD mesh, shaded by its own normals.
//
// Ported from shaders/mesh.metal, which was already pulling MeshVertex from a
// buffer by vertex_id -- so, like debug_lines, this is a rename rather than a
// restructure.
//
// The pass is dormant: DCSDD reconstruction is implemented and tested but not
// triggered (see the editor README). It is ported anyway because leaving one
// pass in MSL would keep the whole metal-cpp shader path alive for it.

#define SDF_SLANG 1
#include "shared_types.h"

import output_transform;  // EncodeOutput: the one place a colour reaches the surface

StructuredBuffer<MeshVertex> vertices;
ConstantBuffer<LineUniforms> line_uniforms;

struct MeshVarying {
    float4 position : SV_Position;
    float4 normal : NORMAL0;
};

[shader("vertex")]
MeshVarying vs_main(uint vid : SV_VertexID) {
    MeshVarying out;
    out.position = mul(line_uniforms.view_proj, float4(vertices[vid].pos.xyz, 1.0));
    out.normal = vertices[vid].normal;
    return out;
}

// Normal-coloured debug shading. Facet normals are constant per triangle, so
// the interpolated normal is already unit length across the whole triangle --
// the normalize() is cheap safety, not a real need.
[shader("fragment")]
float4 fs_main(MeshVarying in) : SV_Target {
    const float3 n = normalize(in.normal.xyz);
    return EncodeOutput(0.5 * (n + 1.0), 1.0, kOutputExtendedLinearP3);
}
