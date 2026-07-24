// GPU tests for the resolve-pass UI-overlay composite (tonemapping.wesl +
// SceneContext::ui_overlay).
//
// The UI (game UI + ImGui) is authored for GAMMA-SPACE blending: on the old
// 8-bit surface, hardware alpha-blending operated on sRGB-encoded values. On
// the P3/HDR float surface blending is linear, which visibly brightens
// translucent panels and AA'd glyph edges. The fix routes UI into an 8-bit
// premultiplied overlay texture that the resolve composites over the scene in
// encoded (gamma) space before the P3 conversion. These tests pin that
// composite against a CPU mirror.
//
// Rendered through SceneRenderer into an offscreen R32Float target (the decal
// tests' approach: TextureReadback does not support RGBA16Float, and R32Float
// takes the outputIsLinear path, so channel values arrive at the CPU
// unchanged). The scene is empty — Pass 3's clear paints clear_color, giving
// every pixel a known linear scene value.

#include <cmath>
#include <memory>

#include <catch_amalgamated.hpp>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;

// Process-lifetime headless GPU context (same rationale + leak as the decal
// harness).
struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen;
};

TestGpu& GetTestGpu() {
  static TestGpu* instance = [] {
    auto* g = new TestGpu();
    wgpu::InstanceDescriptor instance_desc = {};
    g->instance = wgpu::CreateInstance(&instance_desc);
    if (!g->instance) {
      spdlog::error("resolve_composite_tests: CreateInstance failed");
      std::abort();
    }
    wgpu::Adapter adapter = badlands::test::RequestAdapter(g->instance);
    if (!adapter) {
      spdlog::error("resolve_composite_tests: RequestAdapter failed");
      std::abort();
    }
    g->device = badlands::test::RequestDevice(adapter);
    if (!g->device) {
      spdlog::error("resolve_composite_tests: RequestDevice failed");
      std::abort();
    }
    g->queue = g->device.GetQueue();
    g->pipeline_gen =
        std::make_unique<GpuPipelineGenerator>(g->device, FindShaderDirectory());
    return g;
  }();
  return *instance;
}

// --- CPU mirrors of the shader math (color.wesl / colorspace.wesl) ---

float SrgbToLinear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
  if (c <= 0.0031308f) return c * 12.92f;
  return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Red row of colorspace.wesl's SRGB_TO_P3 (column-major constructor: red-out
// takes 0.8224619688 * r + 0.1775380312 * g + 0.0 * b).
float SrgbLinearToP3Red(float r, float g) {
  return 0.8224619688f * r + 0.1775380312f * g;
}

// The composite the resolve must perform for a premultiplied, sRGB-encoded
// overlay pixel over a linear scene value (single channel):
//   blend_enc = ui_enc + linear_to_srgb(clamp(scene)) * (1 - a)
//   result    = srgb_to_linear(blend_enc)
float ExpectedCompositeLinear(float scene_linear, float ui_enc, float ui_a) {
  const float scene_enc =
      LinearToSrgb(std::fmin(std::fmax(scene_linear, 0.0f), 1.0f));
  const float blend_enc = ui_enc + scene_enc * (1.0f - ui_a);
  return SrgbToLinear(blend_enc);
}

// A kWidth x kHeight RGBA8 overlay filled with one premultiplied, encoded
// RGBA byte value.
wgpu::TextureView MakeOverlay(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  TestGpu& gpu = GetTestGpu();
  wgpu::TextureDescriptor desc;
  desc.size = {kWidth, kHeight, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  wgpu::Texture tex = gpu.device.CreateTexture(&desc);

  std::vector<uint8_t> pixels(kWidth * kHeight * 4);
  for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
    pixels[i * 4 + 0] = r;
    pixels[i * 4 + 1] = g;
    pixels[i * 4 + 2] = b;
    pixels[i * 4 + 3] = a;
  }
  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = tex;
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = kWidth * 4;
  layout.rowsPerImage = kHeight;
  wgpu::Extent3D extent = {kWidth, kHeight, 1};
  gpu.queue.WriteTexture(&dst, pixels.data(), pixels.size(), &layout, &extent);
  return tex.CreateView();
}

