#include <catch_amalgamated.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <glm/gtc/constants.hpp>  // glm::pi
#include "game/geometry/tree_options.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/leaf_texture.hpp"
#include "game/geometry/mesh_lod.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

using namespace badlands;

TEST_CASE("OakPreset and PinePreset carry the expected ez-tree values") {
  const TreeOptions oak = OakPreset();
  REQUIRE(oak.seed == 35729u);
  REQUIRE(oak.type == TreeType::Deciduous);
  REQUIRE(oak.levels == 3);
  REQUIRE(oak.children[0] == 6);
  REQUIRE(oak.sections[0] == 8);

  const TreeOptions pine = PinePreset();
  REQUIRE(pine.seed == 13977u);
  REQUIRE(pine.type == TreeType::Evergreen);
  REQUIRE(pine.levels == 1);
  REQUIRE(pine.children[0] == 82);
}

TEST_CASE("BuildTreeSkeleton: deterministic branch structure") {
  const auto oak = BuildTreeSkeleton(OakPreset());
  // Structure is fixed by recursion (1 continuation + children[level] per branch):
  // L0=1, L1=7, L2=35, L3=140 -> 183.
  REQUIRE(oak.size() == 183u);
  // Evergreen: trunk + children[0] radial, no continuation.
  const auto pine = BuildTreeSkeleton(PinePreset());
  REQUIRE(pine.size() == 83u);
}

namespace {

// FNV-1a over the skeleton's GEOMETRY only -- origin, orientation, radius per
// section, plus level/base_radius/segment_count per branch. Deliberately does
// NOT cover the parentage fields (parent/attach_section/attach_alpha/
// is_continuation/base_arc_len): those are bookkeeping the bark grafter reads,
// and adding them must not move a single vertex. This is the guard that says so.
uint64_t HashSkeletonGeometry(const std::vector<SkeletonBranch>& skeleton) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const void* bytes, size_t n) {
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  };
  for (const SkeletonBranch& br : skeleton) {
    mix(&br.level, sizeof(br.level));
    mix(&br.base_radius, sizeof(br.base_radius));
    mix(&br.segment_count, sizeof(br.segment_count));
    for (const BranchSection& s : br.sections) {
      mix(&s.origin, sizeof(s.origin));
      mix(&s.orientation, sizeof(s.orientation));
      mix(&s.radius, sizeof(s.radius));
    }
  }
  return h;
}

// Connected-component count of a mesh AFTER the same weld SimplifyMesh does --
// meshopt_generateVertexRemap over the FULL vertex stride, so two vertices merge
// only if they are bit-identical in position, UV, normal AND tangent. That is
// the number meshoptimizer's edge collapse actually sees: it cannot merge
// separate components, so this is the mesh's decimation floor in disguise.
size_t WeldedComponentCount(const StaticTexturedMeshComponent& m) {
  constexpr size_t kStride = kTexturedMeshFloatsPerVertex;
  std::map<std::vector<float>, uint32_t> canonical;
  std::vector<uint32_t> weld(m.vertex_count);
  for (uint32_t v = 0; v < m.vertex_count; ++v) {
    std::vector<float> key(m.vertices.begin() + static_cast<long>(v * kStride),
                           m.vertices.begin() + static_cast<long>((v + 1) * kStride));
    weld[v] = canonical.emplace(std::move(key), static_cast<uint32_t>(canonical.size()))
                  .first->second;
  }

  std::vector<uint32_t> parent(canonical.size());
  for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
  std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  };
  auto unite = [&](uint32_t a, uint32_t b) {
    const uint32_t ra = find(a), rb = find(b);
    if (ra != rb) parent[ra] = rb;
  };
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    unite(weld[m.indices[i]], weld[m.indices[i + 1]]);
    unite(weld[m.indices[i]], weld[m.indices[i + 2]]);
  }

  std::set<uint32_t> roots;
  for (uint32_t idx : m.indices) roots.insert(find(weld[idx]));
  return roots.size();
}

}  // namespace

