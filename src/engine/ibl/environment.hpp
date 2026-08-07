#pragma once

// The environment cube: where the light in an IBL scene comes from.
//
// ONE INGEST, TWO SOURCES. A procedural analytic sky and a decoded equirect
// HDRI are both a `RadianceFn`, and the cube is filled from that. Adding a
// third source is a third function, not a third path.
//
// THE FILL IS CPU-SIDE, DELIBERATELY. The SH irradiance projection reads the
// same radiance function, so the sky has ONE implementation rather than a Slang
// copy and a C++ copy that agree until someone edits one. Mirrors
// src/engine/rendering/cubemap_builder.hpp, which is the proven Dawn-era
// version of this, and reuses its face convention exactly.
//
// THE CUBE NEVER CONTAINS THE SUN DISC, and this is the subtle rule.
//
// The sun reaches specular through the direct GGX term. Baking a disc into the
// environment as well counts it TWICE, and brightest exactly where a mirror
// shows it. So neither the prefilter nor the SH projection ever sees one; the
// background pass adds the disc analytically, because the sky you *look at* is
// not a lighting term. A mirror still shows a sun -- via the direct highlight,
// which is what supplies it at every roughness.
//
// (Excluding the disc from SH also avoids the Monte-Carlo blow-up a tiny solid
// angle causes, which is why light_environment.hpp excludes it too. That is now
// a second reason for one rule rather than a special case of its own.)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rhi/rhi_device.hpp"

namespace badlands::ibl {

// World-space unit direction -> linear RGB radiance.
//
// MUST BE THREAD-SAFE. The face evaluation runs across the shared pool, so a
// captured accumulation buffer or a non-reentrant RNG is a data race. Pure is
// the intended shape, exactly as CubemapBuilder::RadianceFn documents.
using RadianceFn = std::function<glm::vec3(glm::vec3 dir)>;

// The analytic sky: a zenith -> horizon -> ground gradient, and NO SUN DISC.
struct SkySettings {
  glm::vec3 zenith{0.45f, 0.62f, 0.95f};
  glm::vec3 horizon{0.80f, 0.85f, 0.92f};
  glm::vec3 ground{0.28f, 0.26f, 0.24f};
  float intensity = 1.0f;
};

// Pure, and safe to call from several threads at once.
glm::vec3 EvaluateSky(const SkySettings& sky, glm::vec3 dir);

// Binds `sky` by value, so the returned function stays valid and pure.
RadianceFn ProceduralSky(const SkySettings& sky);

// A decoded equirect HDRI, owning its texels.
class EquirectImage {
 public:
  // Returns null (after logging) if the file is missing or is not a Radiance
  // .hdr. A refusal, not a fallback to a default sky -- a silently substituted
  // environment reads as "the HDRI is dull" rather than "the HDRI is absent".
  static std::unique_ptr<EquirectImage> Load(const std::string& path);

  // Wraps already-decoded texels (`width * height * 3` floats, row-major from
  // the top). Load's second half, factored out so the direction->UV mapping can
  // be asserted without a file on disk -- the mapping is the part that silently
  // ROTATES an environment, and the decode itself is covered on the Rust side.
  static std::unique_ptr<EquirectImage> FromTexels(uint32_t width,
                                                   uint32_t height,
                                                   const float* rgb);

  // Bilinear, wrapping in longitude and clamped in latitude. Pure.
  glm::vec3 Sample(glm::vec3 dir) const;

  uint32_t Width() const { return width_; }
  uint32_t Height() const { return height_; }

 private:
  uint32_t width_ = 0, height_ = 0;
  std::vector<float> rgb_;  // width * height * 3
};

// Borrows `image`, which must outlive the returned function.
RadianceFn EquirectRadiance(const EquirectImage& image);

// Standard cube-map face + UV ([0,1], v = 0 at the TOP) -> unit direction.
//
// IDENTICAL to CubemapBuilder::FaceUVToDirection and to faceUVToDirection in
// the prefilter shader. All three must agree or a prefiltered cube is rotated
// with respect to the source it was convolved from, which looks like a lighting
// choice rather than a bug.
glm::vec3 FaceUVToDirection(uint32_t face, float u, float v);

inline constexpr uint32_t kEnvFaceSize = 128;

// Evaluates `fn` over all six faces into tightly packed RGBA half-floats,
// face-major. Exposed separately from the upload so the maths is testable with
// no GPU present.
//
// Returns empty (after logging) on a zero size or a null function.
std::vector<uint16_t> EvaluateCubeFaces(const RadianceFn& fn,
                                        uint32_t face_size);

// Builds and uploads the source cube: RGBA16Float, 6 faces, 1 mip.
rhi::TexturePtr BuildEnvironmentCube(rhi::IRhiDevice& device,
                                     const RadianceFn& fn,
                                     uint32_t face_size = kEnvFaceSize);

// SH-9 irradiance from the same radiance function, in the convention
// shaders/slang/common/sh_lighting.slang evaluates (diffuse convolution baked
// into the coefficients).
//
// Wraps sh::ProjectFunctionToSHL2 rather than reimplementing it: that header is
// glm-only and already produces this exact convention, and a second projection
// is a second chance to disagree with the evaluator.
void ProjectIrradiance(const RadianceFn& fn, glm::vec4 out_sh[9],
                       int sample_count = 2048);

}  // namespace badlands::ibl
