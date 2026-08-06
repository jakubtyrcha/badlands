// Headless GPU verification of the impostor bake (volumetric-foliage LOD4).
//
// Checked by READING THE ATLAS BACK, not by looking at a picture: what can go
// wrong here -- a tile baked empty, a view baked into the wrong tile, normals
// written in the wrong space, coverage dissolving down the mip chain -- all
// show up in the pixels as numbers long before they show up on screen as
// something a person would notice, and several of them look like "the impostor
// is a bit dark/thin" rather than like a bug.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "core/util/cpu_image.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/visual/alpha_coverage.hpp"
#include "game/visual/impostor_baker.hpp"
#include "game/visual/octahedral.hpp"
#include "game/visual/instanced_lod_field.hpp"
#include "game/visual/tree_lod_model.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

struct BakeGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> gen;
};

BakeGpu& GetBakeGpu() {
  static BakeGpu* g = [] {
    auto* b = new BakeGpu();
    wgpu::InstanceDescriptor idesc = {};
    b->instance = wgpu::CreateInstance(&idesc);
    REQUIRE(b->instance);
    wgpu::Adapter adapter = test::RequestAdapter(b->instance);
    REQUIRE(adapter);
    b->device = test::RequestDevice(adapter);
    REQUIRE(b->device);
    b->queue = b->device.GetQueue();
    b->gen = std::make_unique<GpuPipelineGenerator>(b->device,
                                                    FindShaderDirectory());
    return b;
  }();
  return *g;
}

// Two presets with deliberately different crown shapes: a broad deciduous and a
// narrow conifer, so a bug that only bites one silhouette cannot hide.
std::vector<InstancedLodModel> TestModels() {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  std::vector<InstancedLodModel> models;
  for (const char* name : {"Oak (large)", "Pine (large)"}) {
    for (const NamedTreeOptions& n : catalog) {
      if (n.name == name) {
        models.push_back(BuildTreeFieldModel(n.options, 20.0f));
        break;
      }
    }
  }
  REQUIRE(models.size() == 2);
  return models;
}

// Coverage of one tile at one mip, as a fraction of the tile's texels.
float TileCoverage(const CpuImage& img, int i, int j, uint32_t mip,
                   uint8_t cutoff) {
  const ImpostorTileRect tile = ImpostorTilePixels(i, j, mip);
  size_t hit = 0, total = 0;
  for (uint32_t y = tile.y; y < tile.y + tile.size; ++y) {
    for (uint32_t x = tile.x; x < tile.x + tile.size; ++x) {
      if (img.GetPixel(x, y).a >= cutoff) ++hit;
      ++total;
    }
  }
  return total ? static_cast<float>(hit) / static_cast<float>(total) : 0.0f;
}

// The threshold the bake preserves coverage at. Both the bark and the voxel
// crown are opaque, so mip 0's alpha is a hard silhouette mask and half-coverage
// is the meaningful cut -- the models' own alpha_cutoff described their leaf
// CARDS, which the bake no longer draws. Reading at a different threshold than
// the one preserved reports near-zero for an intact tile.
constexpr uint8_t kSilhouetteCutoff = 128;

}  // namespace

TEST_CASE("Every impostor tile bakes real coverage", "[impostor][gpu]") {
  // The failure this exists for is the pine dead zone recurring somewhere new:
  // a silhouette that rasterizes to nothing produces a transparent tile, which
  // at runtime is an invisible tree rather than an error.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();

  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);
  REQUIRE(baked.atlas.valid());
  REQUIRE(baked.placement.size() == models.size());

  TextureReadback readback(g.instance, g.device, g.queue);
  for (uint32_t m = 0; m < models.size(); ++m) {
    const CpuImage img =
        readback.ReadTextureMip(baked.atlas.albedo, 0, m).Await();
    REQUIRE(img.GetWidth() == kImpostorLayerPx);

    for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
      for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
        const float cov = TileCoverage(img, i, j, 0, kSilhouetteCutoff);
        INFO("model " << m << " tile " << i << "," << j << " coverage " << cov);
        // A band, not a number: a tree seen from near-overhead legitimately
        // covers far less of its tile than one seen from the side, and the
        // frame is a bounding SPHERE so no view fills the tile completely.
        CHECK(cov > 0.02f);
        CHECK(cov < 0.95f);
      }
    }
  }
}