// The skeleton is load-bearing far beyond bark: leaves attach to its branches,
// voxel crowns come from those leaves, impostors are baked from both, and
// SilhouetteBounds -> CrownRadiusM feeds foliage SPACING. Any change that moves
// it silently relays out the forest. Pinned per preset so a failure names the
// one that drifted.
TEST_CASE("BuildTreeSkeleton: geometry is byte-stable across the catalog") {
  // Measured on the pre-graft generator; in TreeCatalog() order. A change here
  // is either a deliberate skeleton retune (update these and expect the forest
  // to re-lay-out) or a bug.
  static constexpr std::array<uint64_t, 15> kGolden = {
      0x90fa4e91c56a600aull,  // Oak (small)
      0xa7bb6890f479829full,  // Oak (medium)
      0xad2aa0919040487full,  // Oak (large)
      0x8f4bf7e178ab2adaull,  // Pine (small)
      0x31cf4ab13defa2b6ull,  // Pine (medium)
      0x08a812127241b485ull,  // Pine (large)
      0xf47c09bf67638ce5ull,  // Ash (small)
      0xfba671736feb9cd0ull,  // Ash (medium)
      0x6a8d48c32abf53a3ull,  // Ash (large)
      0xe525e934bea6ac1bull,  // Aspen (small)
      0xd39649251a045b35ull,  // Aspen (medium)
      0x02d6f74153600bddull,  // Aspen (large)
      0x7f0f7015cc993d68ull,  // Bush 1
      0x5d442c6390d6beb1ull,  // Bush 2
      0x2bc9c2e25183d4ddull,  // Bush 3
  };
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == kGolden.size());
  for (size_t i = 0; i < catalog.size(); ++i) {
    CAPTURE(catalog[i].name);
    CHECK(HashSkeletonGeometry(BuildTreeSkeleton(catalog[i].options)) == kGolden[i]);
  }
}

TEST_CASE("BuildTreeSkeleton: parentage is well-formed") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) {
    CAPTURE(setup.name);
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);

    // The trunk is the root, and it is index 0.
    REQUIRE(skeleton[0].parent == -1);
    REQUIRE_FALSE(skeleton[0].is_continuation);
    REQUIRE(skeleton[0].base_arc_len == 0.0f);

    int continuations = 0;
    for (size_t i = 1; i < skeleton.size(); ++i) {
      CAPTURE(i);
      const SkeletonBranch& br = skeleton[i];
      // Growth is a FIFO queue, so a parent is always recorded before its
      // children -- the grafter relies on this to build parents first.
      REQUIRE(br.parent >= 0);
      REQUIRE(br.parent < static_cast<int>(i));

      const SkeletonBranch& parent = skeleton[static_cast<size_t>(br.parent)];
      REQUIRE(br.attach_section >= 0);
      REQUIRE(br.attach_section < static_cast<int>(parent.sections.size()));
      REQUIRE(br.attach_alpha >= 0.0f);
      REQUIRE(br.attach_alpha <= 1.0f);
      REQUIRE(br.level == parent.level + 1);
      // V continues down the chain, so arc length never runs backwards.
      REQUIRE(br.base_arc_len >= parent.base_arc_len);
      if (br.is_continuation) {
        ++continuations;
        // A continuation starts at its parent's LAST section, which is what
        // makes its ring 0 coincident and therefore weldable.
        REQUIRE(br.attach_section == static_cast<int>(parent.sections.size()) - 1);
      }
    }

    // Evergreens have no stem continuation at all; deciduous trees get one per
    // branch below the terminal level.
    if (setup.options.type == TreeType::Evergreen) {
      REQUIRE(continuations == 0);
    } else {
      int below_terminal = 0;
      for (const SkeletonBranch& br : skeleton)
        if (br.level < setup.options.levels) ++below_terminal;
      REQUIRE(continuations == below_terminal);
    }
  }
}

// THE point of the whole graft. meshoptimizer's edge collapse cannot merge
// separate components, so bark used to floor at ~4 triangles per shell (~774
// for Oak (large), see mesh_lod.hpp). Every junction that stitches removes a
// shell; every one that falls back keeps its own. One shell plus one per
// fallback is therefore the exact expected count, and it is what makes the
// coarse LOD tail honest instead of clustered.
TEST_CASE("GenerateTreeMesh: the bark is one shell plus one per fallback") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) {
    BarkMeshStats stats;
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);
    const TexturedMeshResult r = GenerateTreeMesh(setup.options, skeleton, &stats);
    CAPTURE(setup.name, skeleton.size(), stats.junctions, stats.stitched,
            stats.shrunk, stats.fallback);
    CHECK(stats.junctions == stats.stitched + stats.fallback);
    CHECK(WeldedComponentCount(r.mesh) == static_cast<size_t>(1 + stats.fallback));
  }
}

