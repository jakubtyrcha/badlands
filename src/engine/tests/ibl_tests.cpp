// IBL: the CPU half, asserted with no GPU present.
//
// Everything here is about a CONVENTION agreeing with another convention --
// the face layout with the shader's, the SH coefficients with the evaluator's,
// the prefilter's roughness with the resolve's LOD. Every one of those failures
// produces a smooth, plausible, wrong image, which is why they are asserted
// numerically rather than looked at.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/packing.hpp>

#include "engine/ibl/environment.hpp"
#include "engine/ibl/prefiltered_cube.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

using namespace badlands;
namespace rhi = badlands::rhi;
using badlands::ibl::EquirectImage;
using badlands::ibl::PrefilteredCube;
using badlands::ibl::SkySettings;

namespace {

// The C++ mirror of EvaluateAmbientSHL2 in shaders/slang/common/sh_lighting.slang.
//
// Written out rather than imported because the point is to check that the
// PROJECTION matches the EVALUATOR: if both came from the same place, the pair
// could be consistently wrong and this would still pass.
glm::vec3 EvaluateSH(glm::vec3 n, const glm::vec4 sh[9]) {
  return glm::vec3(sh[0]) + glm::vec3(sh[1]) * n.y + glm::vec3(sh[2]) * n.z +
         glm::vec3(sh[3]) * n.x + glm::vec3(sh[4]) * (n.x * n.y) +
         glm::vec3(sh[5]) * (n.y * n.z) +
         glm::vec3(sh[6]) * (3.0f * n.z * n.z - 1.0f) +
         glm::vec3(sh[7]) * (n.x * n.z) +
         glm::vec3(sh[8]) * (n.x * n.x - n.y * n.y);
}

}  // namespace

TEST_CASE("ibl: cube faces map to the six axes at their centres", "[ibl]") {
  // The centre of each face must be its major axis. A rotated or swapped face
  // makes the prefiltered cube disagree with the source it was convolved from,
  // which reads as a lighting choice rather than a bug.
  const std::array<glm::vec3, 6> expected = {
      glm::vec3(1, 0, 0),  glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
      glm::vec3(0, -1, 0), glm::vec3(0, 0, 1),  glm::vec3(0, 0, -1)};
  for (uint32_t face = 0; face < 6; ++face) {
    const glm::vec3 d = ibl::FaceUVToDirection(face, 0.5f, 0.5f);
    INFO("face " << face);
    CHECK(d.x == Catch::Approx(expected[face].x).margin(1e-5));
    CHECK(d.y == Catch::Approx(expected[face].y).margin(1e-5));
    CHECK(d.z == Catch::Approx(expected[face].z).margin(1e-5));
  }
  // And every direction is unit, at the corners too.
  for (uint32_t face = 0; face < 6; ++face) {
    for (float u : {0.0f, 0.5f, 1.0f}) {
      for (float v : {0.0f, 0.5f, 1.0f}) {
        CHECK(glm::length(ibl::FaceUVToDirection(face, u, v)) ==
              Catch::Approx(1.0f).margin(1e-5));
      }
    }
  }
}

TEST_CASE("ibl: the sky gradient hits its three anchors", "[ibl]") {
  SkySettings sky;
  sky.zenith = {0.1f, 0.2f, 0.3f};
  sky.horizon = {0.4f, 0.5f, 0.6f};
  sky.ground = {0.7f, 0.8f, 0.9f};
  sky.intensity = 2.0f;

  const glm::vec3 up = ibl::EvaluateSky(sky, {0, 1, 0});
  CHECK(up.r == Catch::Approx(0.2f).margin(1e-5));  // zenith * intensity

  const glm::vec3 down = ibl::EvaluateSky(sky, {0, -1, 0});
  CHECK(down.r == Catch::Approx(1.4f).margin(1e-5));  // ground * intensity

  const glm::vec3 flat = ibl::EvaluateSky(sky, {1, 0, 0});
  CHECK(flat.r == Catch::Approx(0.8f).margin(1e-5));  // horizon * intensity

  // NO SUN DISC ANYWHERE. The cube feeds the prefilter and the SH, and a disc
  // in either double-counts the sun against the direct GGX term. If a disc is
  // ever added to the sky, this fails -- which is the point.
  float brightest = 0.0f;
  for (int i = 0; i < 2048; ++i) {
    const float t = float(i) / 2048.0f;
    const float inc = std::acos(1.0f - 2.0f * t);
    const float az = 2.4f * float(i);
    const glm::vec3 d{std::sin(inc) * std::cos(az), std::cos(inc),
                      std::sin(inc) * std::sin(az)};
    brightest = std::max(brightest, ibl::EvaluateSky(sky, d).r);
  }
  CHECK(brightest <= Catch::Approx(1.4f).margin(1e-4));
}

