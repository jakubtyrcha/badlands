// Catch2 suite for the prop triangle-LOD producer (game/visual/prop_lod_model.hpp).
//
// CPU only -- no device, no .usdc. The fixture is a hand-built FLAT-SHADED
// surface, which reproduces the pathology the shipped props actually have:
// every triangle carries its own three vertices with a face normal, so no two
// vertices are bitwise alike and SimplifyMesh's whole-vertex weld recovers no
// topology at all. Four of the ten shipped props are like this (boulder_01,
// both brass vases, the mace), and on them plain edge collapse returns 1.000
// of the source triangles at every ratio down to 0.05 -- see
// game/tests/prop_lod_report_tests.cpp.
//
// The first test below pins that pathology directly, so the reason this
// producer welds at all cannot quietly stop being true.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/geometry/usd_mesh_adapter.hpp"
#include "game/geometry/mesh_lod.hpp"
#include "game/visual/instanced_lod_model.hpp"
#include "game/visual/prop_lod_model.hpp"

using namespace badlands;

namespace {

constexpr size_t kStride = kTexturedMeshFloatsPerVertex;

// A bumpy N x N grid, FLAT SHADED: each triangle emits its own three vertices
// carrying that triangle's face normal. Positions and UVs are shared between
// adjacent triangles (they come from the grid), so a position+UV weld can
// recover the topology while a whole-vertex weld cannot -- exactly the
// situation the props are in.
//
// Bumpy rather than planar on purpose: a flat plane's face normals are all
// equal, so even a whole-vertex weld would succeed and the fixture would prove
// nothing.
ImportedModel MakeFlatShadedGrid(int n, float extent = 2.0f) {
  const auto height = [](float x, float z) {
    return 0.35f * std::sin(x * 2.1f) * std::sin(z * 1.7f);
  };
  const auto pos = [&](int i, int j) {
    const float x = extent * (static_cast<float>(i) / n - 0.5f);
    const float z = extent * (static_cast<float>(j) / n - 0.5f);
    return glm::vec3(x, height(x, z), z);
  };
  const auto uv = [&](int i, int j) {
    return glm::vec2(static_cast<float>(i) / n, static_cast<float>(j) / n);
  };

  ImportedModel model;
  model.name = "flat_shaded_grid";
  StaticTexturedMeshComponent& mesh = model.mesh.mesh;
  mesh.geometry_type = GeometryType::kTexturedMesh;

  const auto emit = [&](const glm::vec3& p, const glm::vec2& t,
                        const glm::vec3& nrm) {
    const float v[kStride] = {p.x, p.y, p.z, t.x, t.y, nrm.x, nrm.y, nrm.z,
                              1.0f, 0.0f, 0.0f, 1.0f};
    mesh.vertices.insert(mesh.vertices.end(), v, v + kStride);
    mesh.indices.push_back(mesh.vertex_count++);
  };
  const auto tri = [&](int ai, int aj, int bi, int bj, int ci, int cj) {
    const glm::vec3 a = pos(ai, aj), b = pos(bi, bj), c = pos(ci, cj);
    const glm::vec3 face = glm::normalize(glm::cross(b - a, c - a));
    emit(a, uv(ai, aj), face);
    emit(b, uv(bi, bj), face);
    emit(c, uv(ci, cj), face);
  };

  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      tri(i, j, i + 1, j, i, j + 1);
      tri(i + 1, j, i + 1, j + 1, i, j + 1);
    }
  }
  model.mesh.local_bounds = ComputeLocalAabbFromVertices(mesh.vertices, kStride);
  return model;
}

size_t TriCount(const TexturedMeshResult& m) { return m.mesh.indices.size() / 3; }

}  // namespace

TEST_CASE("the fixture reproduces the pathology: flat shading blocks collapse",
          "[prop_lod]") {
  const ImportedModel grid = MakeFlatShadedGrid(24);
  const auto& mesh = grid.mesh.mesh;
  const size_t source_tris = mesh.indices.size() / 3;

  // Unwelded, every triangle is its own island: edge collapse has no shared
  // edge to work on and returns essentially the whole mesh.
  const SimplifiedMesh naive =
      SimplifyMesh(mesh.vertices, kStride, mesh.indices, 0.25f);
  const double naive_ratio =
      static_cast<double>(naive.indices.size() / 3) / source_tris;
  CAPTURE(source_tris, naive.indices.size() / 3);
  CHECK(naive_ratio > 0.9);

  // Welded on position+UV, the same target is reachable.
  const SimplifiedMesh welded =
      WeldMeshByPrefix(mesh.vertices, kStride, mesh.indices, 5);
  CHECK(welded.vertex_count < mesh.vertex_count);
  const SimplifiedMesh after =
      SimplifyMesh(welded.vertices, kStride, welded.indices, 0.25f);
  const double welded_ratio =
      static_cast<double>(after.indices.size() / 3) / source_tris;
  CAPTURE(welded_ratio);
  CHECK(welded_ratio < 0.4);
}

