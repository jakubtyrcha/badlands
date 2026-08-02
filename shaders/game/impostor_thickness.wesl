// Accumulates the impostor's optical THICKNESS for one baked view
// (volumetric-foliage LOD4 -- see src/game/visual/impostor_baker.hpp).
//
// Additive depth accumulation, not a min/max envelope. Each fragment
// contributes its own depth with a sign taken from which side of the surface is
// facing the eye, so a CLOSED solid contributes (exit - enter) and a pile of
// them contributes the sum. The voxel crown is exactly a pile of closed tets,
// so this measures how much crown the ray actually passes THROUGH.
//
// An envelope (near/far depth difference) would have been simpler -- two depth
// tests, no blending -- but a conifer crown is mostly air, so its envelope
// hugely overstates the optical path and the transmission term would come out
// far too dark. This is the quantity evaluateTranslucency's
// exp(-sigma * thicknessWorld) actually wants.
//
// R16Float, because additive blending on a float target is only guaranteed in
// core WebGPU for 16-bit; float32 blending is behind an optional feature.
//
// No depth attachment and no depth test at all: every layer of the crown has to
// contribute, so an early-Z reject would silently truncate the sum to the
// front-most surface.

struct ThicknessUniforms {
    mvp: mat4x4<f32>,
}
@group(0) @binding(0) var<uniform> u: ThicknessUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> @builtin(position) vec4<f32> {
    return u.mvp * vec4<f32>(input.pos, 1.0);
}

// Sign from winding rather than from the normal: EmitTetMesh gives a tet ONE
// shared normal for all four of its faces (see leaf_voxelizer.hpp), so a
// normal-vs-view test cannot tell a tet's front face from its back one. A
// globally inverted winding would only flip the sum's sign, which the reader
// removes with abs().
@fragment
fn fs_main(@builtin(position) position: vec4<f32>,
           @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    return vec4<f32>(select(position.z, -position.z, front), 0.0, 0.0, 0.0);
}
