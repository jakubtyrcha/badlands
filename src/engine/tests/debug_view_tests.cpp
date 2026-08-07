// What sits behind the two debug windows, tested with no device and no ImGui.
//
// THE WIDGETS ARE NOT TESTABLE; the two things they drive are, and both are
// pure functions. The second case below is the one that matters: it asserts
// that every entry the Graphics debug window can select is ALSO reachable from
// --debug-view, which is what stops a mode existing that only the UI can reach
// and no headless assertion ever covers.

#include <set>
#include <string>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "executables/object_viewer/resolve_pass.hpp"

using namespace badlands::object_viewer;

TEST_CASE("debug views: every UI entry has a CLI name and a distinct constant",
          "[debugview]") {
  const auto& views = DebugViews();
  REQUIRE(views.size() == size_t(DebugView::kCount));

  std::set<std::string> cli_names, labels;
  for (size_t i = 0; i < views.size(); ++i) {
    INFO("view index " << i);
    // A blank name is a mode the CLI cannot select, which is a mode no ctest
    // can cover.
    CHECK_FALSE(views[i].cli.empty());
    CHECK_FALSE(views[i].label.empty());
    // Distinct, or two entries in the radio group select the same shader
    // constant and one of them silently does nothing.
    CHECK(cli_names.insert(std::string(views[i].cli)).second);
    CHECK(labels.insert(std::string(views[i].label)).second);
    // THE INDEX IS THE SHADER CONSTANT, so a name must map back to its own
    // slot. A table reordered without the enum would still parse every name
    // and select the wrong view for each.
    CHECK(DebugViewFromName(views[i].cli) == DebugView(i));
  }
}

TEST_CASE("debug views: an unknown name is refused, not defaulted",
          "[debugview]") {
  // kCount is the refusal. Returning Lit would make a typo render the lit view
  // and exit 0, which is a test that passes without testing anything.
  CHECK(DebugViewFromName("nope") == DebugView::kCount);
  CHECK(DebugViewFromName("") == DebugView::kCount);
  CHECK(DebugViewFromName("Lit") == DebugView::kCount);  // case-sensitive
}