TEST_CASE("ibl: a constant environment projects to L0 alone", "[ibl]") {
  // The closed form: a constant radiance has no directional variation, so every
  // coefficient above L0 must vanish and the evaluator must return the constant
  // in every direction. This is what catches a projection that disagrees with
  // the evaluator's convolution convention -- both look like plausible ambient.
  const glm::vec3 kConstant{0.25f, 0.5f, 0.75f};
  glm::vec4 sh[9];
  ibl::ProjectIrradiance([&](glm::vec3) { return kConstant; }, sh, 4096);

  CHECK(sh[0].x == Catch::Approx(kConstant.x).margin(0.02));
  CHECK(sh[0].y == Catch::Approx(kConstant.y).margin(0.02));
  CHECK(sh[0].z == Catch::Approx(kConstant.z).margin(0.02));
  for (int i = 1; i < 9; ++i) {
    INFO("coefficient " << i);
    CHECK(std::abs(sh[i].x) < 0.02f);
    CHECK(std::abs(sh[i].y) < 0.02f);
    CHECK(std::abs(sh[i].z) < 0.02f);
  }

  for (glm::vec3 n : {glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1),
                      glm::normalize(glm::vec3(1, 1, 1))}) {
    const glm::vec3 got = EvaluateSH(n, sh);
    CHECK(got.x == Catch::Approx(kConstant.x).margin(0.03));
    CHECK(got.z == Catch::Approx(kConstant.z).margin(0.03));
  }
}

TEST_CASE("ibl: SH tracks a directional bias", "[ibl]") {
  // A sky bright above and dark below must evaluate brighter for an up-facing
  // normal than a down-facing one. A projection that dropped its L1 band still
  // passes the constant test above, so this is the case that catches it.
  glm::vec4 sh[9];
  ibl::ProjectIrradiance(
      [](glm::vec3 d) {
        return d.y > 0.0f ? glm::vec3(1.0f) : glm::vec3(0.05f);
      },
      sh, 4096);
  const float up = EvaluateSH({0, 1, 0}, sh).r;
  const float down = EvaluateSH({0, -1, 0}, sh).r;
  INFO("up " << up << " down " << down);
  CHECK(up > down + 0.2f);
  CHECK(down >= -0.05f);  // and it does not go negative, which ringing would
}

TEST_CASE("ibl: the cube fill covers every face and keeps its values", "[ibl]") {
  constexpr uint32_t kSize = 8;
  const auto texels = ibl::EvaluateCubeFaces(
      [](glm::vec3) { return glm::vec3(2.0f, 4.0f, 8.0f); }, kSize);
  REQUIRE(texels.size() == size_t(kSize) * kSize * 4 * 6);

  // Every texel of every face, not just the first: a fill that wrote one face
  // and left the rest zero has the right size and the wrong content.
  for (size_t i = 0; i < texels.size(); i += 4) {
    REQUIRE(glm::unpackHalf1x16(texels[i + 0]) == Catch::Approx(2.0f));
    REQUIRE(glm::unpackHalf1x16(texels[i + 1]) == Catch::Approx(4.0f));
    REQUIRE(glm::unpackHalf1x16(texels[i + 2]) == Catch::Approx(8.0f));
    REQUIRE(glm::unpackHalf1x16(texels[i + 3]) == Catch::Approx(1.0f));
  }

  // HDR survives the half-float round trip, which is the reason the cube is not
  // RGBA8. 8.0 in an 8-bit target is 1.0 and the specular headroom is gone.
  const auto bright = ibl::EvaluateCubeFaces(
      [](glm::vec3) { return glm::vec3(300.0f); }, 2);
  CHECK(glm::unpackHalf1x16(bright[0]) == Catch::Approx(300.0f).margin(1.0));
}

