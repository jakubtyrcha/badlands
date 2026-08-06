// Catch2 suite for the screen-space LOD ladder (game/visual/lod_screen_space.hpp).
//
// Pure math, no GPU. What matters here is not the exact numbers -- those are
// tunable by eye -- but the STRUCTURAL guarantees the field builder and
// GpuInstanceRenderer depend on: strictly ascending cutoffs, monotonically
// falling budgets, and a chain that stays valid once the impostor's own cutoff
// is appended to it.

#include <catch_amalgamated.hpp>

#include "engine/rendering/gpu_instance_renderer.hpp"
#include "game/visual/lod_screen_space.hpp"

using namespace badlands;

TEST_CASE("the ladder's cutoffs ascend strictly and its budgets fall",
          "[lod_ladder]") {
  // A boulder-sized, boulder-dense model.
  const LodLadder ladder = BuildLodLadder(/*size_m=*/2.0f, /*source_tris=*/66122);

  REQUIRE_FALSE(ladder.triangle_budgets.empty());
  REQUIRE(ladder.thresholds.size() == ladder.triangle_budgets.size());
  REQUIRE(ladder.mesh_level_count() == ladder.triangle_budgets.size() + 1);

  for (size_t i = 1; i < ladder.thresholds.size(); ++i) {
    CAPTURE(i, ladder.thresholds[i - 1], ladder.thresholds[i]);
    // Strictly ascending, not merely sorted: GpuInstanceRenderer treats an
    // equal pair as malformed, since the level between them is unreachable.
    CHECK(ladder.thresholds[i] > ladder.thresholds[i - 1]);
  }
  for (size_t i = 1; i < ladder.triangle_budgets.size(); ++i) {
    CAPTURE(i);
    CHECK(ladder.triangle_budgets[i] < ladder.triangle_budgets[i - 1]);
  }
  // A coarser level that kept the source's triangle count would be a level
  // that costs a draw call and saves nothing.
  CHECK(ladder.triangle_budgets.front() < 66122);
}

TEST_CASE("appending the impostor cutoff keeps the chain ascending",
          "[lod_ladder]") {
  // This is the property the whole stop rule exists for: the field builder
  // appends impostor_threshold_m to `thresholds`, and the result still has to
  // satisfy GpuInstanceRenderer's strictly-ascending validation.
  const float sizes[] = {0.3f, 0.9f, 2.0f, 8.0f, 25.0f};
  const size_t tri_counts[] = {376, 2612, 16548, 66122, 103000};

  for (float size_m : sizes) {
    for (size_t tris : tri_counts) {
      CAPTURE(size_m, tris);
      const LodLadder ladder = BuildLodLadder(size_m, tris);
      REQUIRE(ladder.impostor_threshold_m > 0.0f);
      if (!ladder.thresholds.empty()) {
        CHECK(ladder.impostor_threshold_m > ladder.thresholds.back());
      }
      // Total levels including the impostor must fit the engine's cap.
      CHECK(ladder.mesh_level_count() + 1 <= GpuInstanceRenderer::kMaxLods);
    }
  }
}

TEST_CASE("the ladder adapts its LENGTH to the model, not just its numbers",
          "[lod_ladder]") {
  // The point of deriving rather than tabulating: a dense model earns more
  // levels than a sparse one of the same size, because its source mesh stops
  // being needed much nearer.
  const LodLadder dense = BuildLodLadder(/*size_m=*/2.0f, /*source_tris=*/66122);
  const LodLadder sparse = BuildLodLadder(/*size_m=*/2.0f, /*source_tris=*/400);

  CHECK(dense.mesh_level_count() > sparse.mesh_level_count());
  // Same size, so the impostor takes over at the same distance regardless of
  // density -- it is bounded by the atlas' view count, not by the mesh.
  CHECK(dense.impostor_threshold_m == Catch::Approx(sparse.impostor_threshold_m));
}

TEST_CASE("a bigger model holds its detail further out", "[lod_ladder]") {
  const LodLadder small = BuildLodLadder(/*size_m=*/1.0f, /*source_tris=*/20000);
  const LodLadder large = BuildLodLadder(/*size_m=*/8.0f, /*source_tris=*/20000);

  REQUIRE_FALSE(small.thresholds.empty());
  REQUIRE_FALSE(large.thresholds.empty());
  // Screen coverage scales with size/distance, so an 8x model switches 8x
  // further out for the same on-screen triangle density.
  CHECK(large.thresholds.front() ==
        Catch::Approx(small.thresholds.front() * 8.0f).epsilon(0.001));
  CHECK(large.impostor_threshold_m ==
        Catch::Approx(small.impostor_threshold_m * 8.0f).epsilon(0.001));
}

TEST_CASE("budgets never fall below the floor", "[lod_ladder]") {
  LodLadderOptions opts;
  opts.min_triangles = 500;
  const LodLadder ladder = BuildLodLadder(2.0f, 66122, opts);
  for (int budget : ladder.triangle_budgets) {
    CHECK(budget >= opts.min_triangles);
  }
}

TEST_CASE("a model the impostor swallows immediately gets no mesh levels",
          "[lod_ladder]") {
  // Tiny and sparse: the source already meets the density target so close in
  // that the first coarser level would sit past the impostor's own cutoff.
  // The correct answer is a one-level chain with NO cutoffs -- the field
  // builder then appends the impostor's, giving two levels and one cutoff.
  const LodLadder ladder = BuildLodLadder(/*size_m=*/0.1f, /*source_tris=*/12);

  CHECK(ladder.triangle_budgets.empty());
  CHECK(ladder.thresholds.empty());
  CHECK(ladder.mesh_level_count() == 1);
  CHECK(ladder.impostor_threshold_m > 0.0f);
}

TEST_CASE("degenerate inputs return a valid one-level chain", "[lod_ladder]") {
  for (const LodLadder& ladder :
       {BuildLodLadder(0.0f, 5000), BuildLodLadder(2.0f, 0),
        BuildLodLadder(-1.0f, 5000)}) {
    CHECK(ladder.mesh_level_count() == 1);
    CHECK(ladder.thresholds.empty());
    CHECK(ladder.triangle_budgets.empty());
  }
}

TEST_CASE("max_mesh_levels leaves room for the impostor", "[lod_ladder]") {
  LodLadderOptions opts;
  opts.min_triangles = 1;  // remove the floor so only the cap can stop it
  opts.impostor_size_ratio = 1e6f;  // and push the impostor out of reach
  const LodLadder ladder = BuildLodLadder(2.0f, 4'000'000, opts);

  CHECK(ladder.mesh_level_count() <= opts.max_mesh_levels);
  // The impostor still fits on top -- the whole reason max_mesh_levels sits one
  // below kMaxLods rather than at it.
  CHECK(ladder.mesh_level_count() + 1 <= GpuInstanceRenderer::kMaxLods);
}