TEST_CASE("sun: elevation 90 points straight up", "[debugview]") {
  // The closed form the Scene lighting window depends on. Swapping the sine and
  // cosine puts the sun on the horizon at noon, which reads as a lighting
  // choice rather than as a bug.
  SunSettings sun;
  sun.elevation_deg = 90.0f;
  const glm::vec3 up = SunDirection(sun);
  CHECK(up.y == Catch::Approx(1.0f).margin(1e-5));
  CHECK(up.x == Catch::Approx(0.0f).margin(1e-5));
  CHECK(up.z == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("sun: elevation 0 is horizontal and azimuth rotates around y",
          "[debugview]") {
  SunSettings sun;
  sun.elevation_deg = 0.0f;

  sun.azimuth_deg = 0.0f;
  const glm::vec3 a0 = SunDirection(sun);
  CHECK(a0.y == Catch::Approx(0.0f).margin(1e-5));
  CHECK(a0.z == Catch::Approx(-1.0f).margin(1e-5));  // azimuth 0 faces -z

  sun.azimuth_deg = 90.0f;
  const glm::vec3 a90 = SunDirection(sun);
  CHECK(a90.y == Catch::Approx(0.0f).margin(1e-5));
  CHECK(a90.x == Catch::Approx(1.0f).margin(1e-5));  // and 90 faces +x

  // A quarter turn apart, so the rotation is around y and not some other axis.
  CHECK(glm::dot(a0, a90) == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("sun: every angle produces a unit vector", "[debugview]") {
  // Including the poles, where a normalize of a near-zero horizontal radius is
  // the classic place this goes NaN.
  for (float el = -90.0f; el <= 90.0f; el += 15.0f) {
    for (float az = 0.0f; az < 360.0f; az += 30.0f) {
      SunSettings sun;
      sun.elevation_deg = el;
      sun.azimuth_deg = az;
      const glm::vec3 d = SunDirection(sun);
      INFO("elevation " << el << " azimuth " << az);
      CHECK(glm::length(d) == Catch::Approx(1.0f).margin(1e-5));
    }
  }
}

// --- The material pack's normal_format ---------------------------------------

#include <filesystem>
#include <fstream>

#include "badlands_assets.h"
#include "engine/rhi/null/null_rhi.hpp"
#include "executables/object_viewer/material_pack.hpp"

namespace {

// A pack written to a temp directory, so nothing here asserts on a SHIPPED
// data file -- re-exporting a texture must not be able to fail a test.
struct TempPack {
  std::filesystem::path dir;

  explicit TempPack(const char* name, const char* normal_format,
                    uint8_t normal_green) {
    dir = std::filesystem::temp_directory_path() /
          ("badlands_pack_test_" + std::string(name));
    std::filesystem::create_directories(dir / "textures");

    // 2x2 is enough: the flip is per texel and the mip chain is not what is
    // under test.
    auto write_png = [&](const char* file, uint8_t r, uint8_t g, uint8_t b) {
      std::vector<uint8_t> px(2 * 2 * 4);
      for (int i = 0; i < 4; ++i) {
        px[i * 4 + 0] = r; px[i * 4 + 1] = g;
        px[i * 4 + 2] = b; px[i * 4 + 3] = 255;
      }
      badlands_write_png((dir / "textures" / file).string().c_str(), px.data(),
                         2, 2);
    };
    write_png("albedo.png", 128, 128, 128);
    write_png("normal.png", 128, normal_green, 255);
    write_png("arm.png", 255, 128, 0);

    std::ofstream manifest(dir / "material.json");
    manifest << R"({"albedo":"textures/albedo.png",)"
             << R"("normal":"textures/normal.png",)"
             << R"("arm":"textures/arm.png",)"
             << R"("normal_format":")" << normal_format << R"("})";
  }
  ~TempPack() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

std::unique_ptr<badlands::rhi::IRhiDevice> MakeNullDevice() {
  return badlands::rhi::CreateDevice(
      {.backend = badlands::rhi::BackendKind::Null, .label = "pack_tests"});
}

}  // namespace

TEST_CASE("material pack: normal_format 'dx' flips green at load",
          "[materialpack]") {
  // THE MANIFEST FIELD IS LOAD-BEARING. material_library.cpp flips a DirectX
  // pack's green channel at load so every uploaded map is GL-convention; the
  // RHI loader ignored it entirely, so a `gl` pack was sampled as if it were
  // `dx` and every normal-mapped surface lit with its v-axis inverted -- which
  // reads as a lighting choice rather than as a bug.
  auto device = MakeNullDevice();
  REQUIRE(device);

  constexpr uint8_t kGreen = 40;  // far from 128, so a flip is unambiguous
  TempPack dx("dx", "dx", kGreen);
  TempPack gl("gl", "gl", kGreen);

  auto dx_pack =
      badlands::object_viewer::LoadMaterialPack(*device, dx.dir.string());
  auto gl_pack =
      badlands::object_viewer::LoadMaterialPack(*device, gl.dir.string());
  REQUIRE(dx_pack);
  REQUIRE(gl_pack);

  const uint8_t dx_green = dx_pack->normal.mips[0][1];
  const uint8_t gl_green = gl_pack->normal.mips[0][1];
  INFO("dx green = " << int(dx_green) << ", gl green = " << int(gl_green));
  CHECK(dx_green == uint8_t(255 - kGreen));
  CHECK(gl_green == kGreen);
  // The other channels are untouched -- a flip that caught red or blue would
  // tilt the normal along the wrong axis entirely.
  CHECK(dx_pack->normal.mips[0][0] == 128);
  CHECK(dx_pack->normal.mips[0][2] == 255);
}

TEST_CASE("material pack: an unknown normal_format is not flipped",
          "[materialpack]") {
  // Warned about and treated as already GL-convention, matching
  // material_library.cpp. Guessing a flip from an unrecognised value would be
  // worse than doing nothing, because it is unverifiable either way.
  auto device = MakeNullDevice();
  REQUIRE(device);
  constexpr uint8_t kGreen = 40;
  TempPack odd("odd", "directx", kGreen);
  auto pack =
      badlands::object_viewer::LoadMaterialPack(*device, odd.dir.string());
  REQUIRE(pack);
  CHECK(pack->normal.mips[0][1] == kGreen);
}
