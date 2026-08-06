#include <metal_stdlib>
#include "shared_types.h"

using namespace metal;

struct MeshVarying {
    float4 position [[position]];
    float4 normal;
};

vertex MeshVarying mesh_vertex(
    uint vid [[vertex_id]],
    device const MeshVertex *vertices [[buffer(0)]],
    constant LineUniforms &line_uniforms [[buffer(1)]])
{
    MeshVarying out;
    out.position = line_uniforms.view_proj * float4(vertices[vid].pos.xyz, 1.0);
    out.normal = vertices[vid].normal;
    return out;
}

// Normal-colored debug shading: facet normals are constant per triangle, so
// the interpolated `in.normal` is already unit length across the whole
// triangle -- the normalize() here is cheap safety, not a real need.
fragment float4 mesh_fragment(MeshVarying in [[stage_in]])
{
    float3 n = normalize(in.normal.xyz);
    return float4(0.5 * (n + 1.0), 1.0);
}