// The measured fallback rate, pinned so a regression that silently stops
// stitching cannot pass as "still one shell plus fallbacks".
//
// Everything merges except three junctions on Bush 2, and those are the
// documented degenerate case: its `length[0]` is 0.1 over 3 sections, so the
// trunk they attach to is ~0.03 per section and has no surface worth cutting.
TEST_CASE("GenerateTreeMesh: fallbacks stay confined to the known hard cases") {
  // Measured, in TreeCatalog() order. A fallback means one branch stayed an
  // independent buried tube and still costs its own mesh component, so this is
  // the cap on how far bark can decimate. Asserted as an upper bound: getting
  // BETTER is fine, getting worse is a regression that must be looked at.
  //
  // All 14 of these come from quantization -- a socket whose rounded quad
  // rectangle collides with a sibling's, which MarkHole refuses rather than
  // tear a crack between two collars -- or from Bush 1/2's stub trunks, which
  // are ~0.05 long and have no surface worth cutting.
  static constexpr std::array<int, 15> kMaxFallback = {
      1,  // Oak (small)
      0,  // Oak (medium)
      0,  // Oak (large)
      0,  // Pine (small)
      2,  // Pine (medium)
      1,  // Pine (large)
      0,  // Ash (small)
      0,  // Ash (medium)
      0,  // Ash (large)
      0,  // Aspen (small)
      0,  // Aspen (medium)
      0,  // Aspen (large)
      3,  // Bush 1
      6,  // Bush 2  -- 0.1-long trunk over 3 sections
      1,  // Bush 3
  };
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == kMaxFallback.size());
  int total_junctions = 0, total_fallback = 0;
  for (size_t i = 0; i < catalog.size(); ++i) {
    BarkMeshStats stats;
    GenerateTreeMesh(catalog[i].options, BuildTreeSkeleton(catalog[i].options), &stats);
    CAPTURE(catalog[i].name, stats.junctions, stats.stitched, stats.fallback);
    REQUIRE(stats.junctions > 0);
    CHECK(stats.fallback <= kMaxFallback[i]);
    total_junctions += stats.junctions;
    total_fallback += stats.fallback;
  }
  // Under 1% of junctions across the whole catalog.
  CAPTURE(total_junctions, total_fallback);
  CHECK(total_fallback * 100 < total_junctions);
}

// What the shipped LOD chain actually asks of bark. SimplifyBarkForVoxelLod
// runs kDefaultLodRatios (0.5 at L1, 0.2 at L2) through error-bounded collapse,
// and an absolute 256-triangle budget through SimplifyMeshSloppy at L3 -- it
// never uses the very low ratios mesh_lod.hpp's ~774 floor note was measured at.
//
// Measured after grafting: L1 lands exactly on target for all 15 presets and L3
// stays inside its budget for all 15. L2 is exact for 9 of them; the deciduous
// presets with the deepest recursion run over, Bush 3 worst at ~2.1x.
//
// Connectivity did NOT remove meshoptimizer's deep-decimation floor, and it is
// worth knowing why before anyone tries: at ratio 0.05 every preset still floors
// (identical counts at 0.05, 0.01 and 0.005). Re-welding on POSITION ONLY --
// which closes the duplicated UV seam column that currently splits every tube
// into a bordered rectangle -- roughly halves that floor (Pine 1734 -> 815
// against a 680 target). That is a mesh_lod.cpp change and its own piece of work.
TEST_CASE("GenerateTreeMesh: bark meets the LOD chain's actual budgets") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) {
    CAPTURE(setup.name);
    const TexturedMeshResult r = GenerateTreeMesh(setup.options);
    const size_t src = r.mesh.indices.size() / 3;

    const size_t l1 = SimplifyMesh(r.mesh.vertices, kTexturedMeshFloatsPerVertex,
                                   r.mesh.indices, kDefaultLodRatios[1]).indices.size() / 3;
    const size_t l2 = SimplifyMesh(r.mesh.vertices, kTexturedMeshFloatsPerVertex,
                                   r.mesh.indices, kDefaultLodRatios[2]).indices.size() / 3;
    const size_t l3 = SimplifyMeshSloppy(r.mesh.vertices, kTexturedMeshFloatsPerVertex,
                                         r.mesh.indices,
                                         256.0f / static_cast<float>(src)).indices.size() / 3;
    CAPTURE(src, l1, l2, l3);

    // L1 is exact everywhere -- a miss here means collapse got blocked outright.
    CHECK(l1 <= static_cast<size_t>(kDefaultLodRatios[1] * src) + 8);
    // L2 overshoots on the deepest deciduous presets; 2.2x bounds the worst.
    CHECK(l2 <= static_cast<size_t>(2.2 * kDefaultLodRatios[2] * static_cast<double>(src)));
    // The coarse tail is a fixed budget, and clustering must still hit it.
    CHECK(l3 <= 256u);
  }
}

