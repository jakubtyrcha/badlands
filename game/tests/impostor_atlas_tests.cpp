// The impostor atlas' tile layout (src/game/visual/impostor_atlas.hpp).
//
// The baker WRITES tiles by this arithmetic and the material READS them by it.
// If the two ever disagree the result is not an error -- it is a tree rendered
// from a neighbouring view, or one fringed with a slice of the view next door,
// which reads as "the impostor looks a bit off" and is near-impossible to trace
// from a screenshot. All of it is integer and float arithmetic, so none of it
// needs a device.

#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>

#include "game/visual/impostor_atlas.hpp"

using namespace badlands;

namespace {

// Outer pixel bounds of a tile, normalized -- what the texel-centre range must
// stay strictly inside of.
struct OuterUv {
  float min_x, max_x, min_y, max_y;
};

OuterUv OuterBounds(int i, int j, uint32_t mip) {
  const float tile = static_cast<float>(kImpostorTilePx >> mip);
  const float layer = static_cast<float>(kImpostorLayerPx >> mip);
  return OuterUv{static_cast<float>(i) * tile / layer,
                 (static_cast<float>(i) + 1.0f) * tile / layer,
                 static_cast<float>(j) * tile / layer,
                 (static_cast<float>(j) + 1.0f) * tile / layer};
}

}  // namespace

TEST_CASE("Every tile's uv range stays inside its own pixel rect",
          "[impostor]") {
  // The direct statement of "a fetch for this view cannot reach another view".
  for (uint32_t mip = 0; mip < kImpostorMipLevels; ++mip) {
    for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
      for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
        const ImpostorUvRect r = ImpostorTileUvRange(i, j, mip);
        const OuterUv o = OuterBounds(i, j, mip);
        INFO("mip " << mip << " tile " << i << "," << j);
        CHECK(r.min.x > o.min_x);
        CHECK(r.max.x < o.max_x);
        CHECK(r.min.y > o.min_y);
        CHECK(r.max.y < o.max_y);
        // And non-degenerate -- a collapsed range would sample one texel for
        // the whole tile, which is what a mip chain run one level too far does.
        CHECK(r.max.x > r.min.x);
        CHECK(r.max.y > r.min.y);
      }
    }
  }
}

TEST_CASE("No two tiles' uv ranges overlap, at any mip", "[impostor]") {
  // Stated over ALL pairs rather than adjacent ones, so a transposed index (a
  // row/column swap in the tile origin) fails here rather than surviving as a
  // consistent-looking but wrong layout.
  for (uint32_t mip = 0; mip < kImpostorMipLevels; ++mip) {
    for (int a = 0; a < kImpostorViewCount; ++a) {
      const ImpostorUvRect ra =
          ImpostorTileUvRange(a % kImpostorViewsPerAxis,
                              a / kImpostorViewsPerAxis, mip);
      for (int b = a + 1; b < kImpostorViewCount; ++b) {
        const ImpostorUvRect rb =
            ImpostorTileUvRange(b % kImpostorViewsPerAxis,
                                b / kImpostorViewsPerAxis, mip);
        const bool disjoint_x = ra.max.x < rb.min.x || rb.max.x < ra.min.x;
        const bool disjoint_y = ra.max.y < rb.min.y || rb.max.y < ra.min.y;
        INFO("mip " << mip << " views " << a << " and " << b);
        CHECK((disjoint_x || disjoint_y));
      }
    }
  }
}