Camera MakeCamera() {
  Camera camera;
  camera.position = glm::vec3(0.0f, 10.0f, 10.0f);
  camera.LookAt(glm::vec3(0.0f));
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 45.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;
  return camera;
}

// Renders an EMPTY scene (every pixel = clear_color, linear) through the full
// SceneRenderer with the given overlay + P3 mode, and returns the readback.
CpuImage RenderResolveFrame(glm::vec4 clear_color, wgpu::TextureView ui_overlay,
                            bool output_is_p3) {
  TestGpu& gpu = GetTestGpu();

  entt::registry registry;
  SceneContext scene_context;
  scene_context.registry = &registry;
  scene_context.clear_color = clear_color;
  scene_context.ui_overlay = ui_overlay;

  ColorRenderTarget rt(gpu.device, kWidth, kHeight,
                       wgpu::TextureFormat::R32Float);

  SceneRenderer renderer;
  renderer.Initialize(gpu.device, gpu.queue, gpu.pipeline_gen.get(),
                      wgpu::TextureFormat::R32Float, kWidth, kHeight,
                      gpu.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback
  renderer.SetOutputIsP3(output_is_p3);

  renderer.Render(MakeCamera(), registry, scene_context, rt.GetView());
  badlands::test::WaitForGpu(gpu.instance, gpu.device, gpu.queue);

  TextureReadback readback(gpu.instance, gpu.device, gpu.queue);
  return readback.ReadTextureSync(rt.GetTexture(), kWidth, kHeight,
                                  wgpu::TextureFormat::R32Float);
}

float CenterRed(const CpuImage& image) {
  return image.GetDepth(kWidth / 2, kHeight / 2);
}

constexpr float kTolerance = 2e-3f;  // 8-bit overlay quantization + f16 HDR

}  // namespace

TEST_CASE("Surface mode: float format falls back to 8-bit when P3 tagging fails") {
  // An untagged (nil-colorspace) float CAMetalLayer has no defined transfer —
  // the resolve would emit linear values into a layer nothing reasons about.
  // The post-tagging decision must therefore drop a float surface whose P3
  // tagging failed back to BGRA8Unorm (today's known-good untagged path).
  using wgpu::TextureFormat;

  // The failure case that motivates the function.
  CHECK(GpuContext::ResolveSurfaceFormatAfterTagging(
            TextureFormat::RGBA16Float, /*tag_ok=*/false) ==
        TextureFormat::BGRA8Unorm);

  // Tagging succeeded: the float (EDR) surface stands.
  CHECK(GpuContext::ResolveSurfaceFormatAfterTagging(
            TextureFormat::RGBA16Float, /*tag_ok=*/true) ==
        TextureFormat::RGBA16Float);

  // 8-bit surfaces are safe untagged (they ARE the pre-P3 path): no change
  // either way — in particular no fallback loop.
  CHECK(GpuContext::ResolveSurfaceFormatAfterTagging(
            TextureFormat::BGRA8Unorm, /*tag_ok=*/false) ==
        TextureFormat::BGRA8Unorm);
  CHECK(GpuContext::ResolveSurfaceFormatAfterTagging(
            TextureFormat::BGRA8Unorm, /*tag_ok=*/true) ==
        TextureFormat::BGRA8Unorm);
}

