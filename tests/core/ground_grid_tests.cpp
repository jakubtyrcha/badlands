#include <doctest.h>

#include <ground_grid.h>

// Pins shaders/ground_grid.h -- the ground plate's shading math. The whole
// reason that math lives in a dual-compile header (sdf_scene.h precedent) is
// so it can be exercised here with no Metal device: `dq`, which is fwidth(q)
// on the GPU, is just an argument.
//
// Coordinate reminder: q.x is world X, q.y is world Z (the plate is on y=0).

namespace {

// A derivative small enough that every line is many pixels wide -- i.e. the
// fully-resolved regime, well below kGroundFadeBegin for both tiers.
constexpr float kFine = 0.004f;
sq_float2 fine() { return gg_make2(kFine, kFine); }

sq_float4 shade(float x, float z, sq_float2 dq) {
    return ground_grid_shade(gg_make2(x, z), dq, kGroundHalfExtent,
                             kGroundMinorSpacing, kGroundMajorSpacing);
}
sq_float4 shade(float x, float z) { return shade(x, z, fine()); }

} // namespace

TEST_CASE("gg_line_coverage: on-line vs between-line, and the periodicity") {
    const sq_float2 dq = fine();

    SUBCASE("dead on a line: full coverage") {
        CHECK(gg_line_coverage(gg_make2(3.0f, 0.37f), dq, 1.0f) == doctest::Approx(1.0f));
    }
    SUBCASE("mid-cell: no coverage") {
        CHECK(gg_line_coverage(gg_make2(3.5f, 0.37f), dq, 1.0f) == doctest::Approx(0.0f));
    }
    // These two land exactly on the AA ramp's endpoints, so they carry an
    // absolute tolerance: `3.0f + kFine` does not round-trip to a distance of
    // exactly kFine in float32, which leaves ~1e-5 of residual coverage.
    SUBCASE("one pixel off the line: coverage has fallen to zero") {
        // e == 1 exactly at a distance of one derivative unit.
        CHECK(gg_line_coverage(gg_make2(3.0f + kFine, 0.37f), dq, 1.0f) < 1e-3f);
    }
    SUBCASE("half a pixel off: half coverage") {
        CHECK(gg_line_coverage(gg_make2(3.0f + 0.5f * kFine, 0.37f), dq, 1.0f)
              == doctest::Approx(0.5f).epsilon(1e-3));
    }
    SUBCASE("the major tier ignores non-multiples of its spacing") {
        CHECK(gg_line_coverage(gg_make2(3.0f, 0.37f), dq, 5.0f) == doctest::Approx(0.0f));
        CHECK(gg_line_coverage(gg_make2(5.0f, 0.37f), dq, 5.0f) == doctest::Approx(1.0f));
    }
}

TEST_CASE("gg_density_fade: over-dense tiers fade out instead of aliasing into a smear") {
    // This is the property the finite-plate ruling depends on: the plate keeps
    // fixed 1-unit spacing across the camera's whole 0.5..90 radius range with
    // no LOD, because a tier that can no longer be resolved stops being drawn.
    CHECK(gg_density_fade(0.0f) == doctest::Approx(1.0f));
    CHECK(gg_density_fade(kGroundFadeBegin) == doctest::Approx(1.0f));
    CHECK(gg_density_fade(kGroundFadeEnd) == doctest::Approx(0.0f));
    CHECK(gg_density_fade(10.0f) == doctest::Approx(0.0f));

    // Monotonic non-increasing across the transition.
    float prev = 2.0f;
    for (int i = 0; i <= 40; ++i) {
        const float v = gg_density_fade(static_cast<float>(i) * 0.05f);
        CHECK(v <= prev + 1e-6f);
        prev = v;
    }

    SUBCASE("a coarse derivative kills coverage that the naive form would saturate to 1") {
        // dq == 1.0 world unit per pixel: minor lines are packed one per
        // pixel. Without the fade this returns ~1 EVERYWHERE (the classic
        // derivative-AA grid failure) -- a solid white smear.
        const sq_float2 coarse = gg_make2(1.0f, 1.0f);
        CHECK(gg_line_coverage(gg_make2(3.0f, 0.37f), coarse, 1.0f) == doctest::Approx(0.0f));
        // The major tier, 5x coarser, is still resolvable at that derivative.
        CHECK(gg_line_coverage(gg_make2(5.0f, 0.37f), coarse, 5.0f) > 0.5f);
    }
}

TEST_CASE("ground_grid_shade: tier alphas mean literally what they say") {
    // Every major line coincides with a minor line, and both axes coincide
    // with both tiers, so this is entirely about the masking in
    // ground_grid_shade -- not about the individual coverage functions.

    SUBCASE("a minor line alone") {
        const sq_float4 c = shade(3.0f, 0.37f);
        CHECK(c.w == doctest::Approx(kGroundMinorAlpha));
    }
    SUBCASE("a major line reads as major, NOT major stacked on minor") {
        const sq_float4 c = shade(5.0f, 0.37f);
        CHECK(c.w == doctest::Approx(kGroundMajorAlpha));
        // Would be 0.36 + 0.15*(1-0.36) = 0.456 without the mask.
        CHECK(c.w < 0.40f);
    }
    SUBCASE("empty ground between every tier") {
        CHECK(shade(3.5f, 0.37f).w == doctest::Approx(0.0f));
    }
}

