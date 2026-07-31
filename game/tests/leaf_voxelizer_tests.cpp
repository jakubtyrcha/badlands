#include <catch_amalgamated.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>  // glm::translate, glm::scale
#include "game/geometry/leaf_voxelizer.hpp"
#include "game/geometry/mesh_lod.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/tree_options.hpp"
#include "game/visual/foliage_voxel_config.hpp"
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

// Runs `fn` with the default logger's sinks swapped for a ringbuffer_sink
// (saved/restored around the call so this doesn't leak into any other test),
// returning the raw messages (level + payload, no pattern formatting needed)
// recorded during the call.
std::vector<spdlog::details::log_msg_buffer> CaptureLogMessages(
    const std::function<void()>& fn) {
  auto ring_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64);
  std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
  const std::vector<spdlog::sink_ptr> saved_sinks = logger->sinks();
  logger->sinks() = {ring_sink};
  fn();
  std::vector<spdlog::details::log_msg_buffer> messages = ring_sink->last_raw();
  logger->sinks() = saved_sinks;
  return messages;
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

  // Pins the Phase 3 review fix (formerly exercised via Pine's real
  // empty-crown case, now that Phase 6 retuned kFoliageVoxelWorldSizes so no
  // TreeCatalog preset voxelizes empty any more -- see the sane-tet-count-
  // band test above): model_viewer_view.cpp's voxel-mode world_bounds union
  // must skip an empty crown. r.local_bounds is Aabb::Empty() (min=+FLT_MAX,
  // max=-FLT_MAX sentinel corners) here, and TransformedBy would smear those
  // sentinels through xf into world_bounds if unioned unguarded, corrupting
  // orbit-camera framing. Exercise the SAME vertex_count>0 guard the viewer
  // applies, against an arbitrary non-trivial transform.
  const glm::mat4 xf = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, -2.0f)) *
                       glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
  const Aabb bark_world_bounds =
      Aabb::FromMinMax(glm::vec3(-1.0f), glm::vec3(1.0f)).TransformedBy(xf);

  Aabb guarded = bark_world_bounds;
  if (r.mesh.vertex_count > 0) {
    guarded = guarded.Union(r.local_bounds.TransformedBy(xf));
  }
  CHECK(guarded.min == bark_world_bounds.min);
  CHECK(guarded.max == bark_world_bounds.max);

  // Contrast: the same union WITHOUT the guard corrupts world_bounds with
  // the Aabb::Empty() sentinel's transformed garbage -- proves the guard is
  // load-bearing, not defensive dead code.
  const Aabb unguarded = bark_world_bounds.Union(r.local_bounds.TransformedBy(xf));
  const bool corrupted = std::abs(unguarded.min.x) > 1e6f || std::abs(unguarded.min.y) > 1e6f ||
                         std::abs(unguarded.min.z) > 1e6f || std::abs(unguarded.max.x) > 1e6f ||
                         std::abs(unguarded.max.y) > 1e6f || std::abs(unguarded.max.z) > 1e6f;
  CHECK(corrupted);
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