// THE spacing guarantee. Bark local_bounds flows through SilhouetteBounds ->
// CrownRadiusM -> ForestType::models[i].radius_m -> the sampler's sum-of-radii
// spacing rule, so a bark AABB that moved would silently re-lay-out the whole
// forest. The graft only ever replaces geometry BURIED inside a parent, so the
// swept skeleton's own extent is the reference the mesh has to match.
TEST_CASE("GenerateTreeMesh: grafting does not move the bark silhouette") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) {
    CAPTURE(setup.name);
    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);

    // Every ring vertex of every branch, exactly as the pre-graft generator
    // swept them -- computed from the skeleton, so it is independent of the
    // mesh builder under test.
    Aabb reference = Aabb::Empty();
    for (const SkeletonBranch& br : skeleton) {
      const int segments = std::max(3, br.segment_count);
      for (const BranchSection& sec : br.sections) {
        for (int j = 0; j < segments; ++j) {
          const float angle = glm::two_pi<float>() * static_cast<float>(j) /
                              static_cast<float>(segments);
          const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
          reference = reference.Union(
              Aabb{sec.origin + sec.orientation * (dir * sec.radius),
                   sec.origin + sec.orientation * (dir * sec.radius)});
        }
      }
    }

    const Aabb got = GenerateTreeMesh(setup.options, skeleton).local_bounds;
    const float scale = std::max(1.0f, glm::length(reference.max - reference.min));
    const float tol = 1e-3f * scale;

    // X and Z are the spacing guarantee and they are EXACT: CrownRadiusM takes
    // the largest half-extent about the trunk axis from these two alone, so a
    // drift here would move every tree in the forest.
    for (int axis : {0, 2}) {
      CAPTURE(axis);
      CHECK(got.min[axis] == Catch::Approx(reference.min[axis]).margin(tol));
      CHECK(got.max[axis] == Catch::Approx(reference.max[axis]).margin(tol));
    }

    // Y may only ever SHRINK, and only from below. A branch's base ring is
    // buried by definition -- its centre sits on the parent's axis -- so on a
    // preset whose trunk is shorter than a child's own radius, part of that
    // ring hangs out underneath and goes away with it. Only Bush 1 does this
    // (`length[0]` 0.1 over 6 sections and a 2x deciduous decay = a 0.05-tall
    // trunk against 0.58 radius), and it costs 0.068 of an 11-unit tree. Growth
    // in either direction is still a failure everywhere.
    const float height = std::max(1.0f, reference.max.y - reference.min.y);
    CHECK(got.max.y <= reference.max.y + tol);
    CHECK(got.min.y >= reference.min.y - tol);
    CHECK(got.max.y >= reference.max.y - tol);          // the crown never drops
    CHECK(got.min.y - reference.min.y <= 0.01f * height);
  }
}

TEST_CASE("GenerateTreeMesh: V is continuous and monotone along a stem chain") {
  const TreeOptions oak = OakPreset();
  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(oak);
  for (const SkeletonBranch& br : skeleton) {
    if (!br.is_continuation) continue;
    // A continuation's base V picks up exactly where its parent's tip left off.
    const SkeletonBranch& parent = skeleton[static_cast<size_t>(br.parent)];
    float parent_arc = 0.0f;
    for (size_t k = 1; k < parent.sections.size(); ++k)
      parent_arc += glm::length(parent.sections[k].origin - parent.sections[k - 1].origin);
    REQUIRE(br.base_arc_len == Catch::Approx(parent.base_arc_len + parent_arc));
  }
}

TEST_CASE("BuildTreeSkeleton: trunk rooted at origin, tapers, deterministic") {
  const auto a = BuildTreeSkeleton(OakPreset());
  const auto b = BuildTreeSkeleton(OakPreset());
  REQUIRE(a.size() == b.size());

  const SkeletonBranch& trunk = a[0];
  REQUIRE(trunk.sections.size() == static_cast<size_t>(OakPreset().sections[0] + 1));
  REQUIRE(glm::length(trunk.sections.front().origin) == Catch::Approx(0.0f));
  // Base wider than tip.
  REQUIRE(trunk.sections.front().radius > trunk.sections.back().radius);
  // Run-twice identical (determinism): compare a mid branch's first origin.
  REQUIRE(a[10].sections.front().origin.x == Catch::Approx(b[10].sections.front().origin.x));
  REQUIRE(a[10].sections.front().origin.y == Catch::Approx(b[10].sections.front().origin.y));
  REQUIRE(a[10].sections.front().origin.z == Catch::Approx(b[10].sections.front().origin.z));
}

TEST_CASE("GenerateTreeMesh: well-formed indexed mesh") {
  const TexturedMeshResult r = GenerateTreeMesh(OakPreset());
  const auto& m = r.mesh;
  REQUIRE(m.vertex_count > 0u);
  REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
  REQUIRE_FALSE(m.indices.empty());
  REQUIRE(m.indices.size() % 3 == 0);
  for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
  for (float f : m.vertices) REQUIRE(std::isfinite(f));
  // Base on floor, grows up.
  REQUIRE(r.local_bounds.min.y == Catch::Approx(0.0f).margin(0.05f));
  REQUIRE(r.local_bounds.max.y > 1.0f);
}

