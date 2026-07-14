// Deferred lighting pass — sun directional (EON diffuse + GGX specular) +
// spherical-harmonic ambient diffuse, evaluated in World-Camera-Offseted Space.
//
// Adapted from sampo's src/shaders/passes/deferred_lighting.wesl for badlands
// task S2.B1. Kept: world-pos reconstruction from reversed-Z depth, octahedron
// normal decode, the sun directional term (EON retro diffuse + GGX spec), and
// the SH ambient diffuse term. REMOVED (return in later B tasks): shadow-map
// PCF (binding 5/6), GTAO (7), contact shadows (8) — hardcoded shadow=1/ao=1;
// and IBL specular (prefiltered env 9/10 + BRDF LUT 11/12). B1's group-0
// layout is therefore: frame UBO @0, gbuffer depth @1, normals @2, albedo @3,
// material @4 (all textureLoad'd — no sampler needed).
import package::common::frame::{frame, getFullscreenTriangleVertex, reconstructLinearZ, reconstructViewSpacePosition, viewSpaceToWorldOffsetedSpacePosition, decodeOctahedron};
import package::common::color::srgb_to_linear;
import package::common::sh_lighting::evaluateAmbientSHL2;
import package::common::brdf::{evaluateEonRetroBrdf, evaluateSpecularGgxBrdf};

const F0_DIELECTRIC: vec3<f32> = vec3<f32>(0.04);

@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var normals_texture: texture_2d<f32>;
@group(0) @binding(3) var albedo_texture: texture_2d<f32>;
@group(0) @binding(4) var material_texture: texture_2d<f32>;  // RGBA8: R=roughness, G=shadow

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
    @location(0) screenUV: vec2<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) VertexIndex: u32) -> VertexOutput {
    let fullscreen = getFullscreenTriangleVertex(VertexIndex);
    var output: VertexOutput;
    output.Position = fullscreen.clipPosition;
    output.screenUV = fullscreen.screenUV;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texCoord = vec2<i32>(input.Position.xy);
    let depth = textureLoad(depth_texture, texCoord, 0);
    let linearZ = reconstructLinearZ(depth);

    // Sky / no-geometry: leave the pass's clear color untouched.
    if (linearZ >= frame.farPlane * 0.999) {
        discard;
    }

    // 1. Reconstruct Position in World-Camera-Offseted Space (camera at origin)
    let posInViewSpace = reconstructViewSpacePosition(input.screenUV, depth);
    let posOffseted = viewSpaceToWorldOffsetedSpacePosition(posInViewSpace);

    // 2. Prepare vectors (all in Offseted Space)
    let normalOct = textureLoad(normals_texture, texCoord, 0).xy;
    let normal = decodeOctahedron(normalOct); // directions are space-invariant

    let lightDir = normalize(frame.sunDirection.xyz);
    // In Offseted Space, the camera is at (0,0,0)
    let viewDir = normalize(-posOffseted);
    let NdotV = max(dot(normal, viewDir), 0.0);

    // 3. Material properties from GBuffer (R=roughness)
    let materialData = textureLoad(material_texture, texCoord, 0);
    let roughness = materialData.r;

    // 4. Shadowing / occlusion — no shadow map, contact shadows or GTAO in B1
    let shadow = 1.0;
    let ao = 1.0;

    // 5. Lighting calculation
    let albedoLinear = srgb_to_linear(textureLoad(albedo_texture, texCoord, 0).rgb);
    let NdotL = max(dot(normal, lightDir), 0.0);

    // Direct lighting (sun): EON diffuse (includes NdotL internally) + GGX spec
    let diffuseTerm = evaluateEonRetroBrdf(normal, lightDir, viewDir, roughness);
    let specularTerm = evaluateSpecularGgxBrdf(normal, viewDir, lightDir, roughness, F0_DIELECTRIC);
    let directLighting = (diffuseTerm * albedoLinear + specularTerm) * shadow * NdotL * frame.sunColor.xyz;

    // Ambient lighting (SH L2 diffuse only — IBL specular returns in B2)
    let ambientDiffuse = evaluateAmbientSHL2(normal, frame.ambientSH) * ao;
    let indirectLighting = ambientDiffuse * albedoLinear;

    let finalColorLinear = directLighting + indirectLighting;
    return vec4<f32>(finalColorLinear, 1.0);
}