TEST_CASE("A model's placement frame contains its geometry", "[impostor][gpu]") {
  // If the radius under-covers, the bake clips the tree at the tile edge from
  // some views -- which reads as a tree with a flat side.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();
  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);

  for (size_t m = 0; m < models.size(); ++m) {
    // The frame must contain what the bake DRAWS, and the bake draws exactly
    // what the model's ImpostorBakeSpec names -- so the expectation is derived
    // from the spec rather than from a second hardcoded idea of which submeshes
    // those are. For a tree that resolves to LOD0's bark plus its voxel crown,
    // which is what the field's own LOD0 shows.
    Aabb bounds = Aabb::Empty();
    for (const ImpostorBakeSubmesh& sub : models[m].impostor.submeshes) {
      const TexturedMeshResult& mesh = models[m].levels[sub.lod][sub.submesh];
      if (mesh.mesh.vertex_count == 0) continue;
      bounds = bounds.Union(mesh.local_bounds);
    }
    const ImpostorPlacement& p = baked.placement[m];
    INFO("model " << m);
    // Every corner of the silhouette's box is inside the sphere the bake
    // framed with -- the condition for no view clipping.
    for (int c = 0; c < 8; ++c) {
      const glm::vec3 corner((c & 1) ? bounds.max.x : bounds.min.x,
                             (c & 2) ? bounds.max.y : bounds.min.y,
                             (c & 4) ? bounds.max.z : bounds.min.z);
      CHECK(glm::length(corner - p.local_center) <= p.radius * 1.001f);
    }
    CHECK(p.radius > 0.0f);
  }
}

TEST_CASE("Each tile's baked normals face that tile's own view", "[impostor][gpu]") {
  // Far stronger than checking the normals are unit length, which they are by
  // construction and which proves nothing. Surfaces a view can SEE must, on
  // average, face it -- the bake flips two-sided leaf cards toward the viewer
  // for exactly this reason. So the mean decoded normal of tile (i,j) must have
  // a positive dot with ImpostorViewDirection(i,j).
  //
  // This catches three separate mistakes at once, all of which produce a
  // plausible-looking atlas: normals baked in the wrong SPACE (view instead of
  // local), views rendered into the wrong TILE (a transposed index), and the
  // octahedral remap losing its sign.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();
  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);

  TextureReadback readback(g.instance, g.device, g.queue);
  const CpuImage albedo =
      readback.ReadTextureMip(baked.atlas.albedo, 0, 0).Await();
  const CpuImage surface =
      readback.ReadTextureMip(baked.atlas.surface, 0, 0).Await();
  REQUIRE(surface.GetWidth() == kImpostorLayerPx);

  // The same decode deferred_lighting.wesl uses (frame.wesl's
  // decodeOctahedron), in C++.
  const auto decode = [](glm::vec2 oct) {
    glm::vec3 n(oct.x, oct.y, 1.0f - std::abs(oct.x) - std::abs(oct.y));
    if (n.z < 0.0f) {
      n = glm::vec3((1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f),
                    (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f), n.z);
    }
    return n;
  };

  int tiles_checked = 0;
  for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
    for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
      const ImpostorTileRect t = ImpostorTilePixels(i, j, 0);
      glm::vec3 sum(0.0f);
      int covered = 0;
      for (uint32_t y = t.y; y < t.y + t.size; ++y) {
        for (uint32_t x = t.x; x < t.x + t.size; ++x) {
          // Only where something was actually drawn -- cleared texels carry the
          // clear value, which is not a normal.
          if (albedo.GetPixel(x, y).a < kSilhouetteCutoff) continue;
          const CpuImage::Color c = surface.GetPixel(x, y);
          const glm::vec2 oct(static_cast<float>(c.r) / 255.0f * 2.0f - 1.0f,
                              static_cast<float>(c.g) / 255.0f * 2.0f - 1.0f);
          sum += glm::normalize(decode(oct));
          ++covered;
        }
      }
      REQUIRE(covered > 20);
      const glm::vec3 mean = sum / static_cast<float>(covered);
      REQUIRE(glm::length(mean) > 1e-3f);

      const glm::vec3 view = ImpostorViewDirection(i, j);
      const float d = glm::dot(glm::normalize(mean), view);
      INFO("tile " << i << "," << j << " mean-normal dot view = " << d
                   << " over " << covered << " texels");
      CHECK(d > 0.2f);
      ++tiles_checked;
    }
  }
  CHECK(tiles_checked == kImpostorViewCount);
}

