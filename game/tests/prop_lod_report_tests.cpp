// Characterization report: how far the shipped USD props actually decimate.
//
// Hidden by default (the leading '.' in the tag) -- this is a measurement, not
// a pass/fail judgement. Run it explicitly:
//     ./build/badlands_geometry_tests "[.report]"
//
// WHY THIS EXISTS. SimplifyMesh welds on the FULL 12-float vertex (see
// mesh_lod.cpp's WeldForSimplify, which passes floats_per_vertex * sizeof(float)
// as meshopt's stride), so two vertices at the same position with different
// normals or UVs stay separate and the edge between them cannot collapse. The
// props are authored unwelded -- boulder_01 ships 198,336 vertices for 66,122
// triangles, exactly 3:1 -- so whether a triangle LOD chain is even possible
// depends entirely on how much of that the weld recovers.
//
// The failure mode this is looking for is the one bark already hit:
// meshopt_simplify cannot merge disconnected components, so a mesh of many
// separate shells floors out far above its target no matter how low the ratio
// goes (Oak bark sits at ~774 triangles from ratio 0.05 down to 0.005 -- see
// SimplifyMeshSloppy's comment). If the props floor the same way, the LOD
// producer needs position-only welding (meshopt_simplifyWithAttributes) rather
// than the plain SimplifyMesh path.
//
// The "pos-weld" column is what answers that: it is the vertex count a
// position-only weld would produce, i.e. the topology simplifyWithAttributes
// would get to work on. A large gap between it and "full-weld" means the
// attribute seams are what is blocking decimation and switching the weld would
// help; no gap means the geometry is genuinely that dense and it would not.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "engine/assets/usd_loader.hpp"
#include "engine/assets/usd_material_binding.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/geometry/usd_mesh_adapter.hpp"
#include "game/geometry/mesh_lod.hpp"

namespace {

// Every prop under assets/models/, sorted -- the same order the viewer's
// generator list and usd_loader_tests use.
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

// Distinct vertices when only the first `components` floats are compared.
// components == kTexturedMeshFloatsPerVertex reproduces what SimplifyMesh's
// weld sees; components == 3 is the position-only weld.
//
// Compares by value rather than by bit pattern (meshopt hashes bytes), so -0.0
// and 0.0 merge here and would not there. That difference is immaterial to the
// question this report asks and keeps the test free of a meshoptimizer link --
// the library is PRIVATE to badlands_game_lib.
size_t CountDistinctVertices(const std::vector<float>& vertices, size_t stride,
                             size_t components) {
  std::set<std::vector<float>> distinct;
  const size_t count = vertices.size() / stride;
  for (size_t i = 0; i < count; ++i) {
    const auto begin = vertices.begin() + static_cast<long>(i * stride);
    distinct.emplace(begin, begin + static_cast<long>(components));
  }
  return distinct.size();
}

size_t TriangleCount(const badlands::SimplifiedMesh& mesh) {
  return mesh.indices.size() / 3;
}

}  // namespace

TEST_CASE("how far the shipped props actually decimate", "[proplod][.report]") {
  constexpr size_t kStride = badlands::kTexturedMeshFloatsPerVertex;
  constexpr std::array<float, 3> kRatios{0.5f, 0.2f, 0.05f};

  for (const auto& path : ShippedModels()) {
    const badlands::UsdSceneData scene =
        badlands::LoadUsdScene(path.string());
    badlands::UsdMaterialBinding binding;
    binding.default_pack_dir = path.parent_path().string();
    const std::vector<badlands::ImportedModel> models =
        badlands::BuildImportedModels(scene, binding);

    std::cout << "\n=== " << path.parent_path().filename().string() << " ===\n";

    for (const badlands::ImportedModel& model : models) {
      const auto& mesh = model.mesh.mesh;
      const size_t source_tris = mesh.indices.size() / 3;
      if (source_tris == 0) continue;

      const size_t full_weld =
          CountDistinctVertices(mesh.vertices, kStride, kStride);
      const size_t pos_weld = CountDistinctVertices(mesh.vertices, kStride, 3);

      std::printf(
          "  %-28s verts=%7u tris=%7zu  full-weld=%7zu (%.2fx)  "
          "pos-weld=%7zu (%.2fx)\n",
          model.name.c_str(), mesh.vertex_count, source_tris, full_weld,
          static_cast<double>(mesh.vertex_count) /
              static_cast<double>(std::max<size_t>(full_weld, 1)),
          pos_weld,
          static_cast<double>(mesh.vertex_count) /
              static_cast<double>(std::max<size_t>(pos_weld, 1)));

      for (float ratio : kRatios) {
        const size_t target = static_cast<size_t>(
            static_cast<double>(source_tris) * static_cast<double>(ratio));
        const badlands::SimplifiedMesh collapse =
            badlands::SimplifyMesh(mesh.vertices, kStride, mesh.indices, ratio);
        const badlands::SimplifiedMesh sloppy = badlands::SimplifyMeshSloppy(
            mesh.vertices, kStride, mesh.indices, ratio);

        // "achieved" is the ratio actually reached; a value far above `ratio`
        // is the floor this report exists to detect.
        std::printf(
            "      ratio %.2f  target=%7zu   collapse=%7zu (%.3f)   "
            "sloppy=%7zu (%.3f)\n",
            static_cast<double>(ratio), target, TriangleCount(collapse),
            static_cast<double>(TriangleCount(collapse)) /
                static_cast<double>(source_tris),
            TriangleCount(sloppy),
            static_cast<double>(TriangleCount(sloppy)) /
                static_cast<double>(source_tris));
      }
    }
  }
}