TEST_CASE("Resolve composite: null overlay leaves the scene untouched") {
  // Baseline both resolve modes against a bare scene value: mode 0/linear is
  // a passthrough; mode 2/linear applies only the P3 primary conversion —
  // including EDR values > 1.0, which must not be clamped.
  const float scene_red = 2.0f;
  const glm::vec4 clear(scene_red, 0.0f, 0.0f, 1.0f);

  const CpuImage plain = RenderResolveFrame(clear, nullptr, /*p3=*/false);
  CHECK(std::abs(CenterRed(plain) - scene_red) < kTolerance);

  const CpuImage p3 = RenderResolveFrame(clear, nullptr, /*p3=*/true);
  const float expected_p3 = SrgbLinearToP3Red(scene_red, 0.0f);
  INFO("p3 red = " << CenterRed(p3) << " expected " << expected_p3);
  CHECK(std::abs(CenterRed(p3) - expected_p3) < kTolerance);
}

TEST_CASE("Resolve composite: translucent overlay blends in gamma space") {
  // THE brightness bug's oracle. A 50%-alpha dark-red overlay over a mid-gray
  // scene must produce the gamma-space blend (what 8-bit hardware blending
  // did), NOT the linear-space blend the float surface's hardware blending
  // produced. The two differ by far more than the tolerance.
  const float scene_red = 0.25f;  // linear
  const uint8_t ui_byte = 128;    // premultiplied, encoded
  const uint8_t ui_alpha = 128;
  const float ui_enc = static_cast<float>(ui_byte) / 255.0f;
  const float ui_a = static_cast<float>(ui_alpha) / 255.0f;

  const CpuImage image =
      RenderResolveFrame(glm::vec4(scene_red, 0.0f, 0.0f, 1.0f),
                         MakeOverlay(ui_byte, 0, 0, ui_alpha), /*p3=*/false);

  const float expected = ExpectedCompositeLinear(scene_red, ui_enc, ui_a);
  // The WRONG (linear-blend) answer, for contrast: enc values decoded first,
  // then blended linearly.
  const float wrong_linear_blend =
      SrgbToLinear(ui_enc) + scene_red * (1.0f - ui_a);
  INFO("read " << CenterRed(image) << " expected(gamma) " << expected
               << " wrong(linear) " << wrong_linear_blend);
  REQUIRE(std::abs(expected - wrong_linear_blend) > 10.0f * kTolerance);
  CHECK(std::abs(CenterRed(image) - expected) < kTolerance);
}

TEST_CASE("Resolve composite: overlay composites before the P3 conversion") {
  // Under UI the scene clamps to SDR (as the old 8-bit path did) and the
  // blended result — not the raw scene — gets the P3 primary conversion.
  const float scene_red = 2.0f;  // EDR-bright; clamps to 1.0 under the UI
  const uint8_t ui_byte = 128;
  const uint8_t ui_alpha = 128;
  const float ui_enc = static_cast<float>(ui_byte) / 255.0f;
  const float ui_a = static_cast<float>(ui_alpha) / 255.0f;

  const CpuImage image =
      RenderResolveFrame(glm::vec4(scene_red, 0.0f, 0.0f, 1.0f),
                         MakeOverlay(ui_byte, 0, 0, ui_alpha), /*p3=*/true);

  const float blended = ExpectedCompositeLinear(scene_red, ui_enc, ui_a);
  const float expected = SrgbLinearToP3Red(blended, 0.0f);
  INFO("read " << CenterRed(image) << " expected " << expected);
  CHECK(std::abs(CenterRed(image) - expected) < kTolerance);
}

TEST_CASE("Resolve composite: opaque overlay fully replaces the scene") {
  // a = 255: the scene contributes nothing; a mid-gray UI pixel decodes to
  // its linear value (P3 of gray is gray — the matrix rows sum to 1).
  const uint8_t gray = 128;
  const float expected = SrgbToLinear(static_cast<float>(gray) / 255.0f);

  const CpuImage image =
      RenderResolveFrame(glm::vec4(0.9f, 0.0f, 0.0f, 1.0f),
                         MakeOverlay(gray, gray, gray, 255), /*p3=*/true);
  INFO("read " << CenterRed(image) << " expected " << expected);
  CHECK(std::abs(CenterRed(image) - expected) < kTolerance);
}
