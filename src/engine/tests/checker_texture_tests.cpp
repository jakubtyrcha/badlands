// Pure-CPU tests for the RGBA8 checkerboard fill (engine/rendering/
// checker_texture.hpp), the GPU-free core of MaterialLibrary::CheckerAlbedo.
#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>

#include "engine/rendering/checker_texture.hpp"

using badlands::BuildCheckerboardRgba8;

TEST_CASE("checkerboard has RGBA8 size and opaque alpha") {
  const auto px = BuildCheckerboardRgba8(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                         /*tiles=*/2, /*texels=*/4);
  REQUIRE(px.size() == 4u * 4u * 4u);
  for (size_t i = 3; i < px.size(); i += 4) REQUIRE(px[i] == 255);
}

TEST_CASE("checkerboard alternates color_a / color_b by tile parity") {
  const auto px = BuildCheckerboardRgba8(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                         /*tiles=*/2, /*texels=*/4);
  auto rgb_at = [&](int x, int y) {
    const size_t o = (static_cast<size_t>(y) * 4 + x) * 4;
    return glm::ivec3(px[o], px[o + 1], px[o + 2]);
  };
  // tile size = 4/2 = 2 px. (0,0)=A red, (2,0)=B green, (0,2)=B green, (2,2)=A red.
  REQUIRE(rgb_at(0, 0) == glm::ivec3(255, 0, 0));
  REQUIRE(rgb_at(2, 0) == glm::ivec3(0, 255, 0));
  REQUIRE(rgb_at(0, 2) == glm::ivec3(0, 255, 0));
  REQUIRE(rgb_at(2, 2) == glm::ivec3(255, 0, 0));
}
