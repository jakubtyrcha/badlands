#include <catch_amalgamated.hpp>
#include <algorithm>
#include <cmath>
#include "game/geometry/leaf_voxelizer.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/tree_options.hpp"
#include "engine/rendering/geometry/mesh_builder_utils.hpp"  // PushVertex
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

using namespace badlands;

namespace {

glm::vec3 VertexPos(const StaticTexturedMeshComponent& m, uint32_t v) {
  const size_t off = static_cast<size_t>(v) * kTexturedMeshFloatsPerVertex;
  return glm::vec3(m.vertices[off + 0], m.vertices[off + 1], m.vertices[off + 2]);
}
glm::vec2 VertexUv(const StaticTexturedMeshComponent& m, uint32_t v) {
  const size_t off = static_cast<size_t>(v) * kTexturedMeshFloatsPerVertex;
  return glm::vec2(m.vertices[off + 3], m.vertices[off + 4]);
}
glm::vec3 VertexNormal(const StaticTexturedMeshComponent& m, uint32_t v) {
  const size_t off = static_cast<size_t>(v) * kTexturedMeshFloatsPerVertex;
  return glm::vec3(m.vertices[off + 5], m.vertices[off + 6], m.vertices[off + 7]);
}

// Every tet's 4 canonical points, in emit order (base+0..3), give a
// positive-oriented tetrahedron; see leaf_voxelizer_tests.cpp's topology
// test for the derivation this guards.
float SignedTetVolume(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                      const glm::vec3& p3) {
  return glm::dot(glm::cross(p1 - p0, p2 - p0), p3 - p0);
}

}  // namespace

TEST_CASE("VoxelizeLeafCards: byte-deterministic run-to-run (OakPreset)") {
  const TreeOptions oak = OakPreset();
  const TexturedMeshResult leaves = GenerateLeafMesh(oak);
  const LeafVoxelizeOptions opts;

  const TexturedMeshResult a = VoxelizeLeafCards(leaves.mesh, oak.leaves.silhouette, opts);
  const TexturedMeshResult b = VoxelizeLeafCards(leaves.mesh, oak.leaves.silhouette, opts);
  REQUIRE(a.mesh.vertex_count > 0u);
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);
}

TEST_CASE("EmitTetMesh: well-formed tet topology (OakPreset)") {
  const TreeOptions oak = OakPreset();
  const TexturedMeshResult leaves = GenerateLeafMesh(oak);
  const TexturedMeshResult r =
      VoxelizeLeafCards(leaves.mesh, oak.leaves.silhouette, LeafVoxelizeOptions{});
  const auto& m = r.mesh;

  REQUIRE(m.vertex_count > 0u);
  REQUIRE(m.vertex_count % 4u == 0u);
  REQUIRE(m.indices.size() == 3u * static_cast<size_t>(m.vertex_count));
  REQUIRE(m.vertices.size() == static_cast<size_t>(m.vertex_count) * kTexturedMeshFloatsPerVertex);
  for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
  for (float f : m.vertices) REQUIRE(std::isfinite(f));

  const uint32_t tet_count = m.vertex_count / 4u;
  for (uint32_t t = 0; t < tet_count; ++t) {
    INFO("tet " << t);
    const uint32_t base = t * 4u;
    const glm::vec3 n0 = VertexNormal(m, base + 0);
    REQUIRE(glm::length(n0) == Catch::Approx(1.0f).margin(1e-4f));
    for (uint32_t k = 1; k < 4; ++k) {
      const glm::vec3 nk = VertexNormal(m, base + k);
      REQUIRE(nk.x == Catch::Approx(n0.x).margin(1e-5f));
      REQUIRE(nk.y == Catch::Approx(n0.y).margin(1e-5f));
      REQUIRE(nk.z == Catch::Approx(n0.z).margin(1e-5f));
    }

    const float vol = SignedTetVolume(VertexPos(m, base + 0), VertexPos(m, base + 1),
                                      VertexPos(m, base + 2), VertexPos(m, base + 3));
    REQUIRE(vol > 0.0f);
  }
}

