#include <doctest.h>

#include "rhi_renderer.h"

using namespace sq;

// Pins build_raymarch_uniforms (R1's per-frame RaymarchUniforms builder,
// pulled out of Renderer::render() so it's directly testable without any
// Metal device/drawable involved -- see its doc comment in renderer.h): a
// pure field-for-field transcription, so this just confirms nothing gets
// mixed up (e.g. view_proj/inv_view_proj swapped, or a value landing in the
// wrong params0/params1 lane).
TEST_CASE("build_raymarch_uniforms: packs view_proj/inv_view_proj verbatim and "
          "params0/params1 in the documented field order") {
    // Distinct per-column translation components so a column mixup between
    // view_proj and inv_view_proj would be caught.
    simd_float4x4 view_proj = matrix_identity_float4x4;
    view_proj.columns[3] = simd_make_float4(1.0f, 2.0f, 3.0f, 1.0f);

    simd_float4x4 inv_view_proj = matrix_identity_float4x4;
    inv_view_proj.columns[3] = simd_make_float4(4.0f, 5.0f, 6.0f, 1.0f);

    const RaymarchUniforms u =
        build_raymarch_uniforms(view_proj, inv_view_proj, 1920.0f, 1080.0f, 42, 0.1f, 100.0f);

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            CHECK(u.view_proj.columns[col][row] == doctest::Approx(view_proj.columns[col][row]));
            CHECK(u.inv_view_proj.columns[col][row] == doctest::Approx(inv_view_proj.columns[col][row]));
        }
    }

    // params0 = (drawable_width_px, drawable_height_px, node_count, 0)
    CHECK(u.params0.x == doctest::Approx(1920.0f));
    CHECK(u.params0.y == doctest::Approx(1080.0f));
    CHECK(u.params0.z == doctest::Approx(42.0f));
    CHECK(u.params0.w == doctest::Approx(0.0f));

    // params1 = (near, far, 0, 0)
    CHECK(u.params1.x == doctest::Approx(0.1f));
    CHECK(u.params1.y == doctest::Approx(100.0f));
    CHECK(u.params1.z == doctest::Approx(0.0f));
    CHECK(u.params1.w == doctest::Approx(0.0f));
}

// Same shape, same purpose, for the ground-plate pass's uniform builder. The
// extent/spacings ride in as arguments rather than being read from
// ground_grid.h inside the builder, so this pins the field mapping
// independently of whatever constants Renderer::render() happens to pass --
// values chosen distinct from the real ones for exactly that reason.
TEST_CASE("build_ground_grid_uniforms: packs the matrices verbatim and params0/params1 "
          "in the documented field order") {
    simd_float4x4 view_proj = matrix_identity_float4x4;
    view_proj.columns[3] = simd_make_float4(1.0f, 2.0f, 3.0f, 1.0f);

    simd_float4x4 inv_view_proj = matrix_identity_float4x4;
    inv_view_proj.columns[3] = simd_make_float4(4.0f, 5.0f, 6.0f, 1.0f);

    const GroundGridUniforms u = build_ground_grid_uniforms(
        view_proj, inv_view_proj, 1920.0f, 1080.0f, 12.5f, 0.25f, 2.5f);

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            CHECK(u.view_proj.columns[col][row] == doctest::Approx(view_proj.columns[col][row]));
            CHECK(u.inv_view_proj.columns[col][row] == doctest::Approx(inv_view_proj.columns[col][row]));
        }
    }

    // params0 = (drawable_width_px, drawable_height_px, half_extent, 0)
    CHECK(u.params0.x == doctest::Approx(1920.0f));
    CHECK(u.params0.y == doctest::Approx(1080.0f));
    CHECK(u.params0.z == doctest::Approx(12.5f));
    CHECK(u.params0.w == doctest::Approx(0.0f));

    // params1 = (minor_spacing, major_spacing, 0, 0)
    CHECK(u.params1.x == doctest::Approx(0.25f));
    CHECK(u.params1.y == doctest::Approx(2.5f));
    CHECK(u.params1.z == doctest::Approx(0.0f));
    CHECK(u.params1.w == doctest::Approx(0.0f));
}