TEST_CASE("GenerateTreeMesh: deterministic, seed-sensitive") {
  const TexturedMeshResult a = GenerateTreeMesh(OakPreset());
  const TexturedMeshResult b = GenerateTreeMesh(OakPreset());
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);

  TreeOptions other = OakPreset();
  other.seed = 999u;
  const TexturedMeshResult c = GenerateTreeMesh(other);
  REQUIRE(c.mesh.vertices != a.mesh.vertices);  // different tree
}

// This used to pin Pine at exactly 6431 verts / 30096 indices, arithmetic that
// held only while every branch was an independent tube. Ring refinement adds
// rings where a socket would otherwise fall between two (Pine needs it: ~0.4-
// tall footprints against ~4.2-tall quads), so a closed-form count no longer
// exists. What is worth pinning is the SHAPE of the cost, not a magic number.
TEST_CASE("GenerateTreeMesh: refinement costs stay bounded (Pine)") {
  BarkMeshStats stats;
  const TreeOptions pine = PinePreset();
  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(pine);
  const TexturedMeshResult p = GenerateTreeMesh(pine, skeleton, &stats);

  // All but a couple of the trunk's 82 laterals are socketed; the exceptions
  // are sockets whose quantized quad rectangle collided with a sibling's.
  REQUIRE(stats.junctions == 82);
  REQUIRE(stats.fallback <= 2);

  // Refinement only touches the branch being cut into (the trunk), so the
  // vertex count must stay near the un-refined 6431 rather than scaling with
  // the whole tree. Two extra rings per socket on an 8-segment trunk is
  // 82 * 2 * 9 = 1476 at the absolute worst, before deduplication.
  CAPTURE(p.mesh.vertex_count);
  REQUIRE(p.mesh.vertex_count > 6431u);
  REQUIRE(p.mesh.vertex_count < 6431u + 1476u);

  // Collars add triangles; holes remove them. Neither may run away.
  const size_t tris = p.mesh.indices.size() / 3;
  CAPTURE(tris);
  REQUIRE(tris > 30096u / 3);
  REQUIRE(tris < 30096u);
}

TEST_CASE("TreeCatalog: every predefined setup generates a well-formed mesh") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == 15u);  // oak/pine/ash/aspen x3 sizes + 3 bushes

  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    REQUIRE_FALSE(setup.name.empty());

    const TexturedMeshResult r = GenerateTreeMesh(setup.options);
    const auto& m = r.mesh;
    REQUIRE(m.vertex_count > 0u);
    REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
    REQUIRE_FALSE(m.indices.empty());
    REQUIRE(m.indices.size() % 3 == 0);

    bool indices_in_range = true;
    for (uint32_t idx : m.indices)
      if (idx >= m.vertex_count) indices_in_range = false;
    REQUIRE(indices_in_range);

    bool all_finite = true;
    for (float f : m.vertices)
      if (!std::isfinite(f)) all_finite = false;
    REQUIRE(all_finite);

    const float height = r.local_bounds.max.y - r.local_bounds.min.y;
    REQUIRE(std::isfinite(height));
    REQUIRE(height > 0.0f);
  }
}

TEST_CASE("TreeCatalog presets bake with LeafSilhouetteBakeCutoff's cutoff") {
  // Guards against the viewer's leaf-texture cache (model_viewer_view.cpp)
  // and TreeCatalog's per-preset LeafOptions::alpha_cutoff drifting apart --
  // both must agree with the single shared LeafSilhouetteBakeCutoff helper.
  auto check = [](const TreeOptions& o, const std::string& name) {
    INFO("setup: " << name);
    REQUIRE(o.leaves.alpha_cutoff ==
            Catch::Approx(LeafSilhouetteBakeCutoff(o.leaves.silhouette)));
  };
  check(OakPreset(), "OakPreset");
  check(PinePreset(), "PinePreset");

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) check(setup.options, setup.name);
}