TEST_CASE("Coverage survives the mip chain", "[impostor][gpu]") {
  // Without Castano preservation a box filter drives partially-covered texels
  // under the cutoff, so a cutout tree THINS as it recedes and eventually
  // disappears. leaf_texture.cpp already learned this; the atlas shares the
  // kernel (alpha_coverage.hpp) so it cannot relearn it.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();
  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);

  TextureReadback readback(g.instance, g.device, g.queue);

  for (uint32_t m = 0; m < models.size(); ++m) {
    constexpr uint8_t cutoff = kSilhouetteCutoff;
    const CpuImage base =
        readback.ReadTextureMip(baked.atlas.albedo, 0, m).Await();

    // Tile (0,0) is a low-elevation SIDE view (~18 degrees), where a tree
    // covers a real fraction of its tile. A near-zenith tile covers a few
    // percent, so at a 4x4 mip its coverage quantizes to 0 or 1/16 and no
    // assertion about it would mean anything.
    const float c0 = TileCoverage(base, 0, 0, 0, cutoff);
    REQUIRE(c0 > 0.05f);

    // Stops before the 4x4 level for the same quantization reason: 16 texels
    // resolve coverage only to the nearest 6.25%.
    for (uint32_t mip = 1; mip + 1 < kImpostorMipLevels; ++mip) {
      const CpuImage img =
          readback.ReadTextureMip(baked.atlas.albedo, mip, m).Await();
      REQUIRE(img.GetWidth() == (kImpostorLayerPx >> mip));
      const float c = TileCoverage(img, 0, 0, mip, cutoff);
      INFO("model " << m << " mip " << mip << ": coverage " << c << " vs base "
                    << c0);
      // Unpreserved, this collapses toward 0 as the chain deepens; the band is
      // wide because coarse levels are quantized, not because the target is
      // vague.
      CHECK(c > c0 * 0.5f);
      CHECK(c < c0 * 2.0f + 0.1f);
    }
  }
}

TEST_CASE("The bake is deterministic", "[impostor][gpu]") {
  // Content built at load must be identical run to run, or a screenshot
  // comparison and a bug report stop meaning anything.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();

  const ImpostorBakeResult a =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  const ImpostorBakeResult b =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(a.ok);
  REQUIRE(b.ok);

  TextureReadback readback(g.instance, g.device, g.queue);
  const CpuImage ia = readback.ReadTextureMip(a.atlas.albedo, 0, 0).Await();
  const CpuImage ib = readback.ReadTextureMip(b.atlas.albedo, 0, 0).Await();
  REQUIRE(ia.GetDataSize() == ib.GetDataSize());

  size_t diff = 0;
  for (uint32_t y = 0; y < kImpostorLayerPx; ++y) {
    for (uint32_t x = 0; x < kImpostorLayerPx; ++x) {
      const CpuImage::Color pa = ia.GetPixel(x, y);
      const CpuImage::Color pb = ib.GetPixel(x, y);
      if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b || pa.a != pb.a) ++diff;
    }
  }
  CHECK(diff == 0);

  REQUIRE(a.placement.size() == b.placement.size());
  for (size_t i = 0; i < a.placement.size(); ++i) {
    CHECK(a.placement[i].radius == b.placement[i].radius);
  }
}

TEST_CASE("A field builds its impostor LOD and keeps the voxel chain intact",
          "[impostor][gpu]") {
  // The wiring test. Asserted through what InstancedLodField itself exposes -- the
  // factory, the shared quad, and the material-handle count -- rather than by
  // reaching into GpuInstanceRenderer, which is private and not worth widening
  // the engine's interface for.
  //
  // The handle count is the load-bearing one: the impostor level resolves
  // exactly two materials per model (its G-buffer variant and its light-facing
  // shadow variant), so a level that silently failed to configure shows up here
  // as a short count rather than as trees quietly vanishing at range.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();
  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);

  std::unique_ptr<InstancedLodField> plain =
      BuildInstancedLodField(g.device, g.queue, *g.gen, models, 64);
  REQUIRE(plain);
  CHECK(plain->impostor_factory == nullptr);
  CHECK(plain->impostor_vertex_buffer == nullptr);
  const size_t plain_handles = plain->material_handles.size();

  InstancedLodImpostor imp;
  imp.atlas = &baked.atlas;
  imp.placement = baked.placement;
  REQUIRE(imp.active(models.size()));

  std::unique_ptr<InstancedLodField> with =
      BuildInstancedLodField(g.device, g.queue, *g.gen, models, 64, imp);
  REQUIRE(with);
  CHECK(with->impostor_factory != nullptr);
  CHECK(with->impostor_vertex_buffer != nullptr);
  CHECK(with->impostor_index_buffer != nullptr);
  CHECK(with->material_handles.size() == plain_handles + 2 * models.size());
  // The mesh levels are untouched: same models, same per-model level count.
  CHECK(with->buffers.size() == plain->buffers.size());
  for (size_t m = 0; m < models.size(); ++m) {
    INFO("model " << m);
    CHECK(with->buffers[m].size() == plain->buffers[m].size());
  }
}

