// Bakes one orthographic view of a tree into the impostor atlas' two maps
// (volumetric-foliage LOD4 -- see src/game/visual/impostor_baker.hpp).
//
// Two color targets, matching ImpostorAtlas:
//   0  rgb = albedo, a = coverage
//   1  rg  = octahedron-encoded TREE-LOCAL normal, b = DEPTH, a = THICKNESS
//
// `b` is the fragment's own normalized depth under the bake's orthographic
// projection -- 0 at the near plane (centre + viewDir * radius), 1 at the far
// one -- so it is free: it is exactly @builtin(position).z. It replaces the
// per-model translucency constant that used to sit here, which the material
// uniform already carries and so was redundant.
//
// `a` is left at 0 here and filled by the SEPARATE thickness pass
// (impostor_thickness.wesl), which cannot share this pipeline: it needs
// additive blending and no depth test, and blend state is per-pipeline rather
// than per-target.
//
// LOCAL normals, not world: the runtime rotates them by the instance's yaw
// before writing the G-buffer. A world-space bake would freeze every instance
// into the orientation the tree happened to be baked at.
//
// The crown is baked from the model's VOXEL L0 crown, not from its leaf cards.
// The field never draws cards -- its own LOD0 is the voxel crown -- so an
// impostor baked from cards would be a picture of a tree the chain never shows,
// and the L2 -> L4 switch would change the tree's appearance rather than just
// its cost.
//
// ONE pipeline for bark and crown rather than a feature flag, with two uniforms
// doing the work: `alpha_cutoff` (0 for both opaque cases) and
// `voxel_brightness` (which swaps albedo between a sampled texture and
// voxel_foliage's vertex-baked uv.x term). Two variants would be two things to
// keep in sync for a difference that is two floats wide.
//
// Depth here is CONVENTIONAL (near = 0, compare Less, clear 1.0), NOT the
// engine's reversed-Z. This pass shares no depth buffer with anything, feeds no
// shadow map, and is the only place in the codebase where that is true -- the
// ortho matrix is built by glm::ortho under GLM_FORCE_DEPTH_ZERO_TO_ONE and
// read straight back.

import package::common::gbuffer_encode::encodeOctahedron;

struct BakeUniforms {
    mvp: mat4x4<f32>,
    tint: vec4<f32>,    // rgb = albedo tint
    // x unused, y = alpha cutoff, z unused,
    // w = voxel-brightness mix: 0 samples the bound texture (bark), 1 takes
    //     albedo from the vertex-baked uv.x the leaf voxelizer writes, exactly
    //     as voxel_foliage.wesl does (albedo = vec3(uv.x) * tint).
    params: vec4<f32>,
    // xyz = this view's direction in TREE-LOCAL space, pointing from the tree
    // toward the eye. Used to face two-sided normals at the viewer.
    view_dir: vec4<f32>,
}

@group(0) @binding(0) var<uniform> u: BakeUniforms;
@group(0) @binding(1) var src_texture: texture_2d<f32>;
@group(0) @binding(2) var src_sampler: sampler;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,  // unused: no TBN, no normal map
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) normal: vec3<f32>,
}

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.position = u.mvp * vec4<f32>(in.pos, 1.0);
    out.uv = in.uv;
    // The bake is orthographic in the tree's own space, so the vertex normal
    // needs no transform at all -- it is already the local normal the atlas
    // stores.
    out.normal = in.normal;
    return out;
}

struct BakeOutput {
    @location(0) albedo: vec4<f32>,
    @location(1) surface: vec4<f32>,
}

@fragment
fn fs_main(in: VertexOutput) -> BakeOutput {
    let texel = textureSample(src_texture, src_sampler, in.uv);
    // Cutout. Bark passes a cutoff of 0 against an opaque white texture, so
    // this never fires for it.
    if (texel.a < u.params.y) {
        discard;
    }

    var out: BakeOutput;
    // mix() rather than a branch so bark and crown share one code path: at w=0
    // this is the texel, at w=1 it is the voxelizer's per-tet brightness.
    let base = mix(texel.rgb, vec3<f32>(in.uv.x), u.params.w);
    out.albedo = vec4<f32>(base * u.tint.rgb, 1.0);

    // Two-sided: a leaf card is a flat quad seen from either face, so when the
    // back face is the visible one its geometric normal points AWAY and would
    // light that part of the crown inside out.
    //
    // Flipped against the VIEW DIRECTION rather than @builtin(front_facing).
    // front_facing depends on winding and on the framebuffer's Y convention,
    // which is one indirection too many for a bake that has to be exactly
    // reproducible -- and it came out inverted when tried. The view direction
    // is known here and says what "toward the viewer" means with no convention
    // in between. For bark this is a no-op: its back faces lose the depth test.
    var n = normalize(in.normal);
    if (dot(n, u.view_dir.xyz) < 0.0) {
        n = -n;
    }
    let oct = encodeOctahedron(n);            // [-1, 1]
    out.surface = vec4<f32>(oct * 0.5 + 0.5,  // -> [0, 1] for RG8 storage
                            in.position.z,    // normalized depth, free
                            0.0);             // thickness, filled by pass 2
    return out;
}