TEST_CASE("SplatLeafCards: >512-axis guard fires even when another axis is degenerate") {
  // Combines the two early-out guards in one mesh: x needs 520 cells (>512,
  // must fail loudly) while z sits EXACTLY on a cell_size multiple (dims.z
  // computes to 0, the "nothing to voxelize, not an error" degenerate case).
  // The >512 check must win regardless of check order -- if the degenerate
  // check ran first and returned early, it would zero out ALL of dims
  // (including the legitimately oversized x) and silently swallow the
  // failure with no spdlog::error, which is exactly the bug this pins.
  StaticTexturedMeshComponent leaf_mesh;
  const float hw = 39.0f;  // width 78 -> 78/0.15 = 520 cells, > 512
  const float z = 0.0f;    // exactly a cell_size multiple -> dims.z degenerates to 0
  PushVertex(leaf_mesh.vertices, {-hw, 0.1f, z}, {0, 1}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {-hw, 0.0f, z}, {0, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {hw, 0.0f, z}, {1, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {hw, 0.1f, z}, {1, 1}, {0, 0, 1}, {1, 0, 0});
  leaf_mesh.vertex_count = 4;
  leaf_mesh.indices = {0, 1, 2, 0, 2, 3};

  const LeafVoxelizeOptions opts;  // default cell_size=0.15
  const LeafVoxelGrid grid = SplatLeafCards(leaf_mesh, LeafSilhouette::Oak, opts);

  // dims must still carry its real computed values (not zeroed to (0,0,0))
  // -- proof the >512 branch returned first, not the degenerate branch.
  REQUIRE(grid.dims.x > 512);
  REQUIRE(grid.dims.z == 0);  // confirms the flat axis really was degenerate too
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

TEST_CASE(
    "VoxelizeLeafCards: every TreeCatalog preset stays in a sane tet-count "
    "band at the viewer's 3 native cell sizes") {
  // Mirrors model_viewer_view.cpp's per-tree native cell-size derivation
  // (kFoliageVoxelWorldSizes[lod_level_ - 1] / s, s = kFoliagePreviewHeight /
  // bark_height) -- kFoliageVoxelWorldSizes/kFoliagePreviewHeight now come
  // from the shared game/visual/foliage_voxel_config.hpp (see the include
  // above) rather than a local mirror, so a retune there is tracked here
  // automatically. Unlike the flat 0.15/0.30/0.60-NATIVE-units test above,
  // this uses the tree's own preview-rescaled cell size, which varies a lot
  // by preset since `s` depends on the preset's own native height -- e.g. a
  // Bush's L0 native cell size is ~0.23 while an Aspen (large)'s is ~1.86.
  // Generous headroom over the largest count measured across all 15 presets
  // x 3 levels (Oak (large) L0, ~11.7k tets) -- this bound exists to catch a
  // real order-of-magnitude blowup (e.g. a stray cell_size/rescale mixup),
  // not to be a tight fit.
  constexpr uint32_t kMaxSaneTetCount = 50000u;

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == 15u);

  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);
    const TexturedMeshResult bark = GenerateTreeMesh(setup.options, skeleton);
    const float bark_height = bark.local_bounds.max.y - bark.local_bounds.min.y;
    const float s = kFoliagePreviewHeight / std::max(bark_height, 0.001f);
    const TexturedMeshResult leaves = GenerateLeafMesh(setup.options, skeleton);
    REQUIRE(leaves.mesh.vertex_count > 0u);

    for (int level = 0; level < 3; ++level) {
      LeafVoxelizeOptions opts;
      opts.cell_size = kFoliageVoxelWorldSizes[static_cast<size_t>(level)] / s;
      INFO("level=" << level << " native cell_size=" << opts.cell_size);

      const TexturedMeshResult r =
          VoxelizeLeafCards(leaves.mesh, setup.options.leaves.silhouette, opts);
      const uint32_t tet_count = r.mesh.vertex_count / 4u;
      INFO("tet_count=" << tet_count);
      CHECK(tet_count <= kMaxSaneTetCount);

      // Phase 6 MUST-FIX: "Pine (medium)"/"Pine (large)" used to voxelize to
      // a genuinely EMPTY crown at Voxel-L1's old 0.30 world size (a
      // SplatLeafCards lattice-sampling aliasing dead zone, not an
      // occupancy_fraction threshold issue -- area_alpha was exactly 0, not
      // merely below occ_threshold; see model_viewer_view.cpp's
      // kFoliageVoxelWorldSizes comment). Retuning L1 to 0.20 clears it for
      // all 15 presets x 3 levels, so every combination is now asserted
      // non-empty -- a real regression (this gap recurring, or a new one)
      // fails loudly here.
      CHECK(tet_count > 0u);
    }
  }
}