TEST_CASE("Local uv maps across the tile monotonically and hits its centre",
          "[impostor]") {
  for (uint32_t mip = 0; mip < kImpostorMipLevels; ++mip) {
    const ImpostorUvRect r = ImpostorTileUvRange(2, 1, mip);
    INFO("mip " << mip);

    CHECK(ImpostorTileUv(2, 1, glm::vec2(0.0f), mip).x == Catch::Approx(r.min.x));
    CHECK(ImpostorTileUv(2, 1, glm::vec2(1.0f), mip).x == Catch::Approx(r.max.x));

    // The tile's geometric centre, which is where the baked tree's trunk axis
    // lands -- a half-texel error here shifts every impostor sideways.
    const OuterUv o = OuterBounds(2, 1, mip);
    const glm::vec2 mid = ImpostorTileUv(2, 1, glm::vec2(0.5f), mip);
    CHECK(mid.x == Catch::Approx((o.min_x + o.max_x) * 0.5f).margin(1e-6));
    CHECK(mid.y == Catch::Approx((o.min_y + o.max_y) * 0.5f).margin(1e-6));

    float prev = -1.0f;
    for (int s = 0; s <= 16; ++s) {
      const float t = static_cast<float>(s) / 16.0f;
      const float u = ImpostorTileUv(2, 1, glm::vec2(t, t), mip).x;
      CHECK(u > prev);
      prev = u;
    }
  }
}

TEST_CASE("The uv range is the tile's texel centres, exactly", "[impostor]") {
  // Pinned as arithmetic rather than as a property, because this is the one
  // number the bake has to agree with: a tile of T texels has centres at
  // 0.5/T .. (T-0.5)/T of the tile. Getting it wrong by half a texel is
  // invisible at mip 0 and a tenth of the tile at mip 5.
  for (uint32_t mip = 0; mip < kImpostorMipLevels; ++mip) {
    const float tile = static_cast<float>(kImpostorTilePx >> mip);
    const float layer = static_cast<float>(kImpostorLayerPx >> mip);
    for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
      for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
        const ImpostorUvRect r = ImpostorTileUvRange(i, j, mip);
        INFO("mip " << mip << " tile " << i << "," << j);
        CHECK(r.min.x == Catch::Approx((static_cast<float>(i) * tile + 0.5f) /
                                       layer));
        CHECK(r.max.x == Catch::Approx((static_cast<float>(i) * tile + tile -
                                        0.5f) / layer));
        CHECK(r.min.y == Catch::Approx((static_cast<float>(j) * tile + 0.5f) /
                                       layer));
        CHECK(r.max.y == Catch::Approx((static_cast<float>(j) * tile + tile -
                                        0.5f) / layer));
      }
    }
  }
}

TEST_CASE("Tile pixel rects tile the layer without gaps or overlap",
          "[impostor]") {
  for (uint32_t mip = 0; mip < kImpostorMipLevels; ++mip) {
    const uint32_t tile = kImpostorTilePx >> mip;
    for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
      for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
        const ImpostorTileRect t = ImpostorTilePixels(i, j, mip);
        INFO("mip " << mip << " tile " << i << "," << j);
        CHECK(t.size == tile);
        CHECK(t.x == static_cast<uint32_t>(i) * tile);
        CHECK(t.y == static_cast<uint32_t>(j) * tile);
        CHECK(t.x + t.size <= (kImpostorLayerPx >> mip));
        CHECK(t.y + t.size <= (kImpostorLayerPx >> mip));
      }
    }
    // The grid exactly fills the layer -- a layer bigger than its tiles would
    // leave a band the bake never writes and the sampler could still reach.
    const ImpostorTileRect last = ImpostorTilePixels(
        kImpostorViewsPerAxis - 1, kImpostorViewsPerAxis - 1, mip);
    CHECK(last.x + last.size == (kImpostorLayerPx >> mip));
    CHECK(last.y + last.size == (kImpostorLayerPx >> mip));
  }
}

TEST_CASE("Binding refuses an atlas that was never built", "[impostor]") {
  // The contract that replaces an engine change: both arrays MUST be bound,
  // because the factory's per-geometry default cannot produce an array view
  // and an unbound slot reaches Dawn as a dimension mismatch at DRAW time --
  // far from the mistake. Failing here names it.
  ImpostorAtlas empty;
  InstanceParams params;
  CHECK_FALSE(BindImpostorAtlas(empty, params));
  CHECK(params.texture_overrides.empty());
}