TEST_CASE("ibl: a zero-size or null cube fill is refused", "[ibl]") {
  CHECK(ibl::EvaluateCubeFaces([](glm::vec3) { return glm::vec3(1.0f); }, 0)
            .empty());
  CHECK(ibl::EvaluateCubeFaces({}, 8).empty());
}

TEST_CASE("ibl: the equirect mapping puts the poles on the poles", "[ibl]") {
  // A 4x2 image: the top row is the northern hemisphere, the bottom the
  // southern. Latitude flipped or longitude rotated is the classic silent
  // failure -- the environment simply faces the wrong way.
  const std::vector<float> texels = {
      // top row (v = 0, near +Y): four distinct reds
      1.0f, 0, 0,  2.0f, 0, 0,  3.0f, 0, 0,  4.0f, 0, 0,
      // bottom row (v = 1, near -Y)
      0, 1.0f, 0,  0, 2.0f, 0,  0, 3.0f, 0,  0, 4.0f, 0,
  };
  auto img = EquirectImage::FromTexels(4, 2, texels.data());
  REQUIRE(img);

  // Straight up must come from the TOP row, which is the red channel.
  const glm::vec3 up = img->Sample({0, 1, 0});
  CHECK(up.r > 0.5f);
  CHECK(up.g < 0.01f);

  // Straight down from the bottom row, the green one.
  const glm::vec3 down = img->Sample({0, -1, 0});
  CHECK(down.g > 0.5f);
  CHECK(down.r < 0.01f);

  // And longitude wraps rather than clamping: -Z and a hair either side of it
  // must not jump, which they would at a seam.
  const glm::vec3 a = img->Sample(glm::normalize(glm::vec3(0.01f, 0.2f, -1.0f)));
  const glm::vec3 b = img->Sample(glm::normalize(glm::vec3(-0.01f, 0.2f, -1.0f)));
  CHECK(std::abs(a.r - b.r) < 0.5f);
}

TEST_CASE("ibl: an unreadable environment is refused, not defaulted", "[ibl]") {
  CHECK(EquirectImage::Load("/nonexistent/nope.hdr") == nullptr);
  CHECK(EquirectImage::FromTexels(0, 0, nullptr) == nullptr);
}

// --- The GPU half ----------------------------------------------------------
//
// Needs a Metal device AND a Slang SDK, so these are tagged [gpu] and skipped
// where either is absent. Everything above runs anywhere.

