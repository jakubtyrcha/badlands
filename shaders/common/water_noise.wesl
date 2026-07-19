// Fluid surface detail for the water material (forward variant only). Analytic-
// derivative 2D gradient noise -> fBm (octaves, per-octave wind) -> domain warp,
// returning a tangent-space normal. Analytic derivatives throughout (no finite
// differences): each octave sums the noise gradient, and the domain warp applies
// its Jacobian (chain rule), so the normal is exact and cheap.
//
// Forward-only, WESL-only (no C++ mirror, not cross-checked) — this drives the
// specular/reflection ripple detail; the geometric base wave stays in
// water_waves. Deterministic on the GPU (integer hash, not sin-of-large-arg).

// 32-bit integer hash (Murmur-ish finalizer).
fn waterHash(x: u32) -> u32 {
    var h = x;
    h = h ^ (h >> 16u);
    h = h * 2246822519u;
    h = h ^ (h >> 13u);
    h = h * 3266489917u;
    h = h ^ (h >> 16u);
    return h;
}

// Unit gradient vector for integer cell `ip`.
fn waterCellGradient(ip: vec2<f32>) -> vec2<f32> {
    let i = vec2<i32>(ip);
    let h = waterHash(u32(i.x) * 374761393u + u32(i.y) * 668265263u);
    let a = f32(h) * (6.28318530718 / 4294967296.0);
    return vec2<f32>(cos(a), sin(a));
}

// IQ analytic-derivative gradient noise: returns (value, dv/dx, dv/dy). Value in
// ~[-1,1]; the .yz gradient is exact. https://iquilezles.org/articles/gradientnoise
fn waterGradNoise(p: vec2<f32>) -> vec3<f32> {
    let i = floor(p);
    let f = p - i;
    let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);         // quintic fade
    let du = 30.0 * f * f * (f * (f - 2.0) + 1.0);             // its derivative

    let ga = waterCellGradient(i + vec2<f32>(0.0, 0.0));
    let gb = waterCellGradient(i + vec2<f32>(1.0, 0.0));
    let gc = waterCellGradient(i + vec2<f32>(0.0, 1.0));
    let gd = waterCellGradient(i + vec2<f32>(1.0, 1.0));

    let va = dot(ga, f - vec2<f32>(0.0, 0.0));
    let vb = dot(gb, f - vec2<f32>(1.0, 0.0));
    let vc = dot(gc, f - vec2<f32>(0.0, 1.0));
    let vd = dot(gd, f - vec2<f32>(1.0, 1.0));

    let value = va + u.x * (vb - va) + u.y * (vc - va)
                + u.x * u.y * (va - vb - vc + vd);
    let deriv = ga + u.x * (gb - ga) + u.y * (gc - ga)
                + u.x * u.y * (ga - gb - gc + gd)
                + du * vec2<f32>((vb - va) + u.y * (va - vb - vc + vd),
                                 (vc - va) + u.x * (va - vb - vc + vd));
    return vec3<f32>(value, deriv.x, deriv.y);
}

// fBm: 4 octaves summing value AND analytic gradient. Each octave pans along a
// rotated wind vector (fakes dispersion) and doubles frequency / halves amplitude.
fn waterFbm(p: vec2<f32>, t: f32) -> vec3<f32> {
    var amp = 0.5;
    var freq = 1.0;
    var sum = 0.0;
    var grad = vec2<f32>(0.0);
    var wind = vec2<f32>(0.11, 0.05);
    const rc = 0.766;  // ~40deg rotation per octave
    const rs = 0.643;
    for (var o = 0u; o < 4u; o = o + 1u) {
        let n = waterGradNoise(p * freq + wind * t);
        sum = sum + amp * n.x;
        grad = grad + amp * freq * vec2<f32>(n.y, n.z);  // chain rule for freq scaling
        amp = amp * 0.5;
        freq = freq * 2.0;
        wind = vec2<f32>(rc * wind.x - rs * wind.y, rs * wind.x + rc * wind.y) * 1.25;
    }
    return vec3<f32>(sum, grad.x, grad.y);
}

// Domain-warped fBm height field's tangent-space normal at world XZ `xz`.
// (-dH/dx, -dH/dz, 1) normalized, scaled by `strength` (Z is the TS up axis,
// matching the previous convention). Warp Jacobian applied via the chain rule.
fn waterDetailNormalTS(xz: vec2<f32>, t: f32, strength: f32) -> vec3<f32> {
    const baseScale = 0.15;              // world -> noise space
    const k = 0.45;                      // warp intensity

    // The noise is sampled at absolute coords `p*freq + wind*t`, so an unbounded
    // `t` eventually pushes that past f32 fractional precision and the ripples
    // degrade. Wrap `t` to a long period: sessions under it are unaffected; a
    // marathon run resets the ripple field once per period (a fully seamless fix
    // needs tiling/flow noise — deferred).
    const kTimeWrap = 3600.0;
    let tw = t - floor(t / kTimeWrap) * kTimeWrap;

    let p = xz * baseScale;

    // Two low-freq fBm samples form the warp offset; keep their gradients for J.
    let w1 = waterFbm(p, tw);
    let w2 = waterFbm(p + vec2<f32>(5.2, 1.3), tw);
    let q = p + k * vec2<f32>(w1.x, w2.x);

    let h = waterFbm(q, tw);
    let gq = vec2<f32>(h.y, h.z);        // dH/dq
    // dH/dp = Jᵀ·gq, J = I + k·[grad w1; grad w2]
    let dHdx = (1.0 + k * w1.y) * gq.x + (k * w2.y) * gq.y;
    let dHdz = (k * w1.z) * gq.x + (1.0 + k * w2.z) * gq.y;

    // Bump amplitude: `strength` (params2.x) scales the tangent-space slope. The
    // fBm gradient is ~O(1), so without a gain the ripples are nearly flat (~3deg)
    // and the sharp sun-specular lobe never catches — kAmp restores steep,
    // glinting ripples. The gradient RATIO stays the exact analytic direction;
    // only the magnitude is an artistic bump strength.
    const kAmp = 2.5;
    let s = strength * kAmp;
    return normalize(vec3<f32>(-dHdx * s, -dHdz * s, 1.0));
}