TEST_CASE("SplatLeafCards: hand-built single card occupies exactly the expected cells") {
  // PineSprig's stem column (leaf_texture.cpp's PineSprigAlpha) is >=
  // 2*(kStemHalfWidth - edge/2) = 2*(0.04 - 0.0078125) = 0.0644 wide around
  // u=0 for EVERY row t (no t-dependence on the stem term), so any lattice
  // sample landing within |u_tex| <= 0.0322 is guaranteed alpha=255
  // regardless of row. Pin the card to a single sample column (width <
  // d=cell_size/3 forces nu=1, so every sample's u fraction is exactly 0.5
  // -- the texture's center column, u_tex~=0.0078, safely inside that band)
  // so every lattice sample is guaranteed-opaque without hand-predicting the
  // (procedural) sprig stamp layout the other silhouettes use.
  StaticTexturedMeshComponent leaf_mesh;
  const float x0 = 0.02f, x1 = 0.03f;  // width 0.01 (< d=0.05 -> nu=1)
  const float y0 = 0.0f, y1 = 0.43f;   // height 0.43 (-> nv=9, 3 samples/cell)
  const float z = 0.02f;               // flat card; not a cell_size multiple
  PushVertex(leaf_mesh.vertices, {x0, y1, z}, {0, 1}, {0, 0, 1}, {1, 0, 0});  // v0 top-left
  PushVertex(leaf_mesh.vertices, {x0, y0, z}, {0, 0}, {0, 0, 1}, {1, 0, 0});  // v1 bottom-left
  PushVertex(leaf_mesh.vertices, {x1, y0, z}, {1, 0}, {0, 0, 1}, {1, 0, 0});  // v2 bottom-right
  PushVertex(leaf_mesh.vertices, {x1, y1, z}, {1, 1}, {0, 0, 1}, {1, 0, 0});  // v3 top-right
  leaf_mesh.vertex_count = 4;
  leaf_mesh.indices = {0, 1, 2, 0, 2, 3};

  LeafVoxelizeOptions opts;
  opts.cell_size = 0.15f;
  opts.occupancy_fraction = 0.001f;  // this card only fills a slice of each cell
  opts.coverage_texture_size = 128;

  const LeafVoxelGrid grid = SplatLeafCards(leaf_mesh, LeafSilhouette::PineSprig, opts);
  REQUIRE(grid.dims == glm::ivec3(1, 3, 1));
  REQUIRE(grid.cells.size() == 3u);

  const float dA = (0.01f / 1.0f) * (0.43f / 9.0f);  // nu=1, nv=9 (see comment above)
  const float expected_area = 3.0f * dA;             // 3 samples/cell land per cell, alpha=1 each

  for (int cy = 0; cy < 3; ++cy) {
    INFO("cell y=" << cy);
    const auto it = grid.cells.find(PackCellKey(glm::ivec3(0, cy, 0)));
    REQUIRE(it != grid.cells.end());
    REQUIRE(it->second.area_alpha == Catch::Approx(expected_area).epsilon(1e-4));
    // PineSprig sets RGB=leaf_color(=vec3(1)) everywhere -- gray=1 always.
    REQUIRE(it->second.area_gray == Catch::Approx(it->second.area_alpha).epsilon(1e-6));
    REQUIRE(it->second.area_axis.x == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(it->second.area_axis.y == Catch::Approx(expected_area).epsilon(1e-4));
    REQUIRE(it->second.area_axis.z == Catch::Approx(0.0f).margin(1e-6f));
  }

  // Occupied cells reliably emit occupied tets (dims=(1,3,1) leaves no other
  // possible cell for anything to leak into).
  const TexturedMeshResult r = EmitTetMesh(grid, opts);
  REQUIRE(r.mesh.vertex_count == 12u);  // 3 occupied cells * 4 verts/tet
}

TEST_CASE("SplatLeafCards / EmitTetMesh: empty input produces empty output") {
  const StaticTexturedMeshComponent empty_mesh;  // vertex_count=0, indices empty
  const LeafVoxelizeOptions opts;

  const LeafVoxelGrid grid = SplatLeafCards(empty_mesh, LeafSilhouette::Oak, opts);
  REQUIRE(grid.cells.empty());

  const TexturedMeshResult r = EmitTetMesh(grid, opts);
  REQUIRE(r.mesh.vertex_count == 0u);
  REQUIRE(r.mesh.indices.empty());
}

TEST_CASE("SplatLeafCards: fails loudly (empty grid) past the 512-cells-per-axis guard") {
  StaticTexturedMeshComponent leaf_mesh;
  const float hw = 39.0f;  // width 78 -> 78/0.15 = 520 cells, > 512
  const float z = 0.02f;   // flat card; not a cell_size multiple (see the hand-built-card test)
  PushVertex(leaf_mesh.vertices, {-hw, 0.1f, z}, {0, 1}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {-hw, 0.0f, z}, {0, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {hw, 0.0f, z}, {1, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {hw, 0.1f, z}, {1, 1}, {0, 0, 1}, {1, 0, 0});
  leaf_mesh.vertex_count = 4;
  leaf_mesh.indices = {0, 1, 2, 0, 2, 3};

  const LeafVoxelizeOptions opts;  // default cell_size=0.15
  const LeafVoxelGrid grid = SplatLeafCards(leaf_mesh, LeafSilhouette::Oak, opts);
  REQUIRE(grid.dims.x > 512);
  REQUIRE(grid.cells.empty());
}

TEST_CASE("EmitTetMesh: axis falls back to (cell - aabb center) when area_axis is zero") {
  LeafVoxelGrid grid;
  grid.origin = glm::vec3(0.0f);
  grid.cell_size = 1.0f;
  grid.dims = glm::ivec3(3, 1, 1);  // aabb_center = (1.5, 0.5, 0.5)
  // Cell (0,0,0) -> cell_center=(0.5,0.5,0.5) -> fallback axis normalize(-1,0,0).
  grid.cells[PackCellKey(glm::ivec3(0, 0, 0))] = {
      .area_alpha = 1.0f, .area_gray = 0.5f, .area_axis = glm::vec3(0.0f)};

  LeafVoxelizeOptions opts;
  opts.occupancy_fraction = 0.1f;  // threshold 0.1 < area_alpha 1.0 -> occupied
  opts.axis_jitter = 0.0f;         // isolate the fallback direction, no perturbation
  opts.brightness_jitter = 0.0f;   // isolate the brightness formula

  const TexturedMeshResult r = EmitTetMesh(grid, opts);
  REQUIRE(r.mesh.vertex_count == 4u);
  for (uint32_t v = 0; v < 4; ++v) {
    const glm::vec3 n = VertexNormal(r.mesh, v);
    REQUIRE(n.x == Catch::Approx(-1.0f).margin(1e-5f));
    REQUIRE(n.y == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(n.z == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(VertexUv(r.mesh, v).x == Catch::Approx(0.5f).margin(1e-5f));  // area_gray/area_alpha
  }
}

TEST_CASE("VoxelizeLeafCards: every TreeCatalog preset voxelizes at 3 cell sizes") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == 15u);

  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    const TexturedMeshResult leaves = GenerateLeafMesh(setup.options);
    REQUIRE(leaves.mesh.vertex_count > 0u);

    uint32_t prev_tet_count = 0;
    for (int i = 0; i < 3; ++i) {
      LeafVoxelizeOptions opts;
      opts.cell_size = 0.15f * static_cast<float>(1 << i);  // 0.15, 0.30, 0.60
      INFO("cell_size=" << opts.cell_size);

      const TexturedMeshResult r =
          VoxelizeLeafCards(leaves.mesh, setup.options.leaves.silhouette, opts);
      const auto& m = r.mesh;
      REQUIRE(m.vertex_count > 0u);
      REQUIRE(m.vertex_count % 4u == 0u);
      REQUIRE(m.indices.size() == 3u * static_cast<size_t>(m.vertex_count));

      for (uint32_t v = 0; v < m.vertex_count; ++v) {
        const float u = VertexUv(m, v).x;
        CHECK(u >= 0.0f);
        CHECK(u <= 1.0f);
        const glm::vec3 p = VertexPos(m, v);
        CHECK(p.x >= r.local_bounds.min.x - 1e-4f);
        CHECK(p.x <= r.local_bounds.max.x + 1e-4f);
        CHECK(p.y >= r.local_bounds.min.y - 1e-4f);
        CHECK(p.y <= r.local_bounds.max.y + 1e-4f);
        CHECK(p.z >= r.local_bounds.min.z - 1e-4f);
        CHECK(p.z <= r.local_bounds.max.z + 1e-4f);
      }

      const uint32_t tet_count = m.vertex_count / 4u;
      if (i > 0) CHECK(tet_count <= prev_tet_count);  // coarser cells -> not more tets
      prev_tet_count = tet_count;
    }
  }
}