// ===========================================================================
// Review fix: SplatLeafCards cast `std::ceil(span/h)` to `int` BEFORE the
// >512-cells-per-axis guard ran. span/h overflowing int range (or being
// NaN/+-inf) makes that cast undefined behavior. On this toolchain/arch
// (arm64 clang, saturating float->int conversion) an overflowing-positive
// cast happens to saturate to INT_MAX, which the pre-existing `dims > 512`
// check still (accidentally) caught -- but a NaN or -inf source (see the
// second test below) casts to something that is NOT > 512, so it falls
// through into the "degenerate, nothing to voxelize" early-out a few lines
// down and silently returns an empty grid with NO error logged. The fix
// computes the cell counts in float FIRST and gates on
// `v >= INT_MIN_as_float && v <= 512.0f` (catching NaN, +inf, -inf, and any
// out-of-int-range magnitude in either direction) before ever casting to int.
//
// A single garbage/NaN VERTEX coordinate (as the review brief's own wording
// suggested for this test) turns out not to reach this code path at all on
// this codebase's Aabb: ComputeLocalAabb accumulates via glm::min/max, and
// glm::min(acc, NaN) == acc (glm's comparison-based min/max never lets a NaN
// operand win), so a single NaN vertex is silently dropped from the AABB
// rather than propagated -- confirmed empirically (that construction stays
// green pre-fix, not red). The tests below instead drive NaN/-inf through
// `cell_size` (used as a raw divisor, no min/max filtering) and through an
// ENTIRE axis of non-finite vertices (leaves that axis's Aabb component at
// its Aabb::Empty() sentinel, +-FLT_MAX, whose own floor(.../h)*h can then
// overflow to +-inf) -- both are real, reachable ways this UB was hit.
// ===========================================================================

