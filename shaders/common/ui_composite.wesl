// UI-overlay composite helpers, shared by the resolve (tonemapping.wesl) and
// the G-buffer debug view (gbuffer_debug.wesl). The overlay holds the frame's
// 2D UI, premultiplied and sRGB-encoded (see SceneContext::ui_overlay); the
// consumer composites it over its output in ENCODED (gamma) space — the space
// UI blending is authored for — then converts onward as needed. Each consumer
// declares its own overlay binding (slots differ); these are pure functions.

// Clamped load: the fallback overlay is a 1x1 transparent texel, so clamp the
// pixel coordinate into the overlay's bounds instead of relying on
// out-of-bounds textureLoad behavior.
fn loadUiOverlay(tex: texture_2d<f32>, coord: vec2<i32>) -> vec4<f32> {
    let dims = vec2<i32>(textureDimensions(tex));
    return textureLoad(tex, clamp(coord, vec2<i32>(0), dims - 1), 0);
}

// Premultiplied "over" in encoded space: what 8-bit hardware blending did
// when the UI drew straight onto the surface.
fn compositeUiOverEncoded(scene_encoded: vec3<f32>, ui: vec4<f32>) -> vec3<f32> {
    return ui.rgb + scene_encoded * (1.0 - ui.a);
}
