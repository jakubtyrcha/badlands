// Color space conversion utilities — IEC 61966-2-1 piecewise sRGB transfer
// Handles HDR values > 1.0 correctly (extends the power curve)

fn srgb_to_linear_component(c: f32) -> f32 {
    if (c <= 0.04045) { return c / 12.92; }
    return pow((c + 0.055) / 1.055, 2.4);
}

fn srgb_to_linear(c: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(
        srgb_to_linear_component(c.x),
        srgb_to_linear_component(c.y),
        srgb_to_linear_component(c.z)
    );
}

fn linear_to_srgb_component(c: f32) -> f32 {
    if (c <= 0.0031308) { return c * 12.92; }
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

fn linear_to_srgb(c: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(
        linear_to_srgb_component(c.x),
        linear_to_srgb_component(c.y),
        linear_to_srgb_component(c.z)
    );
}