namespace {

std::unique_ptr<badlands::slang::SlangCompiler> MakeIblCompiler() {
  const std::vector<std::string> paths = {"shaders/slang/ibl"};
  return badlands::slang::CreateSlangCompiler(paths);
}

// One texel of a face, read back as floats.
glm::vec3 ReadFaceTexel(rhi::IRhiDevice& device, rhi::ITexture* cube,
                        uint32_t mip, uint32_t face, uint32_t texel_index) {
  const uint32_t size = std::max(1u, cube->GetWidth() >> mip);
  auto readback = device.CreateBuffer(
      {.size = size_t(size) * size * 8,  // RGBA16Float
       .usage = rhi::BufferUsage::CopyDst | rhi::BufferUsage::MapRead,
       .label = "ibl_readback"});
  REQUIRE(readback);
  auto encoder = device.CreateCommandEncoder("ibl_read");
  encoder->Transition(cube, rhi::ResourceState::CopySrc);
  encoder->Transition(readback.get(), rhi::ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(cube, mip, face, readback.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();

  std::vector<uint8_t> bytes(size_t(size) * size * 8, 0);
  REQUIRE(readback->Read(0, bytes));
  const uint16_t* h = reinterpret_cast<const uint16_t*>(bytes.data());
  const size_t i = size_t(texel_index) * 4;
  return {glm::unpackHalf1x16(h[i]), glm::unpackHalf1x16(h[i + 1]),
          glm::unpackHalf1x16(h[i + 2])};
}

}  // namespace

TEST_CASE("ibl: the prefilter preserves a constant environment", "[ibl][gpu]") {
  // A constant environment is the one case with a closed form: convolving a
  // constant with ANY normalized kernel returns the constant. So every face of
  // every mip must come back at the input value -- which catches a broken
  // normalization, a source bound at the wrong mip, and a face index that does
  // not survive the trip through the params buffer.
  auto device = rhi::CreateDevice({.backend = rhi::BackendKind::Metal,
                                   .enable_validation = false,
                                   .label = "ibl_tests"});
  if (!device) return;  // no Metal here; the CPU cases still ran
  auto compiler = MakeIblCompiler();
  if (!compiler) return;  // no Slang SDK

  const glm::vec3 kRadiance{0.5f, 1.5f, 4.0f};  // deliberately above 1
  auto source = ibl::BuildEnvironmentCube(
      *device, [&](glm::vec3) { return kRadiance; }, 32);
  REQUIRE(source);
  auto sampler = device->CreateSampler(
      {.address_u = rhi::AddressMode::ClampToEdge,
       .address_v = rhi::AddressMode::ClampToEdge,
       .label = "ibl_test"});
  REQUIRE(sampler);

  auto prefiltered = PrefilteredCube::Create(*device, *compiler);
  REQUIRE(prefiltered);
  REQUIRE(prefiltered->Generate(source.get(), sampler.get()));

  for (uint32_t mip = 0; mip < PrefilteredCube::kMipCount; ++mip) {
    for (uint32_t face = 0; face < 6; ++face) {
      const glm::vec3 got = ReadFaceTexel(*device, prefiltered->Texture(), mip,
                                          face, 0);
      INFO("face " << face << " mip " << mip << " -> " << got.r << "," << got.g
                   << "," << got.b);
      CHECK(got.r == Catch::Approx(kRadiance.r).margin(0.02));
      CHECK(got.g == Catch::Approx(kRadiance.g).margin(0.05));
      // Above 1 must survive: an 8-bit target anywhere in the chain clamps
      // here, and the specular headroom is silently gone.
      CHECK(got.b == Catch::Approx(kRadiance.b).margin(0.15));
    }
  }
}

TEST_CASE("ibl: the prefilter blurs more at higher roughness", "[ibl][gpu]") {
  // Mip 0 is a mirror and must reproduce the source; the last mip is fully
  // rough and must not. A chain that convolved every mip at the same roughness
  // passes the constant-environment test above and fails this one.
  auto device = rhi::CreateDevice({.backend = rhi::BackendKind::Metal,
                                   .enable_validation = false,
                                   .label = "ibl_tests"});
  if (!device) return;
  auto compiler = MakeIblCompiler();
  if (!compiler) return;

  // Bright above, dark below -- a step the convolution has to smear.
  auto source = ibl::BuildEnvironmentCube(
      *device,
      [](glm::vec3 d) {
        return d.y > 0.0f ? glm::vec3(10.0f) : glm::vec3(0.0f);
      },
      32);
  REQUIRE(source);
  auto sampler = device->CreateSampler({.label = "ibl_test"});
  REQUIRE(sampler);
  auto prefiltered = PrefilteredCube::Create(*device, *compiler);
  REQUIRE(prefiltered);
  REQUIRE(prefiltered->Generate(source.get(), sampler.get()));

  // Face 2 is +Y, entirely in the bright hemisphere; face 3 is -Y, entirely
  // dark. At mip 0 they keep the step; at the roughest mip both have pulled
  // towards the average.
  const float sharp_up = ReadFaceTexel(*device, prefiltered->Texture(), 0, 2, 0).r;
  const float sharp_down = ReadFaceTexel(*device, prefiltered->Texture(), 0, 3, 0).r;
  const uint32_t last = PrefilteredCube::kMipCount - 1;
  const float rough_up = ReadFaceTexel(*device, prefiltered->Texture(), last, 2, 0).r;
  const float rough_down = ReadFaceTexel(*device, prefiltered->Texture(), last, 3, 0).r;

  INFO("mip0 up/down " << sharp_up << "/" << sharp_down << "  rough up/down "
                       << rough_up << "/" << rough_down);
  CHECK(sharp_up > 9.0f);
  CHECK(sharp_down < 1.0f);
  // The contrast must SHRINK with roughness. That is the monotonicity the whole
  // chain exists to provide.
  CHECK((rough_up - rough_down) < (sharp_up - sharp_down));
  CHECK(rough_down > sharp_down);
}

TEST_CASE("ibl: the BRDF LUT is generated and bounded", "[ibl][gpu]") {
  auto device = rhi::CreateDevice({.backend = rhi::BackendKind::Metal,
                                   .enable_validation = false,
                                   .label = "ibl_tests"});
  if (!device) return;
  auto compiler = MakeIblCompiler();
  if (!compiler) return;

  auto lut = ibl::BrdfLut::Create(*device, *compiler);
  REQUIRE(lut);
  REQUIRE(lut->Texture());

  auto readback = device->CreateBuffer(
      {.size = size_t(ibl::BrdfLut::kSize) * ibl::BrdfLut::kSize * 4,  // RG16F
       .usage = rhi::BufferUsage::CopyDst | rhi::BufferUsage::MapRead,
       .label = "lut_readback"});
  REQUIRE(readback);
  auto encoder = device->CreateCommandEncoder("lut_read");
  encoder->Transition(lut->Texture(), rhi::ResourceState::CopySrc);
  encoder->Transition(readback.get(), rhi::ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(lut->Texture(), 0, 0, readback.get(), 0);
  encoder->Finish();
  device->Submit(*encoder);
  device->WaitIdle();

  std::vector<uint8_t> bytes(size_t(ibl::BrdfLut::kSize) * ibl::BrdfLut::kSize * 4);
  REQUIRE(readback->Read(0, bytes));
  const uint16_t* h = reinterpret_cast<const uint16_t*>(bytes.data());

  // scale + bias is the fraction of specular energy that survives, so it lives
  // in [0, 1]. Outside that the split sum is creating or destroying energy, and
  // the white-furnace assertion downstream would report it as a lighting bug.
  float max_sum = 0.0f;
  bool any_nonzero = false;
  for (size_t i = 0; i < size_t(ibl::BrdfLut::kSize) * ibl::BrdfLut::kSize; ++i) {
    const float scale = glm::unpackHalf1x16(h[i * 2 + 0]);
    const float bias = glm::unpackHalf1x16(h[i * 2 + 1]);
    REQUIRE(scale >= -0.001f);
    REQUIRE(bias >= -0.001f);
    max_sum = std::max(max_sum, scale + bias);
    if (scale + bias > 0.01f) any_nonzero = true;
  }
  // A LUT of zeros would satisfy every bound above, so require it did something.
  CHECK(any_nonzero);
  CHECK(max_sum <= 1.02f);
}

TEST_CASE("ibl: the prefilter and the resolve agree on the mip", "[ibl]") {
  // THE invariant of the prefiltered chain: whatever roughness a mip was
  // convolved at, asking for that roughness must return that mip. A mismatch
  // is uniformly-too-sharp or uniformly-too-blurry with nothing to point at.
  for (uint32_t mip = 0; mip < PrefilteredCube::kMipCount; ++mip) {
    const float r = PrefilteredCube::RoughnessForMip(mip);
    CHECK(PrefilteredCube::MipForRoughness(r) ==
          Catch::Approx(float(mip)).margin(1e-5));
  }
  // The endpoints are the whole range: mirror at 0, roughest at the last mip.
  CHECK(PrefilteredCube::RoughnessForMip(0) == Catch::Approx(0.0f));
  CHECK(PrefilteredCube::RoughnessForMip(PrefilteredCube::kMipCount - 1) ==
        Catch::Approx(1.0f));
  CHECK(PrefilteredCube::MipForRoughness(1.0f) ==
        Catch::Approx(float(PrefilteredCube::kMipCount - 1)));
}