TEST_CASE("SplatLeafCards: fails loudly (empty grid) when a vertex coordinate "
          "would overflow the int cast, not just silently degenerate") {
  // A flat card, one edge pushed out to a coordinate so large that
  // ceil(span/cell_size) overflows int range -- the pre-fix cast UB case.
  // NOTE: on this arm64/clang build, the overflowing cast happens to
  // saturate to INT_MAX (not wrap negative), so the PRE-fix code already
  // passed this specific case (dims.x == INT_MAX > 512 still logs the
  // existing >512 error) -- this test is not RED on this platform, but it is
  // the exact scenario the review brief calls out and stays a meaningful
  // regression pin post-fix (see the NaN/-inf tests below for the cases that
  // WERE observed red here).
  StaticTexturedMeshComponent leaf_mesh;
  constexpr float kHuge = 1e30f;
  const float z = 0.02f;  // flat card; not a cell_size multiple
  PushVertex(leaf_mesh.vertices, {0.0f, 0.1f, z}, {0, 1}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {0.0f, 0.0f, z}, {0, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {kHuge, 0.0f, z}, {1, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {kHuge, 0.1f, z}, {1, 1}, {0, 0, 1}, {1, 0, 0});
  leaf_mesh.vertex_count = 4;
  leaf_mesh.indices = {0, 1, 2, 0, 2, 3};

  const LeafVoxelizeOptions opts;  // default cell_size=0.15
  LeafVoxelGrid grid;
  const std::vector<spdlog::details::log_msg_buffer> messages =
      CaptureLogMessages([&] {
        grid = SplatLeafCards(leaf_mesh, LeafSilhouette::Oak, opts);
      });

  CHECK(grid.cells.empty());
  const bool logged_error =
      std::any_of(messages.begin(), messages.end(),
                 [](const spdlog::details::log_msg_buffer& m) {
                   return m.level == spdlog::level::err;
                 });
  INFO("captured " << messages.size() << " log message(s), dims=("
                   << grid.dims.x << "," << grid.dims.y << "," << grid.dims.z
                   << ")");
  CHECK(logged_error);
}

TEST_CASE("SplatLeafCards: fails loudly (empty grid) when cell_size is NaN") {
  // cell_size is used as a raw divisor throughout (no NaN-filtering min/max
  // in the way) -- a NaN cell_size makes origin/span/span_cells all NaN,
  // directly exercising the guard's NaN branch. REAL-world path this
  // mirrors: LeafVoxelizeOptions::cell_size is caller-derived (e.g.
  // model_viewer_view.cpp's `kFoliageVoxelWorldSizes[lod] / s`) and a
  // degenerate `s` (e.g. from a zero-height bark mesh) would feed NaN/inf in
  // here exactly like this.
  StaticTexturedMeshComponent leaf_mesh;
  const float z = 0.02f;
  PushVertex(leaf_mesh.vertices, {0.0f, 0.1f, z}, {0, 1}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {0.0f, 0.0f, z}, {0, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {0.03f, 0.0f, z}, {1, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {0.03f, 0.1f, z}, {1, 1}, {0, 0, 1}, {1, 0, 0});
  leaf_mesh.vertex_count = 4;
  leaf_mesh.indices = {0, 1, 2, 0, 2, 3};

  LeafVoxelizeOptions opts;
  opts.cell_size = std::numeric_limits<float>::quiet_NaN();

  LeafVoxelGrid grid;
  const std::vector<spdlog::details::log_msg_buffer> messages =
      CaptureLogMessages([&] {
        grid = SplatLeafCards(leaf_mesh, LeafSilhouette::Oak, opts);
      });

  CHECK(grid.cells.empty());
  const bool logged_error =
      std::any_of(messages.begin(), messages.end(),
                 [](const spdlog::details::log_msg_buffer& m) {
                   return m.level == spdlog::level::err;
                 });
  INFO("captured " << messages.size() << " log message(s), dims=("
                   << grid.dims.x << "," << grid.dims.y << "," << grid.dims.z
                   << ")");
  CHECK(logged_error);
}

TEST_CASE("SplatLeafCards: fails loudly (empty grid) when an entire axis is "
          "non-finite (Aabb::Empty() sentinel -> -inf span, not just +inf)") {
  // Every vertex's x-coordinate is NaN: glm::min/max never let a NaN operand
  // win (see the section comment above), so aabb.min.x/aabb.max.x are left at
  // their Aabb::Empty() sentinel (+FLT_MAX / -FLT_MAX). grid.origin.x's
  // floor(FLT_MAX/h)*h then itself overflows to +inf, making span.x (and so
  // span_cells.x) land at -inf -- NOT caught by a naive `!(x <= 512.0f)`
  // guard (-inf <= 512.0f is true), which is why the fix's guard checks
  // `>= INT_MIN` too, not only `<= 512.0f`.
  StaticTexturedMeshComponent leaf_mesh;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  PushVertex(leaf_mesh.vertices, {nan, 0.1f, 0.02f}, {0, 1}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {nan, 0.0f, 0.02f}, {0, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {nan, 0.0f, 0.03f}, {1, 0}, {0, 0, 1}, {1, 0, 0});
  PushVertex(leaf_mesh.vertices, {nan, 0.1f, 0.03f}, {1, 1}, {0, 0, 1}, {1, 0, 0});
  leaf_mesh.vertex_count = 4;
  leaf_mesh.indices = {0, 1, 2, 0, 2, 3};

  const LeafVoxelizeOptions opts;  // default cell_size=0.15
  LeafVoxelGrid grid;
  const std::vector<spdlog::details::log_msg_buffer> messages =
      CaptureLogMessages([&] {
        grid = SplatLeafCards(leaf_mesh, LeafSilhouette::Oak, opts);
      });

  CHECK(grid.cells.empty());
  const bool logged_error =
      std::any_of(messages.begin(), messages.end(),
                 [](const spdlog::details::log_msg_buffer& m) {
                   return m.level == spdlog::level::err;
                 });
  INFO("captured " << messages.size() << " log message(s), dims=("
                   << grid.dims.x << "," << grid.dims.y << "," << grid.dims.z
                   << ")");
  CHECK(logged_error);
}

// ===========================================================================
// The coarse tail of the LOD chain (L3, the last shipped level). These pin
// what the tail exists to deliver -- every preset still HAS a crown (no
// dead-zone empties, the Phase 6 failure mode), and the whole tree costs a
// bounded, small number of triangles AND vertices -- against a retune of
// kFoliageVoxelWorldSizes / kFoliageCoarseBarkTriBudgets or a change in the
// simplifier's behavior. Written against "the last entry in the chain" rather
// than a level number, so shortening or extending the chain re-points them.
// ===========================================================================

TEST_CASE("Voxel L3 (the coarsest level): every TreeCatalog preset's whole "
          "tree stays within the level's triangle band") {
  // Measured range at the shipped tuning is ~230-680 triangles across the 15
  // presets (crown 56-428 + bark 158-255). This bound has headroom over that
  // spread: it exists to catch an order-of-magnitude regression (a cell-size
  // or budget mixup), not to be a tight fit.
  constexpr size_t kMaxTotalTris = 900;
  constexpr size_t kCoarsest = kFoliageVoxelWorldSizes.size() - 1;

  for (const NamedTreeOptions& setup : TreeCatalog()) {
    INFO("setup: " << setup.name);
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);
    TexturedMeshResult bark = GenerateTreeMesh(setup.options, skeleton);
    const float bark_height = bark.local_bounds.max.y - bark.local_bounds.min.y;
    const float s = kFoliagePreviewHeight / std::max(bark_height, 0.001f);
    const TexturedMeshResult leaves = GenerateLeafMesh(setup.options, skeleton);

    // Mirrors model_viewer_view.cpp's single-tree path: bark through the
    // shared per-level policy, leaves through the voxelizer at this level's
    // preview-space cell size converted to native units.
    SimplifyBarkForVoxelLod(bark.mesh, kCoarsest);
    LeafVoxelizeOptions opts;
    opts.cell_size = kFoliageVoxelWorldSizes[kCoarsest] / s;
    const TexturedMeshResult crown =
        VoxelizeLeafCards(leaves.mesh, setup.options.leaves.silhouette, opts);

    const size_t bark_tris = bark.mesh.indices.size() / 3;
    const size_t crown_tris = crown.mesh.indices.size() / 3;
    INFO("bark=" << bark_tris << " crown=" << crown_tris
                 << " bark_verts=" << bark.mesh.vertex_count);

    // The decimated bark must carry a vertex buffer sized to the triangles it
    // kept, not the full welded set it was decimated FROM (a triangle can
    // reference at most 3 distinct vertices, and a real mesh shares many).
    // Pre-compaction this was ~6000 vertices behind a couple hundred
    // triangles -- the triangle budget alone would have looked cheap while
    // the GPU upload stayed enormous.
    CHECK(bark.mesh.vertex_count <= 3 * bark_tris);

    // A crown that voxelized to nothing would make the bound trivially easy
    // to hit AND leave the tree a bare trunk -- check it separately.
    CHECK(crown_tris > 0);
    CHECK(bark_tris > 0);
    CHECK(bark_tris + crown_tris < kMaxTotalTris);
  }
}

TEST_CASE("Voxel LODs: every level stays non-empty and no coarser than the "
          "level before it") {
  // Guards the chain's monotonicity end to end (the earlier per-preset test
  // covers levels 0-2 at flat native cell sizes; this one runs every shipped
  // level at the viewer's own preview-space derivation) and re-checks the
  // Phase 6 empty-crown failure mode at the coarse sizes, where the occupancy
  // threshold (occupancy_fraction * cell^2) is largest.
  for (const NamedTreeOptions& setup : TreeCatalog()) {
    INFO("setup: " << setup.name);
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);
    const TexturedMeshResult bark = GenerateTreeMesh(setup.options, skeleton);
    const float bark_height = bark.local_bounds.max.y - bark.local_bounds.min.y;
    const float s = kFoliagePreviewHeight / std::max(bark_height, 0.001f);
    const TexturedMeshResult leaves = GenerateLeafMesh(setup.options, skeleton);

    size_t prev_tris = std::numeric_limits<size_t>::max();
    for (size_t level = 0; level < kFoliageVoxelWorldSizes.size(); ++level) {
      LeafVoxelizeOptions opts;
      opts.cell_size = kFoliageVoxelWorldSizes[level] / s;
      const TexturedMeshResult r =
          VoxelizeLeafCards(leaves.mesh, setup.options.leaves.silhouette, opts);
      const size_t tris = r.mesh.indices.size() / 3;
      INFO("level=" << level << " tris=" << tris << " prev=" << prev_tris);
      CHECK(tris > 0);
      CHECK(tris <= prev_tris);
      prev_tris = tris;
    }
  }
}
