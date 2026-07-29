#include <metal_stdlib>
#include "shared_types.h"

using namespace metal;

struct LineVarying {
    float4 position [[position]];
    float4 color;
};

vertex LineVarying debug_line_vertex(
    uint vid [[vertex_id]],
    device const LineVertex *vertices [[buffer(0)]],
    constant LineUniforms &line_uniforms [[buffer(1)]])
{
    LineVarying out;
    out.position = line_uniforms.view_proj * float4(vertices[vid].pos.xyz, 1.0);
    out.color = vertices[vid].color;
    return out;
}

fragment float4 debug_line_fragment(LineVarying in [[stage_in]])
{
    return in.color;
}
