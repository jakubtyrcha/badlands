// Wireframe, gizmo and marker geometry: world-space vertices carrying their
// own colour, projected and drawn as lines.
//
// Ported from shaders/debug_lines.metal. The MSL was ALREADY pulling vertices
// from a buffer by vertex_id rather than declaring a vertex layout, so this is
// a rename rather than a restructure -- the same reason the RHI's lack of
// vertex-input state costs this pass nothing.
//
// LineVertex and LineUniforms come from shared_types.h, the header the host
// packs against. Not mirrored here: a mirrored struct agrees until someone
// edits one side.

#define SDF_SLANG 1
#include "shared_types.h"

StructuredBuffer<LineVertex> vertices;
ConstantBuffer<LineUniforms> line_uniforms;

struct LineVarying {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
LineVarying vs_main(uint vid : SV_VertexID) {
    LineVarying out;
    out.position = mul(line_uniforms.view_proj, float4(vertices[vid].pos.xyz, 1.0));
    out.color = vertices[vid].color;
    return out;
}

[shader("fragment")]
float4 fs_main(LineVarying in) : SV_Target {
    return in.color;
}