TEST_CASE("WeldMeshByPrefix keeps UV seams apart", "[prop_lod]") {
  // Two triangles sharing an edge in POSITION but split in UV -- a texture
  // seam. Welding on position alone would fuse them and smear the texture
  // across the seam; the producer's prefix of 5 must not.
  std::vector<float> verts;
  std::vector<uint32_t> indices;
  const auto emit = [&](float x, float y, float u) {
    const float v[kStride] = {x, y, 0, u, 0, 0, 0, 1, 1, 0, 0, 1};
    verts.insert(verts.end(), v, v + kStride);
    indices.push_back(static_cast<uint32_t>(indices.size()));
  };
  emit(0, 0, 0.0f); emit(1, 0, 0.0f); emit(0, 1, 0.0f);
  emit(0, 0, 1.0f); emit(1, 0, 1.0f); emit(0, 1, 1.0f);  // same posns, other uv

  const SimplifiedMesh pos_only = WeldMeshByPrefix(verts, kStride, indices, 3);
  const SimplifiedMesh pos_uv = WeldMeshByPrefix(verts, kStride, indices, 5);
  CHECK(pos_only.vertex_count == 3);  // seam fused
  CHECK(pos_uv.vertex_count == 6);    // seam preserved
}

TEST_CASE("a prop's levels get monotonically coarser and its chain validates",
          "[prop_lod]") {
  const ImportedModel grid = MakeFlatShadedGrid(40);
  const InstancedLodModel model = BuildPropLodModel(grid, PropMaterialTextures{});

  // ValidateLodModel is what BuildInstancedLodField gates on, so a producer
  // whose output fails it would only be caught at field-build time.
  CHECK(ValidateLodModel(model).empty());
  REQUIRE(model.levels.size() >= 2);
  REQUIRE(model.thresholds.size() == model.levels.size() - 1);
  CHECK(model.submesh_count() == 1);

  for (size_t lod = 1; lod < model.levels.size(); ++lod) {
    CAPTURE(lod, TriCount(model.levels[lod - 1][0]),
            TriCount(model.levels[lod][0]));
    CHECK(TriCount(model.levels[lod][0]) < TriCount(model.levels[lod - 1][0]));
    // No level may be empty: an empty slot renders as a hole at that distance.
    CHECK(TriCount(model.levels[lod][0]) > 0);
  }
}

TEST_CASE("LOD 0 is welded, so every level shades the same way", "[prop_lod]") {
  const ImportedModel grid = MakeFlatShadedGrid(24);
  const InstancedLodModel model = BuildPropLodModel(grid, PropMaterialTextures{});

  REQUIRE_FALSE(model.levels.empty());
  // The whole point of welding at level 0 too: no faceted-to-smooth pop at the
  // first switch, and a much smaller vertex buffer.
  CHECK(model.levels[0][0].mesh.vertex_count < grid.mesh.mesh.vertex_count);
}

TEST_CASE("every level's tangent frame is finite, unit and orthogonal",
          "[prop_lod]") {
  const ImportedModel grid = MakeFlatShadedGrid(32);
  const InstancedLodModel model = BuildPropLodModel(grid, PropMaterialTextures{});

  for (size_t lod = 0; lod < model.levels.size(); ++lod) {
    const auto& v = model.levels[lod][0].mesh.vertices;
    const size_t count = v.size() / kStride;
    CAPTURE(lod, count);
    REQUIRE(count > 0);
    for (size_t i = 0; i < count; ++i) {
      const glm::vec3 n(v[i * kStride + 5], v[i * kStride + 6],
                        v[i * kStride + 7]);
      const glm::vec3 t(v[i * kStride + 8], v[i * kStride + 9],
                        v[i * kStride + 10]);
      const float w = v[i * kStride + 11];
      CAPTURE(i, n.x, n.y, n.z, t.x, t.y, t.z);

      REQUIRE(std::isfinite(n.x + n.y + n.z + t.x + t.y + t.z));
      REQUIRE(glm::length(n) == Catch::Approx(1.0f).epsilon(0.001));
      REQUIRE(glm::length(t) == Catch::Approx(1.0f).epsilon(0.001));
      // The shader recomputes B = w * cross(N, T) after orthogonalizing T
      // against N with normalize(T - N*dot(T,N)). A tangent PARALLEL to the
      // normal makes that normalize(vec3(0)) -- NaN on the GPU and invisible
      // everywhere else -- so near-orthogonality is the property that matters,
      // not merely non-zero length.
      REQUIRE(std::abs(glm::dot(n, t)) < 0.01f);
      REQUIRE((w == 1.0f || w == -1.0f));
    }
  }
}

TEST_CASE("the impostor spec is opaque and points at real geometry",
          "[prop_lod]") {
  const ImportedModel grid = MakeFlatShadedGrid(24);
  const InstancedLodModel model = BuildPropLodModel(grid, PropMaterialTextures{});

  REQUIRE(model.impostor.active());
  // Opaque skips the thickness pass -- a render per view plus an R16Float
  // readback, to fill a channel multiplied by a zero transmission strength.
  CHECK(model.impostor.opaque);
  CHECK(model.impostor.transmission_strength == 0.0f);
  for (const ImpostorBakeSubmesh& sub : model.impostor.submeshes) {
    REQUIRE(sub.lod < model.levels.size());
    REQUIRE(sub.submesh < model.levels[sub.lod].size());
    CHECK(model.levels[sub.lod][sub.submesh].mesh.vertex_count > 0);
    // A prop's albedo comes from its texture, so no vertex-brightness term.
    CHECK(sub.voxel_brightness == 0.0f);
  }
}

TEST_CASE("an empty mesh yields a valid one-level model rather than a crash",
          "[prop_lod]") {
  ImportedModel empty;
  empty.name = "empty";
  const InstancedLodModel model = BuildPropLodModel(empty, PropMaterialTextures{});

  CHECK(model.levels.size() == 1);
  CHECK(model.thresholds.empty());
  CHECK_FALSE(model.impostor.active());
}
