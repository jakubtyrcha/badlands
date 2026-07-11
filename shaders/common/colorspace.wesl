// Bidirectional conversion between linear sRGB and Oklab
// Uses Ottosson's published M1/M2 matrices
// Input/output is linear sRGB (caller handles gamma via color.wesl)

fn linear_srgb_to_oklab(c: vec3<f32>) -> vec3<f32> {
    // M1: linear sRGB -> LMS (approximate)
    let l = 0.4122214708 * c.x + 0.5363325363 * c.y + 0.0514459929 * c.z;
    let m = 0.2119034982 * c.x + 0.6806995451 * c.y + 0.1073969566 * c.z;
    let s = 0.0883024619 * c.x + 0.2817188376 * c.y + 0.6299787005 * c.z;

    // Cube root (perceptual linearization)
    let l_ = pow(max(l, 0.0), 1.0 / 3.0);
    let m_ = pow(max(m, 0.0), 1.0 / 3.0);
    let s_ = pow(max(s, 0.0), 1.0 / 3.0);

    // M2: LMS^(1/3) -> Oklab
    return vec3<f32>(
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    );
}

fn oklab_to_linear_srgb(lab: vec3<f32>) -> vec3<f32> {
    // Inverse M2: Oklab -> LMS^(1/3)
    let l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    let m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    let s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;

    // Cube (undo perceptual linearization)
    let l = l_ * l_ * l_;
    let m = m_ * m_ * m_;
    let s = s_ * s_ * s_;

    // Inverse M1: LMS -> linear sRGB
    return vec3<f32>(
         4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    );
}

// OKLab / Display P3 color space conversions
// Based on CSS Color Level 4 specification matrices

// Forward: Display P3 linear -> XYZ D65
const P3_TO_XYZ = mat3x3<f32>(
    vec3<f32>(0.4865709486, 0.2289745641, 0.0000000000),
    vec3<f32>(0.2656676932, 0.6917385218, 0.0451133819),
    vec3<f32>(0.1982172852, 0.0792869141, 1.0439443689),
);

// XYZ D65 -> LMS (CSS Color Level 4)
const XYZ_TO_LMS = mat3x3<f32>(
    vec3<f32>( 0.8189330101, 0.0329845436, 0.0482003018),
    vec3<f32>( 0.3618667424, 0.9293118715, 0.2643662691),
    vec3<f32>(-0.1288597137, 0.0361456387, 0.6338517070),
);

// LMS^(1/3) -> OKLab (M2)
const LMS_TO_OKLAB = mat3x3<f32>(
    vec3<f32>( 0.2104542553, 1.9779984951, 0.0259040371),
    vec3<f32>( 0.7936177850,-2.4285922050, 0.7827717662),
    vec3<f32>(-0.0040720468, 0.4505937099,-0.8086757660),
);

// Inverse: OKLab -> LMS^(1/3) (M2 inverse)
const OKLAB_TO_LMS = mat3x3<f32>(
    vec3<f32>(1.0000000000, 1.0000000000, 1.0000000000),
    vec3<f32>(0.3963377774,-0.1055613458,-0.0894841775),
    vec3<f32>(0.2158037573,-0.0638541728,-1.2914855480),
);

// LMS -> XYZ D65 (inverse of XYZ_TO_LMS)
const LMS_TO_XYZ = mat3x3<f32>(
    vec3<f32>( 1.2270138511,-0.0405801784,-0.0763812845),
    vec3<f32>(-0.5577999807, 1.1122568696,-0.4214819784),
    vec3<f32>( 0.2812561490,-0.0716766787, 1.5861632204),
);

// XYZ D65 -> Display P3 linear (inverse of P3_TO_XYZ)
const XYZ_TO_P3 = mat3x3<f32>(
    vec3<f32>( 2.4934969119,-0.8294889696, 0.0358458302),
    vec3<f32>(-0.9313836179, 1.7626640603,-0.0761723893),
    vec3<f32>(-0.4027107845, 0.0236246858, 0.9568845240),
);

// Precomputed combined: M_FWD = XYZ_TO_LMS * P3_TO_XYZ
const M_FWD = mat3x3<f32>(
    vec3<f32>( 0.4812071845, 0.2288339014, 0.0839498937),
    vec3<f32>( 0.4621742084, 0.6532168774, 0.2238898507),
    vec3<f32>( 0.0566017575, 0.1179492212, 0.6923084280),
);

// Precomputed combined: M_INV = XYZ_TO_P3 * LMS_TO_XYZ
const M_INV = mat3x3<f32>(
    vec3<f32>( 3.1277662610,-0.9459432697, 0.0597641079),
    vec3<f32>(-2.2571350655, 2.0040755505,-0.2175783521),
    vec3<f32>( 0.1293687945,-0.0581322354, 1.1578067568),
);

fn safe_cbrt(v: vec3<f32>) -> vec3<f32> {
    return sign(v) * pow(abs(v), vec3<f32>(1.0 / 3.0));
}

fn linear_p3_to_oklab(rgb: vec3<f32>) -> vec3<f32> {
    let lms = M_FWD * rgb;
    let lms_g = safe_cbrt(lms);
    return LMS_TO_OKLAB * lms_g;
}

fn oklab_to_linear_p3(lab: vec3<f32>) -> vec3<f32> {
    let lms_g = OKLAB_TO_LMS * lab;
    let lms = lms_g * lms_g * lms_g;
    return M_INV * lms;
}

// Direct sRGB linear <-> Display P3 linear conversion (both D65 white point).
// Precomputed as XYZ_TO_P3 * SRGB_TO_XYZ where SRGB_TO_XYZ is from IEC 61966-2-1.

const SRGB_TO_P3 = mat3x3<f32>(
    vec3<f32>( 0.8224619688, 0.0331941989, 0.0170826307),
    vec3<f32>( 0.1775380312, 0.9668058012, 0.0723974406),
    vec3<f32>( 0.0000000000, 0.0000000000, 0.9105199285),
);

const P3_TO_SRGB = mat3x3<f32>(
    vec3<f32>( 1.2249401762, -0.0420569547, -0.0196375545),
    vec3<f32>(-0.2249401762,  1.0420569547, -0.0786360455),
    vec3<f32>( 0.0000000000,  0.0000000000,  1.0982736002),
);

fn linear_srgb_to_linear_p3(c: vec3<f32>) -> vec3<f32> {
    return SRGB_TO_P3 * c;
}

fn linear_p3_to_linear_srgb(c: vec3<f32>) -> vec3<f32> {
    return P3_TO_SRGB * c;
}
