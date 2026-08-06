// Thick antialiased debug lines.
//
// Ported from shaders/geometry/thick_line.wesl. The CPU expands each world-space
// segment into a screen-aligned quad (2 triangles, 6 vertices) carrying a signed
// perpendicular distance and a half-thickness; the vertex stage only projects,
// and the fragment stage applies a 1px conical fringe. That split is why the
// expansion (ExpandDebugLines) is a pure, unit-tested CPU function with no GPU
// dependency at all -- and why this port reuses it untouched.
//
// WHAT CHANGED FROM THE WESL, and why:
//
//   * No vertex input layout. The RHI has no vertex buffers by design, so the
//     8 floats per vertex are PULLED from a storage buffer by SV_VertexID.
//     Same bytes, same order, different way in.
//   * No srgb_to_linear on the colour. The WESL path rendered into a linear
//     HDR target and tonemapped later; this one writes straight to an 8-bit
//     surface, so converting would darken every line by the gamma curve.

module thick_line;

public struct LineFrame {
    public float4x4 view_proj;   // camera-offset frame matrix
    // xyz = world camera position, w unused. The CPU rebased the quad corners
    // by -camera, so the shader applies the single camera offset here.
    public float4 camera_pos;
}

ConstantBuffer<LineFrame> frame;

// 8 floats per vertex: pos(3) + color(3) + signed distance + half thickness.
// A struct rather than a raw float array so a stride change is a compile error
// here rather than sheared geometry at runtime.
public struct LineVertex {
    public float3 pos;
    public float3 color;
    public float dist;
    public float half_thickness;
}
StructuredBuffer<LineVertex> vertices;

struct VOut {
    float4 pos : SV_Position;
    float3 color : COLOR0;
    float dist : TEXCOORD0;
    float half_thickness : TEXCOORD1;
}

[shader("vertex")]
VOut vs_lines(uint vid : SV_VertexID) {
    LineVertex v = vertices[vid];
    VOut o;
    o.pos = mul(frame.view_proj, float4(v.pos - frame.camera_pos.xyz, 1.0));
    o.color = v.color;
    o.dist = v.dist;
    o.half_thickness = v.half_thickness;
    return o;
}

[shader("fragment")]
float4 fs_lines(VOut i) : SV_Target {
    float d = abs(i.dist);
    float half_w = i.half_thickness;
    // A 1-pixel conical fringe. Beyond half_w + 1 the quad contributes nothing,
    // which is what makes the quad's corners invisible rather than square.
    float alpha = 1.0 - smoothstep(half_w, half_w + 1.0, d);
    if (alpha < 0.01) { discard; }
    return float4(i.color, alpha);
}
