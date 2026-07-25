// Phase A indirect-draw primitive (GPU-driven instanced rendering, primitives
// spike). Trivial self-contained pipeline (no bind groups, no camera/frame
// import) so the test isolates ONE thing: does DrawIndexedIndirect honor the
// instanceCount the GPU reads out of an indirect-args buffer, not whatever the
// CPU call site happens to pass. VertexLayout::kPos2d (pos(vec2) only) —
// vertices are already in clip space (z=0, w=1).
struct VertexInput {
    @location(0) pos: vec2<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.position = vec4<f32>(input.pos, 0.0, 1.0);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