TEST_CASE("ground_grid_shade: axes are coloured, direction-signed, and beat the tiers") {
    SUBCASE("+X half is the axis colour at the positive alpha") {
        const sq_float4 c = shade(7.3f, 0.0f); // on z==0, off any X multiple of 1
        CHECK(c.w == doctest::Approx(kGroundAxisAlphaPos));
        // Premultiplied, so divide out to recover the hue.
        CHECK(c.x / c.w == doctest::Approx(kGroundAxisX.x));
        CHECK(c.y / c.w == doctest::Approx(kGroundAxisX.y));
        CHECK(c.z / c.w == doctest::Approx(kGroundAxisX.z));
    }
    SUBCASE("-X half is the same colour, dimmer") {
        const sq_float4 c = shade(-7.3f, 0.0f);
        CHECK(c.w == doctest::Approx(kGroundAxisAlphaNeg));
        CHECK(c.x / c.w == doctest::Approx(kGroundAxisX.x));
    }
    SUBCASE("+Z half carries the Z colour") {
        const sq_float4 c = shade(0.0f, 7.3f);
        CHECK(c.w == doctest::Approx(kGroundAxisAlphaPos));
        CHECK(c.z / c.w == doctest::Approx(kGroundAxisZ.z));
    }
    SUBCASE("-Z half is dimmer") {
        CHECK(shade(0.0f, -7.3f).w == doctest::Approx(kGroundAxisAlphaNeg));
    }
    SUBCASE("an axis sitting on a major line still reads as the axis alpha") {
        // x == 10 is a major line AND a minor line; z == 0 is the X axis.
        const sq_float4 c = shade(10.0f, 0.0f);
        CHECK(c.w == doctest::Approx(kGroundAxisAlphaPos));
    }
}

TEST_CASE("ground_grid_shade: the plate is finite") {
    SUBCASE("nothing at all well outside the extent") {
        CHECK(shade(kGroundHalfExtent + 5.0f, 3.5f).w == doctest::Approx(0.0f));
        CHECK(shade(3.5f, -(kGroundHalfExtent + 5.0f)).w == doctest::Approx(0.0f));
    }
    SUBCASE("the border line sits exactly at the extent") {
        const sq_float4 c = shade(kGroundHalfExtent, 0.37f);
        CHECK(c.w == doctest::Approx(kGroundBorderAlpha));
    }
    SUBCASE("the plate is square (Chebyshev), not a disc") {
        // The corner is inside; a point the same Euclidean distance away
        // along +X is outside.
        CHECK(shade(kGroundHalfExtent - 0.5f, kGroundHalfExtent - 0.5f).w >= 0.0f);
        CHECK(shade(kGroundHalfExtent + 2.0f, 0.37f).w == doctest::Approx(0.0f));
    }
    SUBCASE("tiers are clipped at the extent, but the border is not") {
        // Just outside: minor tier gone, border still fading in its last pixel.
        const sq_float4 c = shade(kGroundHalfExtent + 0.5f * kFine, 3.0f);
        CHECK(c.w > 0.0f);
        CHECK(c.w < kGroundBorderAlpha);
    }
}

TEST_CASE("ground_grid_shade: premultiplied output is never over-bright") {
    // ground_pso_ blends One / OneMinusSourceAlpha, which is only correct if
    // every channel is already scaled by alpha.
    for (float x = -35.0f; x <= 35.0f; x += 0.37f) {
        for (float z = -8.0f; z <= 8.0f; z += 0.53f) {
            const sq_float4 c = shade(x, z);
            CAPTURE(x);
            CAPTURE(z);
            CHECK(c.w >= 0.0f);
            CHECK(c.w <= 1.0f);
            CHECK(c.x <= c.w + 1e-5f);
            CHECK(c.y <= c.w + 1e-5f);
            CHECK(c.z <= c.w + 1e-5f);
        }
    }
}

TEST_CASE("gg_grazing_fade: kills the plate as the view flattens into it") {
    CHECK(gg_grazing_fade(0.0f) == doctest::Approx(0.0f));
    CHECK(gg_grazing_fade(1.0f) == doctest::Approx(1.0f));
    CHECK(gg_grazing_fade(-1.0f) == doctest::Approx(1.0f)); // symmetric: seen from below too
    CHECK(gg_grazing_fade(kGroundGrazingLimit) == doctest::Approx(1.0f));
    CHECK(gg_grazing_fade(kGroundGrazingLimit * 0.5f) == doctest::Approx(0.5f));
}