TEST_CASE("TreeCatalog: leaf world-size bands") {
  // Same preview-height scale the viewer applies to fit every catalog tree
  // into its orbit framing (model_viewer_view.cpp's kTreePreviewHeight) --
  // world_leaf_m is the on-screen leaf-card size once a tree is scaled to
  // that preview height, i.e. what a player actually sees.
  constexpr float kPreviewHeight = 8.0f;

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    const LeafOptions& lf = setup.options.leaves;
    CHECK(lf.arrangement != LeafArrangement::CrossedPair);

    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);
    const TexturedMeshResult bark = GenerateTreeMesh(setup.options, skeleton);
    const float height = bark.local_bounds.max.y - bark.local_bounds.min.y;
    REQUIRE(height > 0.0f);

    const float world_leaf_m = lf.size * kPreviewHeight / height;
    const int quads_per_site = QuadsPerLeafSite(lf);

    INFO(setup.name << ": bark_height=" << height
                     << " world_leaf_m=" << world_leaf_m
                     << " count=" << lf.count
                     << " quads_per_site=" << quads_per_site);

    // Band keyed off silhouette: PineSprig is a small evergreen sprig cluster
    // (unchanged), Bush is a compact deciduous sprig, Oak/Ash/Aspen are the
    // bigger cluster-card deciduous sprigs (each card now depicts a whole
    // photographed branch, not one leaf, so it needs to read at 5-8% of tree
    // height -- ~0.45-0.65m at the 8m preview -- to show that content).
    float lo, hi;
    if (lf.silhouette == LeafSilhouette::PineSprig) {
      lo = 0.15f; hi = 0.35f;
    } else if (lf.silhouette == LeafSilhouette::Bush) {
      lo = 0.25f; hi = 0.6f;
    } else {
      lo = 0.35f; hi = 0.8f;
    }
    CHECK(world_leaf_m >= lo);
    CHECK(world_leaf_m <= hi);
  }
}

TEST_CASE("GenerateLeafMesh: deterministic") {
  const TexturedMeshResult a = GenerateLeafMesh(OakPreset());
  const TexturedMeshResult b = GenerateLeafMesh(OakPreset());
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);
}

TEST_CASE("GenerateLeafMesh: well-formed indexed mesh (Oak, Pine)") {
  for (const TreeOptions& o : {OakPreset(), PinePreset()}) {
    const TexturedMeshResult r = GenerateLeafMesh(o);
    const auto& m = r.mesh;
    REQUIRE(m.vertex_count > 0u);
    REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
    REQUIRE(m.indices.size() % 3 == 0);
    for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
    for (float f : m.vertices) REQUIRE(std::isfinite(f));
  }
}

TEST_CASE("GenerateLeafMesh: quad count scales with QuadsPerLeafSite") {
  TreeOptions single = OakPreset();
  single.leaves.arrangement = LeafArrangement::SingleQuad;
  REQUIRE(QuadsPerLeafSite(single.leaves) == 1);
  const TexturedMeshResult r1 = GenerateLeafMesh(single);
  REQUIRE(r1.mesh.vertex_count > 0u);
  // Each quad = 4 verts + 6 indices.
  REQUIRE(r1.mesh.indices.size() == r1.mesh.vertex_count / 4u * 6u);

  struct Case { LeafArrangement arrangement; int blade_count; int expected_quads; };
  const Case cases[] = {
      {LeafArrangement::CrossedPair, 2, 2},
      {LeafArrangement::FanFromStem, 3, 3},
      {LeafArrangement::AxialFins, 3, 3},
  };
  for (const Case& c : cases) {
    INFO("arrangement index " << static_cast<int>(c.arrangement));
    TreeOptions o = OakPreset();
    o.leaves.arrangement = c.arrangement;
    o.leaves.blade_count = c.blade_count;
    REQUIRE(QuadsPerLeafSite(o.leaves) == c.expected_quads);

    const TexturedMeshResult r = GenerateLeafMesh(o);
    REQUIRE(r.mesh.vertex_count ==
            r1.mesh.vertex_count * static_cast<uint32_t>(c.expected_quads));
    REQUIRE(r.mesh.indices.size() ==
            r1.mesh.indices.size() * static_cast<size_t>(c.expected_quads));
    REQUIRE(r.mesh.indices.size() == r.mesh.vertex_count / 4u * 6u);
  }
}

TEST_CASE("GenerateLeafMesh: disabled produces an empty mesh") {
  TreeOptions o = OakPreset();
  o.leaves.enabled = false;
  const TexturedMeshResult r = GenerateLeafMesh(o);
  REQUIRE(r.mesh.vertex_count == 0u);
  REQUIRE(r.mesh.indices.empty());
}

TEST_CASE("BuildLeafRgba8: leaf-shaped alpha card for every silhouette") {
  const int n = 128;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  for (LeafSilhouette shape : kAllLeafSilhouettes) {
    INFO("silhouette index " << static_cast<int>(shape));
    const std::vector<uint8_t> px = BuildLeafRgba8(n, color, shape);
    REQUIRE(px.size() == static_cast<size_t>(n) * static_cast<size_t>(n) * 4);
    auto alpha = [&](int x, int y) {
      return px[(static_cast<size_t>(y) * n + static_cast<size_t>(x)) * 4 + 3];
    };
    REQUIRE(alpha(0, 0) == 0);             // corners are outside
    REQUIRE(alpha(n - 1, 0) == 0);
    REQUIRE(alpha(0, n - 1) == 0);
    REQUIRE(alpha(n - 1, n - 1) == 0);

    // On-shape probe: every silhouette is now a sprig (Oak/Ash/Aspen/Bush) or
    // the needle-stripe sprig (PineSprig) built around a main stem pinned to
    // u=0 near the card's base -- probe that always-present stem column
    // rather than the exact center, which for a sprig may land in a gap
    // between leaf stamps.
    const int probe_x = n / 2;
    const int probe_y = n / 16;
    REQUIRE(alpha(probe_x, probe_y) == 255);

    // RGB carries the passed color (green > red at the probe texel).
    const size_t c = (static_cast<size_t>(probe_y) * n + static_cast<size_t>(probe_x)) * 4;
    REQUIRE(px[c + 1] > px[c + 0]);
  }
}

