// Catch2 suite for the USD import layer (src/engine/assets/).
//
// Runs against badlands_usd_lib ALONE -- no badlands_engine, no Dawn, no SDL3
// (see CMakeLists.txt). That isolation is the point: the parser is exercised
// without a GPU, so a failure here is a parser failure and nothing else.
//
// Two tests, deliberately split:
//   [usd]          pins the invariants the adapter downstream depends on.
//   [usd][.report] dumps what tinyusdz actually produced. Hidden by default
//                  (the leading '.'), since it is a report and not a pass/fail
//                  judgement -- run it explicitly when an asset or the tinyusdz
//                  pin changes:
//                      ./build/badlands_usd_tests "[.report]"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>
#include <string>
#include <vector>

#include "engine/assets/usd_loader.hpp"

namespace {

// Every prop under assets/models/, sorted -- the same order the viewer's
// generator list uses, so an index here means the same model there.
std::vector<std::filesystem::path> ShippedModels() {
  std::vector<std::filesystem::path> out;
  const std::filesystem::path root{"assets/models"};
  if (!std::filesystem::exists(root)) return out;

  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_directory()) continue;
    for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
      if (file.path().extension() == ".usdc") out.push_back(file.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST_CASE("every shipped prop loads with the attributes the adapter needs",
          "[usd]") {
  const auto models = ShippedModels();
  REQUIRE(models.size() == 10);

  for (const auto& path : models) {
    CAPTURE(path.string());
    const badlands::UsdSceneData scene = badlands::LoadUsdScene(path.string());
    REQUIRE(scene.ok);
    REQUIRE_FALSE(scene.meshes.empty());

    for (const auto& mesh : scene.meshes) {
      CAPTURE(mesh.name);
      CHECK(mesh.vertex_count() > 0);
      CHECK(mesh.triangle_count() > 0);
      // Triangulated: the index buffer is whole triangles.
      CHECK(mesh.indices.size() % 3 == 0);
      // Single-indexable: every attribute array is parallel to `positions`.
      // The adapter interleaves these into one vertex, so a short array here
      // would be a buffer overrun there rather than a cosmetic gap.
      CHECK(mesh.normals.size() == mesh.vertex_count() * 3);
      CHECK(mesh.uvs.size() == mesh.vertex_count() * 2);
      // Tangents are xyzw: w is the bitangent handedness the shader needs.
      // Their presence is what makes normal mapping possible, so it is pinned
      // rather than treated as a bonus.
      CHECK(mesh.tangents.size() == mesh.vertex_count() * 4);
      // Handedness must be a clean sign, not an interpolated or absent value --
      // the shader multiplies it straight into cross(N, T).
      for (size_t i = 0; i < mesh.vertex_count(); ++i) {
        const float w = mesh.tangents[i * 4 + 3];
        REQUIRE((w == 1.0f || w == -1.0f));
      }
      // Indices address only vertices that exist.
      for (uint32_t index : mesh.indices) {
        REQUIRE(index < mesh.vertex_count());
      }
    }
    CHECK(scene.meters_per_unit > 0.0f);
  }
}

TEST_CASE("a missing file fails cleanly rather than throwing", "[usd]") {
  const badlands::UsdSceneData scene =
      badlands::LoadUsdScene("assets/models/does_not_exist.usdc");
  CHECK_FALSE(scene.ok);
  CHECK(scene.meshes.empty());
}

TEST_CASE("report what each shipped prop contains", "[usd][.report]") {
  for (const auto& path : ShippedModels()) {
    const badlands::UsdSceneData scene = badlands::LoadUsdScene(path.string());
    std::cout << "\n=== " << path.parent_path().filename().string() << " ===\n"
              << "  ok=" << scene.ok << "  meshes=" << scene.meshes.size()
              << "  materials=" << scene.materials.size()
              << "  metersPerUnit=" << scene.meters_per_unit
              << "  up=" << (scene.z_up ? "Z" : "Y") << "\n";

    size_t total_tris = 0;
    for (const auto& mesh : scene.meshes) {
      total_tris += mesh.triangle_count();

      // Ranges, because a size check only proves an array is present -- these
      // are what show whether its CONTENTS are sane.
      auto range = [](const std::vector<float>& a, size_t stride, size_t c) {
        float lo = std::numeric_limits<float>::infinity(), hi = -lo;
        for (size_t i = c; i < a.size(); i += stride) {
          lo = std::min(lo, a[i]);
          hi = std::max(hi, a[i]);
        }
        return std::pair{lo, hi};
      };
      const auto [u_lo, u_hi] = range(mesh.uvs, 2, 0);
      const auto [v_lo, v_hi] = range(mesh.uvs, 2, 1);

      // Unit-length is what the shading maths assumes; a zero or NaN normal
      // reads as an unlit black surface with nothing logged.
      float n_lo = std::numeric_limits<float>::infinity(), n_hi = -n_lo;
      size_t bad_normals = 0, bad_tangents = 0;
      for (size_t i = 0; i < mesh.vertex_count(); ++i) {
        const float nl = std::sqrt(mesh.normals[i * 3 + 0] * mesh.normals[i * 3 + 0] +
                                   mesh.normals[i * 3 + 1] * mesh.normals[i * 3 + 1] +
                                   mesh.normals[i * 3 + 2] * mesh.normals[i * 3 + 2]);
        // Stride 4, not 3: tangents are xyzw. Reading them three at a time
        // mixes components across vertices, which makes the badT column below
        // report noise -- and badT is the whole point of this report.
        const float tl = std::sqrt(mesh.tangents[i * 4 + 0] * mesh.tangents[i * 4 + 0] +
                                   mesh.tangents[i * 4 + 1] * mesh.tangents[i * 4 + 1] +
                                   mesh.tangents[i * 4 + 2] * mesh.tangents[i * 4 + 2]);
        n_lo = std::min(n_lo, nl);
        n_hi = std::max(n_hi, nl);
        if (!(nl > 0.9f && nl < 1.1f)) ++bad_normals;
        if (!(tl > 0.9f && tl < 1.1f)) ++bad_tangents;
      }

      std::cout << "  mesh '" << mesh.name << "' mat='" << mesh.material_name
                << "' verts=" << mesh.vertex_count()
                << " tris=" << mesh.triangle_count()
                << " u=[" << u_lo << "," << u_hi << "]"
                << " v=[" << v_lo << "," << v_hi << "]"
                << " |n|=[" << n_lo << "," << n_hi << "]"
                << " badN=" << bad_normals << " badT=" << bad_tangents << "\n";
    }
    std::cout << "  TOTAL tris=" << total_tris << "\n";

    for (const auto& mat : scene.materials) {
      std::cout << "  material '" << mat.name << "'\n"
                << "    base_color: " << mat.base_color << "\n"
                << "    normal:     " << mat.normal << "\n"
                << "    arm:        " << mat.occlusion_roughness_metallic << "\n";
    }
  }
}
