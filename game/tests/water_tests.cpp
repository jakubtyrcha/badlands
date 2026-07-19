// Water wave/normal math (pure CPU) + octahedron encode/decode mirror.
//
// These free functions are the CPU source-of-truth for the water surface:
//   - engine/rendering/water_waves.hpp  (glm-only, GPU-free)
//   - engine/rendering/octahedron.hpp   (mirrors shaders/common encode/decode)
// shaders/common/water_waves.wesl mirrors the same wave constants/formulas so
// the GPU normal-readback test (badlands_water_gpu_test) can cross-check that
// the shader agrees with this core.

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cmath>

#include "engine/rendering/octahedron.hpp"
#include "engine/rendering/water_waves.hpp"

using namespace badlands;

namespace {

// Central-difference height-field normal built purely from WaveHeight samples,
// used to validate the analytic WaveNormal.
glm::vec3 NumericNormal(glm::vec2 xz, float t,
                        const std::array<water::Wave, water::kWaveCount>& waves,
                        float h = 1e-3f) {
  float hx = (water::WaveHeight(xz + glm::vec2(h, 0.0f), t, waves) -
              water::WaveHeight(xz - glm::vec2(h, 0.0f), t, waves)) /
             (2.0f * h);
  float hz = (water::WaveHeight(xz + glm::vec2(0.0f, h), t, waves) -
              water::WaveHeight(xz - glm::vec2(0.0f, h), t, waves)) /
             (2.0f * h);
  return glm::normalize(glm::vec3(-hx, 1.0f, -hz));
}

}  // namespace

TEST_CASE("flat water (zero amplitude) is planar with +Y normal", "[water]") {
  std::array<water::Wave, water::kWaveCount> flat{};
  for (auto& w : flat) {
    w.dir = glm::vec2(1.0f, 0.0f);
    w.amplitude = 0.0f;
    w.frequency = 0.5f;
    w.speed = 1.0f;
    w.phase = 0.0f;
  }
  for (float t : {0.0f, 3.3f}) {
    for (glm::vec2 p : {glm::vec2(0, 0), glm::vec2(12.5f, -7.0f)}) {
      CHECK(water::WaveHeight(p, t, flat) == Catch::Approx(0.0f).margin(1e-6));
      glm::vec3 n = water::WaveNormal(p, t, flat);
      CHECK(n.x == Catch::Approx(0.0f).margin(1e-6));
      CHECK(n.y == Catch::Approx(1.0f).margin(1e-6));
      CHECK(n.z == Catch::Approx(0.0f).margin(1e-6));
    }
  }
}

TEST_CASE("single +X wave has the expected height and slope", "[water]") {
  // One wave along +X: h(x) = A sin(k x + phase) at t=0. Others zero-amplitude.
  const float A = 0.2f, k = 0.4f, phase = 0.0f;
  std::array<water::Wave, water::kWaveCount> waves{};
  waves[0] = {glm::vec2(1.0f, 0.0f), A, k, 0.0f, phase};
  waves[1] = {glm::vec2(1.0f, 0.0f), 0.0f, 1.0f, 0.0f, 0.0f};
  waves[2] = {glm::vec2(1.0f, 0.0f), 0.0f, 1.0f, 0.0f, 0.0f};

  const float x = 2.0f;
  CHECK(water::WaveHeight(glm::vec2(x, 0.0f), 0.0f, waves) ==
        Catch::Approx(A * std::sin(k * x)));

  // dH/dx = A k cos(kx); dH/dz = 0 -> N = normalize(vec3(-A k cos(kx), 1, 0)).
  glm::vec3 n = water::WaveNormal(glm::vec2(x, 0.0f), 0.0f, waves);
  glm::vec3 expected =
      glm::normalize(glm::vec3(-A * k * std::cos(k * x), 1.0f, 0.0f));
  CHECK(n.x == Catch::Approx(expected.x).margin(1e-4));
  CHECK(n.y == Catch::Approx(expected.y).margin(1e-4));
  CHECK(n.z == Catch::Approx(expected.z).margin(1e-4));
}

TEST_CASE("analytic WaveNormal matches finite differences of WaveHeight",
          "[water]") {
  const auto& waves = water::DefaultWaves();
  for (float t : {0.0f, 1.25f, 6.5f}) {
    for (glm::vec2 p :
         {glm::vec2(0, 0), glm::vec2(3.2f, 9.1f), glm::vec2(-14.0f, 5.5f)}) {
      glm::vec3 analytic = water::WaveNormal(p, t, waves);
      glm::vec3 numeric = NumericNormal(p, t, waves);
      CAPTURE(t, p.x, p.y);
      CHECK(glm::length(analytic) == Catch::Approx(1.0f).margin(1e-5));
      CHECK(glm::distance(analytic, numeric) < 1e-3f);
    }
  }
}

TEST_CASE("WaveHeight is bounded by the sum of amplitudes", "[water]") {
  const auto& waves = water::DefaultWaves();
  float bound = 0.0f;
  for (const auto& w : waves) bound += std::abs(w.amplitude);
  for (float t : {0.0f, 2.0f, 4.0f, 8.0f}) {
    for (int i = -20; i <= 20; ++i) {
      float h = water::WaveHeight(glm::vec2(float(i) * 1.7f, float(-i)), t, waves);
      CHECK(std::abs(h) <= bound + 1e-4f);
    }
  }
}

TEST_CASE("octahedron encode/decode round-trips unit normals", "[water][octa]") {
  const std::array<glm::vec3, 7> dirs = {
      glm::vec3(0, 1, 0),  glm::vec3(0, -1, 0),
      glm::vec3(1, 0, 0),  glm::vec3(0, 0, 1),
      glm::vec3(0, 0, -1), glm::normalize(glm::vec3(0.3f, 0.9f, -0.2f)),
      glm::normalize(glm::vec3(-0.5f, 0.4f, -0.77f))};
  for (glm::vec3 n : dirs) {
    glm::vec3 r = DecodeOctahedron(EncodeOctahedron(n));
    CAPTURE(n.x, n.y, n.z);
    CHECK(glm::distance(r, n) < 1e-4f);
  }
}