TEST_CASE("BuildLeafRgba8: opaque-texel counts are pairwise distinct across silhouettes") {
  const int n = 128;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  size_t counts[kAllLeafSilhouettes.size()];
  for (size_t i = 0; i < kAllLeafSilhouettes.size(); ++i) {
    const std::vector<uint8_t> px = BuildLeafRgba8(n, color, kAllLeafSilhouettes[i]);
    size_t count = 0;
    for (size_t p = 0; p < static_cast<size_t>(n) * static_cast<size_t>(n); ++p)
      if (px[p * 4 + 3] >= 128) ++count;
    counts[i] = count;
  }
  for (size_t i = 0; i < kAllLeafSilhouettes.size(); ++i)
    for (size_t j = i + 1; j < kAllLeafSilhouettes.size(); ++j)
      REQUIRE(counts[i] != counts[j]);
}

TEST_CASE("BuildLeafRgba8: deciduous sprigs land in the ez-tree-like coverage band") {
  // Each deciduous silhouette is now a full branch sprig (main stem + twigs +
  // 20-40 leaf stamps), not one leaf filling the card -- assert overall alpha
  // coverage at the production size/cutoff lands in the target density band
  // (ez-tree's photographed sprigs read roughly 30-50% covered).
  const int n = 512;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  constexpr LeafSilhouette kDeciduous[] = {LeafSilhouette::Oak, LeafSilhouette::Ash,
                                           LeafSilhouette::Aspen, LeafSilhouette::Bush};
  for (LeafSilhouette shape : kDeciduous) {
    INFO("silhouette index " << static_cast<int>(shape));
    const std::vector<uint8_t> px = BuildLeafRgba8(n, color, shape);
    const size_t total = static_cast<size_t>(n) * static_cast<size_t>(n);
    size_t covered = 0;
    for (size_t i = 0; i < total; ++i)
      if (px[i * 4 + 3] >= 128) ++covered;  // cutoff 0.5
    const float coverage = static_cast<float>(covered) / static_cast<float>(total);
    INFO("coverage=" << coverage);
    CHECK(coverage >= 0.25f);
    CHECK(coverage <= 0.55f);
  }
}

TEST_CASE("BuildLeafRgba8: deterministic (no RNG state, byte-identical run-to-run)") {
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  const std::vector<uint8_t> a = BuildLeafRgba8(512, color, LeafSilhouette::Oak);
  const std::vector<uint8_t> b = BuildLeafRgba8(512, color, LeafSilhouette::Oak);
  REQUIRE(a == b);
}

