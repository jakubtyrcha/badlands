// Textured mesh rendering shader
// Uses pos(3) + uv(2) + normal(3) + tangent(3) vertex layout
// For flat/unlit rendering, set ambient SH L0 to (1,1,1) and sun color to (0,0,0)
//
// Ported from sampo's src/shaders/material/textured_mesh.wesl. Deviation:
// sampo's direction-aware ambient (`package::common::sh_lighting::
// evaluateAmbientSHL2`) is replaced with a flat ambient constant (mirroring
// shaders/game/building.wesl's convention) — sh_lighting.wesl is a shared
// lighting utility module, not material-specific, and porting it (plus
// whatever it in turn pulls in) is out of scope for the material-system
// port (Task D1 only adds src/engine/rendering/material/** + material
// shaders). Restore full SH ambient evaluation once sh_lighting.wesl is
// ported alongside the rest of the shared lighting pipeline.

import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
import package::common::color::srgb_to_linear;

// Texture and sampler bindings
@group(0) @binding(1) var mesh_texture: texture_2d<f32>;
@group(0) @binding(2) var mesh_sampler: sampler;

// Per-object uniforms (model matrix transforms LocalSpace -> WorldCameraOffsetedSpace)
struct ObjectUniforms {
    modelMatrix: mat4x4<f32>,
}

@group(1) @binding(0) var<uniform> object: ObjectUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,  // Not used in this shader, but required by vertex layout
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) normal: vec3<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;

    // Transform local position to WorldCameraOffsetedSpace via model matrix
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);

    // Pass through UV and transform normal
    output.uv = input.uv;
    output.normal = normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);

    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    // Sample texture (assumed to be in sRGB)
    let texColor = textureSample(mesh_texture, mesh_sampler, input.uv);

    // Handle single-channel (R16Float) vs multi-channel (RGBA16Float) textures:
    // - R16Float: WebGPU samples as (R, 0, 0, 1) with alpha = 1.0
    // - RGBA16Float: We store alpha = 0.996 (distinguishing marker) in generation
    // If alpha is exactly 1.0 and G/B are 0, it's a single-channel grayscale texture.
    // If alpha < 1.0, it's a multi-channel color texture - use RGB as-is.
    var colorRgb = texColor.rgb;
    if (texColor.a > 0.999 && texColor.g < 0.001 && texColor.b < 0.001 && texColor.r > 0.001) {
        // R16Float single-channel texture - expand R to grayscale
        colorRgb = vec3<f32>(texColor.r, texColor.r, texColor.r);
    }

    // Convert texture color from sRGB to linear for lighting calculations
    let texColorLinear = srgb_to_linear(colorRgb);

    // Directional sun lighting
    let lightDir = normalize(frame.sunDirection.xyz);
    let NdotL = max(dot(normalize(input.normal), lightDir), 0.0);
    let diffuse = NdotL * frame.sunColor.xyz;

    // Flat ambient term (see file-level deviation note: full SH ambient
    // evaluation isn't ported yet).
    let ambient = vec3<f32>(0.12, 0.13, 0.16);

    // Compute final color in linear space
    let finalColorLinear = texColorLinear * (diffuse + ambient);

    return vec4<f32>(finalColorLinear, texColor.a);
}
