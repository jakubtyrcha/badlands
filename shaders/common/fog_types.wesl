// Shared volumetric-fog uniform layout (Task: fog rendering). Struct only, no
// bindings — imported by fog_cascade.wesl (raymarch) and fog_composite.wesl so
// both agree on the layout without one pulling in the other's texture bindings.
// Mirrors the C++ FogUniformsGpu in volumetric_fog.cpp (64 bytes).
struct FogUniforms {
    fogMaxDistance: f32,
    phaseG: f32,
    floorY: f32,
    height: f32,
    baseHalfExtent: f32,
    ambientScale: f32,
    sunScale: f32,
    stepCount: f32,
    resY: f32,
    cascadeCount: f32,
    enableShafts: f32,
    jitterEnabled: f32,
    fogRenderW: f32,   // fog raymarch target size (may be half the screen)
    fogRenderH: f32,
    frameIndex: f32,   // animates the spatial jitter
    pad0: f32,
}