TEST_CASE("BuildLeafMipChainRgba8: coverage-preserving mip chain") {
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  struct Case { LeafSilhouette shape; float cutoff; int n; };
  const Case cases[] = {
      {LeafSilhouette::Oak, 0.5f, 128},
      {LeafSilhouette::PineSprig, 0.35f, 128},
      // Production bake size (model_viewer_view.cpp's kLeafTexSize). Box-
      // downsampling INFLATES PineSprig's thin needle-stripe coverage at
      // coarse mips here (empirically 1.19-1.54x pre-fix) -- the 128 probe
      // above doesn't reproduce it; this is the case the Castano bisection's
      // [0.25, 4.0] scale range (leaf_texture.cpp) exists to correct, since a
      // scale floor of 1.0 can never shrink inflated coverage back down.
      {LeafSilhouette::PineSprig, 0.35f, 512},
  };

  for (const Case& c : cases) {
    INFO("silhouette index " << static_cast<int>(c.shape) << " n=" << c.n);
    const std::vector<std::vector<uint8_t>> mips = BuildLeafMipChainRgba8(c.n, color, c.shape, c.cutoff);
    size_t expected_levels = 1;
    for (int w = c.n; w > 1; w /= 2) ++expected_levels;
    REQUIRE(mips.size() == expected_levels);

    auto coverage = [&](const std::vector<uint8_t>& px, int size) {
      const uint8_t thresh = static_cast<uint8_t>(std::lround(c.cutoff * 255.0f));
      size_t count = 0;
      const size_t total = static_cast<size_t>(size) * static_cast<size_t>(size);
      for (size_t i = 0; i < total; ++i)
        if (px[i * 4 + 3] >= thresh) ++count;
      return static_cast<float>(count) / static_cast<float>(total);
    };

    int size = c.n;
    for (const std::vector<uint8_t>& level : mips) {
      REQUIRE(level.size() == static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
      size = std::max(1, size / 2);
    }

    const float level0_coverage = coverage(mips[0], c.n);
    REQUIRE(level0_coverage > 0.0f);

    size = c.n;
    for (const std::vector<uint8_t>& level : mips) {
      if (size >= 8) {
        const float cov = coverage(level, size);
        REQUIRE(std::fabs(cov - level0_coverage) <= 0.30f * level0_coverage);
      }
      size = std::max(1, size / 2);
    }
  }
}

TEST_CASE("GenerateLeafMesh: terminal-tip leaf adds one leaf per leaf-bearing branch") {
  auto count_terminal = [](const TreeOptions& o) {
    int n = 0;
    for (const SkeletonBranch& b : BuildTreeSkeleton(o))
      if (b.level == o.levels && static_cast<int>(b.sections.size()) - 1 >= 1) ++n;
    return n;
  };
  TreeOptions on = OakPreset();  on.leaves.tip_leaf = true;
  TreeOptions off = OakPreset(); off.leaves.tip_leaf = false;
  const uint32_t vn = GenerateLeafMesh(on).mesh.vertex_count;
  const uint32_t vf = GenerateLeafMesh(off).mesh.vertex_count;
  const int quads = QuadsPerLeafSite(OakPreset().leaves);
  REQUIRE(vn > vf);
  // Each tip leaf = quads * 4 verts; one per leaf-bearing terminal branch.
  REQUIRE(vn - vf == static_cast<uint32_t>(count_terminal(OakPreset()) * quads * 4));
}

TEST_CASE("GenerateLeafMesh: FanFromStem blades never weld (full-stride distinct)") {
  TreeOptions o = OakPreset();
  o.leaves.arrangement = LeafArrangement::FanFromStem;
  o.leaves.blade_count = 3;
  const int n = QuadsPerLeafSite(o.leaves);
  REQUIRE(n == 3);

  const TexturedMeshResult r = GenerateLeafMesh(o);
  const size_t stride = kTexturedMeshFloatsPerVertex;
  const size_t verts_per_site = static_cast<size_t>(n) * 4u;
  REQUIRE(r.mesh.vertices.size() >= verts_per_site * stride);

  // First site (first n*4 verts). No two verts from DIFFERENT blades of this
  // site may be bit-identical across the full 11-float stride (pos+uv+normal+
  // tangent) -- shared full-stride verts are exactly what the LOD simplifier
  // would weld, collapsing separate blades into one.
  for (size_t i = 0; i < verts_per_site; ++i) {
    for (size_t j = i + 1; j < verts_per_site; ++j) {
      if (i / 4u == j / 4u) continue;  // same blade -- not the case under test
      bool identical = true;
      for (size_t k = 0; k < stride; ++k) {
        if (r.mesh.vertices[i * stride + k] != r.mesh.vertices[j * stride + k]) {
          identical = false;
          break;
        }
      }
      INFO("vertex " << i << " vs " << j);
      REQUIRE_FALSE(identical);
    }
  }
}

TEST_CASE("GenerateLeafMesh: AxialFins blades share one long axis") {
  TreeOptions o = OakPreset();
  o.leaves.arrangement = LeafArrangement::AxialFins;
  o.leaves.blade_count = 3;
  const int n = QuadsPerLeafSite(o.leaves);
  REQUIRE(n == 3);

  const TexturedMeshResult r = GenerateLeafMesh(o);
  const size_t stride = kTexturedMeshFloatsPerVertex;
  auto pos = [&](size_t v) {
    const size_t off = v * stride;
    return glm::vec3(r.mesh.vertices[off + 0], r.mesh.vertices[off + 1],
                     r.mesh.vertices[off + 2]);
  };

  // Quad corners are {top-left, bottom-left, bottom-right, top-right}; the
  // card's long axis is top-left minus bottom-left (== top-right minus
  // bottom-right, same rotated edge).
  glm::vec3 axis[3];
  for (int b = 0; b < n; ++b) {
    const size_t base = static_cast<size_t>(b) * 4u;
    axis[b] = glm::normalize(pos(base + 0) - pos(base + 1));
  }
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j)
      REQUIRE(std::fabs(glm::dot(axis[i], axis[j])) == Catch::Approx(1.0f).margin(1e-4f));
}