TEST_CASE("A half-supplied impostor is refused, not half-enabled",
          "[impostor][gpu]") {
  // An atlas with the wrong number of placements must fall back to the
  // voxel-only chain. Half-enabling would give every model an LOD4 slot that
  // nothing configured, which draws nothing -- trees disappearing at distance,
  // with no error anywhere.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();
  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);

  InstancedLodImpostor imp;
  imp.atlas = &baked.atlas;
  imp.placement = std::span<const ImpostorPlacement>(baked.placement).first(1);
  CHECK_FALSE(imp.active(models.size()));

  std::unique_ptr<InstancedLodField> tf =
      BuildInstancedLodField(g.device, g.queue, *g.gen, models, 64, imp);
  REQUIRE(tf);
  CHECK(tf->impostor_factory == nullptr);
}

TEST_CASE("The baked thickness has real interior structure", "[impostor][gpu]") {
  // Thickness is what gives the impostor volume: it drives the per-pixel
  // translucency and the AO, so a channel that came back constant (or zero)
  // would leave the crown lighting exactly as flat as the billboard it replaced
  // -- and would look like a shading problem, not a bake problem.
  //
  // Asserted as a DISTRIBUTION rather than as values: the crown's silhouette
  // edge must be thin and its middle thick, which is the whole claim.
  BakeGpu& g = GetBakeGpu();
  const std::vector<InstancedLodModel> models = TestModels();
  const ImpostorBakeResult baked =
      BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(baked.ok);

  TextureReadback readback(g.instance, g.device, g.queue);
  for (uint32_t m = 0; m < models.size(); ++m) {
    const CpuImage albedo =
        readback.ReadTextureMip(baked.atlas.albedo, 0, m).Await();
    const CpuImage surface =
        readback.ReadTextureMip(baked.atlas.surface, 0, m).Await();

    // A low-elevation side view, where a tree presents its full depth.
    const ImpostorTileRect t = ImpostorTilePixels(0, 0, 0);
    std::vector<int> thick;
    int nonzero = 0;
    for (uint32_t y = t.y; y < t.y + t.size; ++y) {
      for (uint32_t x = t.x; x < t.x + t.size; ++x) {
        if (albedo.GetPixel(x, y).a < kSilhouetteCutoff) continue;
        const int a = surface.GetPixel(x, y).a;
        thick.push_back(a);
        if (a > 0) ++nonzero;
      }
    }
    REQUIRE(thick.size() > 200);

    INFO("model " << m << ": " << nonzero << " of " << thick.size()
                  << " covered texels carry thickness");
    // Most of the crown must have measurable depth. All-zero means the signed
    // accumulation cancelled (a winding or blend-state mistake), which is the
    // failure this exists to catch.
    CHECK(nonzero > static_cast<int>(thick.size()) / 2);

    std::sort(thick.begin(), thick.end());
    const int p10 = thick[thick.size() / 10];
    const int p90 = thick[thick.size() * 9 / 10];
    INFO("thickness p10=" << p10 << " p90=" << p90);
    // Real structure, not a constant: the thin tenth and the thick tenth must
    // differ. A constant channel would give the crown one uniform AO and one
    // uniform transmission, i.e. exactly the flat billboard look.
    CHECK(p90 > p10 + 8);
    // And it must stay in range -- the accumulation is normalized by the
    // bake's own 2*radius span, so saturating at 255 everywhere would mean the
    // normalization is wrong.
    CHECK(p90 < 250);
  }
}

