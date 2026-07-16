// Ported from sampo/tests/readback_tests.cpp. Exercises the four render-test
// primitives ported from sampo: CpuImage, TextureReadback, ColorRenderTarget,
// and the headless gpu_test_helpers. Requires a working GPU adapter (Metal on
// macOS); there is no CI, so this runs locally via ctest.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <catch_amalgamated.hpp>

#include "core/util/cpu_image.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;
using badlands::test::RequestAdapter;
using badlands::test::RequestDevice;

TEST_CASE("Readback matches clear color for empty frame", "[readback]") {
  // 1. Initialize WebGPU (headless)
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  // 2. Create render target (small size for fast test)
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 64;
  ColorRenderTarget target(device, kWidth, kHeight,
                           wgpu::TextureFormat::BGRA8Unorm);
  REQUIRE(target.IsValid());

  // 3. Create a render pass that just clears to red
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = target.GetView();
  color_attachment.loadOp = wgpu::LoadOp::Clear;
  color_attachment.storeOp = wgpu::StoreOp::Store;
  color_attachment.clearValue = {1.0, 0.0, 0.0, 1.0};  // Red
#ifndef __EMSCRIPTEN__
  color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;

  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
  pass.End();

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // Wait for GPU to finish render pass
  bool work_done = false;
  queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::AllowProcessEvents,
      [&work_done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
        work_done = true;
        REQUIRE(status == wgpu::QueueWorkDoneStatus::Success);
      });

  while (!work_done) {
    instance.ProcessEvents();
    device.Tick();
  }

  // 4. Read back
  TextureReadback readback(instance, device, queue);
  CpuImage image = readback.ReadTextureSync(target.GetTexture(), kWidth,
                                            kHeight, target.GetFormat());

  REQUIRE(image.GetWidth() == kWidth);
  REQUIRE(image.GetHeight() == kHeight);

  // 5. Verify all pixels are red
  // GetPixel returns RGBA (handles BGRA conversion internally)
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      auto pixel = image.GetPixel(x, y);
      INFO("Pixel at (" << x << ", " << y << "): r=" << (int)pixel.r
                        << " g=" << (int)pixel.g << " b=" << (int)pixel.b
                        << " a=" << (int)pixel.a);
      REQUIRE(pixel.r == 255);
      REQUIRE(pixel.g == 0);
      REQUIRE(pixel.b == 0);
      REQUIRE(pixel.a == 255);
    }
  }
}

// Historically flaky on macOS/Dawn: depth copy from GPU has returned zeros on
// some driver versions. Kept as a hard assert; tag [!mayfail] if it regresses.
TEST_CASE("Depth buffer readback with Depth32Float", "[readback][depth]") {
  // 1. Initialize WebGPU (headless)
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  // 2. Create depth texture (with all required fields like ColorRenderTarget)
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 64;

  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {kWidth, kHeight, 1};
  tex_desc.format = wgpu::TextureFormat::Depth32Float;
  tex_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  tex_desc.mipLevelCount = 1;
  tex_desc.sampleCount = 1;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture depth_texture = device.CreateTexture(&tex_desc);
  REQUIRE(depth_texture);

  wgpu::TextureViewDescriptor view_desc;
  view_desc.format = wgpu::TextureFormat::Depth32Float;
  view_desc.dimension = wgpu::TextureViewDimension::e2D;
  view_desc.mipLevelCount = 1;
  view_desc.arrayLayerCount = 1;
  wgpu::TextureView depth_view = depth_texture.CreateView(&view_desc);
  REQUIRE(depth_view);

  // 3. Create a dummy color target (some drivers need this)
  wgpu::TextureDescriptor color_tex_desc;
  color_tex_desc.size = {kWidth, kHeight, 1};
  color_tex_desc.format = wgpu::TextureFormat::BGRA8Unorm;
  color_tex_desc.usage = wgpu::TextureUsage::RenderAttachment;
  wgpu::Texture color_texture = device.CreateTexture(&color_tex_desc);
  wgpu::TextureView color_view = color_texture.CreateView();

  // 4. Clear depth to a known value (0.5 - middle of range)
  // In reversed-Z: 1.0 = near, 0.0 = far
  constexpr float kClearDepth = 0.5f;

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = color_view;
  color_attachment.loadOp = wgpu::LoadOp::Clear;
  color_attachment.storeOp = wgpu::StoreOp::Discard;
  color_attachment.clearValue = {0.0, 0.0, 0.0, 1.0};
#ifndef __EMSCRIPTEN__
  color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

  wgpu::RenderPassDepthStencilAttachment depth_attachment;
  depth_attachment.view = depth_view;
  depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
  depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
  depth_attachment.depthClearValue = kClearDepth;

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;
  desc.depthStencilAttachment = &depth_attachment;

  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
  pass.End();

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // Wait for GPU
  bool work_done = false;
  queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::AllowProcessEvents,
      [&work_done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
        work_done = true;
        REQUIRE(status == wgpu::QueueWorkDoneStatus::Success);
      });

  while (!work_done) {
    instance.ProcessEvents();
    device.Tick();
  }

  // 4. Read the depth texture directly through TextureReadback.
  //
  // Note: an earlier version of this test went via an intermediate R32Float
  // texture using CopyTextureToTexture(Depth32Float -> R32Float,
  // aspect=DepthOnly). On the Dawn/Metal backend that copy silently produces
  // zero-filled output with no validation error; production code reads depth
  // via a sampling shader rather than texture-to-texture copy. The readback
  // path itself supports reading Depth32Float directly with aspect=DepthOnly,
  // so we exercise that path and ignore the broken texture-to-texture copy.
  TextureReadback readback(instance, device, queue);
  CpuImage image = readback.ReadTextureSync(depth_texture, kWidth, kHeight,
                                            wgpu::TextureFormat::Depth32Float);

  REQUIRE(image.GetWidth() == kWidth);
  REQUIRE(image.GetHeight() == kHeight);
  REQUIRE(image.GetFormat() == wgpu::TextureFormat::Depth32Float);

  // 5. Debug: Print first few raw bytes and depth values
  INFO("Image format: " << static_cast<int>(image.GetFormat()));
  INFO("Bytes per pixel: " << image.GetBytesPerPixel());
  INFO("Row pitch: " << image.GetRowPitch());

  // Print first 16 bytes as hex
  const uint8_t* data = image.GetData();
  std::stringstream hex_stream;
  for (size_t i = 0; i < std::min(size_t(16), image.GetDataSize()); ++i) {
    hex_stream << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(data[i]) << " ";
  }
  INFO("First 16 bytes: " << hex_stream.str());

  // Print first depth value
  float first_depth = image.GetDepth(0, 0);
  INFO("First depth value: " << first_depth);

  // Verify depth values
  constexpr float kEpsilon = 0.0001f;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      float depth = image.GetDepth(x, y);
      INFO("Depth at (" << x << ", " << y << "): " << depth);
      REQUIRE(std::abs(depth - kClearDepth) < kEpsilon);
    }
  }
}

TEST_CASE("Async readback completes correctly", "[readback][async]") {
  // 1. Initialize WebGPU (headless)
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  // 2. Create render target
  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kHeight = 32;
  ColorRenderTarget target(device, kWidth, kHeight,
                           wgpu::TextureFormat::BGRA8Unorm);
  REQUIRE(target.IsValid());

  // 3. Clear to green
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = target.GetView();
  color_attachment.loadOp = wgpu::LoadOp::Clear;
  color_attachment.storeOp = wgpu::StoreOp::Store;
  color_attachment.clearValue = {0.0, 1.0, 0.0, 1.0};  // Green
#ifndef __EMSCRIPTEN__
  color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color_attachment;

  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
  pass.End();

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // 4. Start async readback and await result
  TextureReadback readback(instance, device, queue);
  auto future = readback.ReadTexture(target.GetTexture());
  CpuImage result_image = future.Await();

  REQUIRE(result_image.GetWidth() == kWidth);
  REQUIRE(result_image.GetHeight() == kHeight);

  // 6. Verify pixel colors
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      auto pixel = result_image.GetPixel(x, y);
      REQUIRE(pixel.r == 0);
      REQUIRE(pixel.g == 255);
      REQUIRE(pixel.b == 0);
      REQUIRE(pixel.a == 255);
    }
  }
}

TEST_CASE("CpuImage GetDepth/SetDepth work correctly", "[readback][cpuimage]") {
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 4;

  CpuImage image(kWidth, kHeight, wgpu::TextureFormat::Depth32Float);

  // Set some depth values
  image.SetDepth(0, 0, 0.0f);
  image.SetDepth(1, 0, 0.25f);
  image.SetDepth(2, 0, 0.5f);
  image.SetDepth(3, 0, 1.0f);

  // Verify
  REQUIRE(image.GetDepth(0, 0) == 0.0f);
  REQUIRE(image.GetDepth(1, 0) == 0.25f);
  REQUIRE(image.GetDepth(2, 0) == 0.5f);
  REQUIRE(image.GetDepth(3, 0) == 1.0f);

  // Out of bounds returns 0
  REQUIRE(image.GetDepth(100, 100) == 0.0f);
}

TEST_CASE("Buffer mapping roundtrip", "[readback][buffer]") {
  // Verify basic buffer write and readback works
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  constexpr size_t kBufferSize = 256;
  constexpr uint8_t kTestByte = 0xAB;

  // Create a buffer with known data
  wgpu::BufferDescriptor write_buffer_desc;
  write_buffer_desc.size = kBufferSize;
  write_buffer_desc.usage =
      wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer write_buffer = device.CreateBuffer(&write_buffer_desc);

  // Write test data
  std::vector<uint8_t> test_data(kBufferSize, kTestByte);
  queue.WriteBuffer(write_buffer, 0, test_data.data(), test_data.size());

  // Create staging buffer for readback
  wgpu::BufferDescriptor read_buffer_desc;
  read_buffer_desc.size = kBufferSize;
  read_buffer_desc.usage =
      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer read_buffer = device.CreateBuffer(&read_buffer_desc);

  // Copy from write buffer to read buffer
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
  encoder.CopyBufferToBuffer(write_buffer, 0, read_buffer, 0, kBufferSize);
  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // Map and read back
  bool mapping_done = false;
  bool mapping_success = false;

  read_buffer.MapAsync(wgpu::MapMode::Read, 0, kBufferSize,
                       wgpu::CallbackMode::AllowProcessEvents,
                       [&mapping_done, &mapping_success](
                           wgpu::MapAsyncStatus status, wgpu::StringView) {
                         mapping_done = true;
                         mapping_success =
                             (status == wgpu::MapAsyncStatus::Success);
                       });

  while (!mapping_done) {
    instance.ProcessEvents();
    device.Tick();
  }

  REQUIRE(mapping_success);

  const void* mapped_data = read_buffer.GetConstMappedRange(0, kBufferSize);
  REQUIRE(mapped_data != nullptr);

  const uint8_t* bytes = static_cast<const uint8_t*>(mapped_data);
  for (size_t i = 0; i < kBufferSize; ++i) {
    REQUIRE(bytes[i] == kTestByte);
  }

  read_buffer.Unmap();
}

TEST_CASE("Direct texture to buffer copy", "[readback][texcopy]") {
  // Test copyTextureToBuffer directly with BGRA8 (like the working color test)
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 4;
  constexpr uint8_t kTestR = 0xAB;

  // Create BGRA8Unorm texture exactly like ColorRenderTarget
  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {kWidth, kHeight, 1};
  tex_desc.format = wgpu::TextureFormat::BGRA8Unorm;
  tex_desc.usage = wgpu::TextureUsage::RenderAttachment |
                   wgpu::TextureUsage::CopySrc |
                   wgpu::TextureUsage::TextureBinding;
  tex_desc.mipLevelCount = 1;
  tex_desc.sampleCount = 1;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture texture = device.CreateTexture(&tex_desc);
  REQUIRE(texture);

  // Use render pass clear instead of writeTexture (like working test)
  wgpu::TextureView texture_view = texture.CreateView();

  wgpu::CommandEncoder clear_encoder = device.CreateCommandEncoder();

  wgpu::RenderPassColorAttachment color_attachment;
  color_attachment.view = texture_view;
  color_attachment.loadOp = wgpu::LoadOp::Clear;
  color_attachment.storeOp = wgpu::StoreOp::Store;
  // Clear to a recognizable color (R=171/255 ~ 0.67)
  color_attachment.clearValue = {static_cast<double>(kTestR) / 255.0, 0.0, 0.0,
                                 1.0};
#ifndef __EMSCRIPTEN__
  color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

  wgpu::RenderPassDescriptor render_pass_desc;
  render_pass_desc.colorAttachmentCount = 1;
  render_pass_desc.colorAttachments = &color_attachment;

  wgpu::RenderPassEncoder pass =
      clear_encoder.BeginRenderPass(&render_pass_desc);
  pass.End();

  wgpu::CommandBuffer clear_commands = clear_encoder.Finish();
  queue.Submit(1, &clear_commands);

  // Wait for render pass to complete
  {
    bool work_done = false;
    queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowProcessEvents,
        [&work_done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
          work_done = true;
        });
    while (!work_done) {
      instance.ProcessEvents();
      device.Tick();
    }
  }

  wgpu::Extent3D extent = {kWidth, kHeight, 1};

  // Create staging buffer (with 256-byte aligned row pitch)
  size_t aligned_row_pitch = 256;  // 256-byte aligned
  size_t buffer_size = aligned_row_pitch * kHeight;

  wgpu::BufferDescriptor buffer_desc;
  buffer_desc.size = buffer_size;
  buffer_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer staging_buffer = device.CreateBuffer(&buffer_desc);

  // Copy texture to buffer
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::TexelCopyTextureInfo src;
  src.texture = texture;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};
  src.aspect = wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferInfo dst;
  dst.buffer = staging_buffer;
  dst.layout.offset = 0;
  dst.layout.bytesPerRow = static_cast<uint32_t>(aligned_row_pitch);
  dst.layout.rowsPerImage = kHeight;

  encoder.CopyTextureToBuffer(&src, &dst, &extent);

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // Map and read
  bool mapping_done = false;
  bool mapping_success = false;

  staging_buffer.MapAsync(wgpu::MapMode::Read, 0, buffer_size,
                          wgpu::CallbackMode::AllowProcessEvents,
                          [&mapping_done, &mapping_success](
                              wgpu::MapAsyncStatus status, wgpu::StringView) {
                            mapping_done = true;
                            mapping_success =
                                (status == wgpu::MapAsyncStatus::Success);
                          });

  while (!mapping_done) {
    instance.ProcessEvents();
    device.Tick();
  }

  REQUIRE(mapping_success);

  const void* mapped_data = staging_buffer.GetConstMappedRange(0, buffer_size);
  REQUIRE(mapped_data != nullptr);

  // Check first row of bytes (BGRA format)
  const uint8_t* bytes = static_cast<const uint8_t*>(mapped_data);
  for (uint32_t x = 0; x < kWidth; ++x) {
    size_t offset = x * 4;
    INFO("Pixel[" << x << "] BGRA: " << static_cast<int>(bytes[offset]) << " "
                  << static_cast<int>(bytes[offset + 1]) << " "
                  << static_cast<int>(bytes[offset + 2]) << " "
                  << static_cast<int>(bytes[offset + 3]));
    REQUIRE(bytes[offset + 2] == kTestR);  // R channel
  }

  staging_buffer.Unmap();
}

// Historically flaky on macOS/Dawn: writeTexture + copyTextureToBuffer for
// R32Float has returned zeros on some driver versions. Tag [!mayfail] if it
// regresses.
TEST_CASE("R32Float texture readback", "[readback][float]") {
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 4;

  // Create R32Float texture with known data written to it
  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {kWidth, kHeight, 1};
  tex_desc.format = wgpu::TextureFormat::R32Float;
  tex_desc.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
  wgpu::Texture texture = device.CreateTexture(&tex_desc);
  REQUIRE(texture);

  // Write known float values to the texture
  constexpr float kTestValue = 0.75f;
  std::vector<float> src_data(kWidth * kHeight, kTestValue);

  wgpu::TexelCopyTextureInfo dst;
  dst.texture = texture;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};

  wgpu::TexelCopyBufferLayout src_layout;
  src_layout.offset = 0;
  src_layout.bytesPerRow = kWidth * sizeof(float);
  src_layout.rowsPerImage = kHeight;

  wgpu::Extent3D extent = {kWidth, kHeight, 1};
  queue.WriteTexture(&dst, src_data.data(), src_data.size() * sizeof(float),
                     &src_layout, &extent);

  // Wait for write to complete
  bool work_done = false;
  queue.OnSubmittedWorkDone(
      wgpu::CallbackMode::AllowProcessEvents,
      [&work_done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
        work_done = true;
      });

  while (!work_done) {
    instance.ProcessEvents();
    device.Tick();
  }

  // Read back
  TextureReadback readback(instance, device, queue);
  CpuImage image = readback.ReadTextureSync(texture, kWidth, kHeight,
                                            wgpu::TextureFormat::R32Float);

  REQUIRE(image.GetWidth() == kWidth);
  REQUIRE(image.GetHeight() == kHeight);

  // Debug output
  INFO("Format: " << static_cast<int>(image.GetFormat()));
  INFO("Bytes per pixel: " << image.GetBytesPerPixel());
  INFO("Row pitch: " << image.GetRowPitch());

  // Verify values
  constexpr float kEpsilon = 0.0001f;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      float value = image.GetDepth(x, y);
      INFO("Value at (" << x << ", " << y << "): " << value);
      REQUIRE(std::abs(value - kTestValue) < kEpsilon);
    }
  }
}

TEST_CASE("GetBytesPerPixel returns correct values", "[readback][format]") {
  REQUIRE(GetBytesPerPixel(wgpu::TextureFormat::Depth32Float) == 4);
  REQUIRE(GetBytesPerPixel(wgpu::TextureFormat::R32Float) == 4);
  REQUIRE(GetBytesPerPixel(wgpu::TextureFormat::BGRA8Unorm) == 4);
  REQUIRE(GetBytesPerPixel(wgpu::TextureFormat::RGBA8Unorm) == 4);
  REQUIRE(GetBytesPerPixel(wgpu::TextureFormat::R8Unorm) == 1);
}

// Simple test shader - fullscreen triangle, constant fragment color.
constexpr const char* kSimpleTestShader = R"(
struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VertexOutput {
    var positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    var output: VertexOutput;
    output.Position = vec4<f32>(positions[vid], 0.5, 1.0);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    return vec4<f32>(0.75, 0.25, 0.1, 1.0);
}
)";

TEST_CASE("Simple fullscreen draw test", "[readback][fullscreen_draw]") {
  // 1. Initialize WebGPU (headless)
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 64;

  // 2. Create output texture (with TextureBinding like ColorRenderTarget)
  wgpu::TextureDescriptor output_tex_desc;
  output_tex_desc.size = {kWidth, kHeight, 1};
  output_tex_desc.format = wgpu::TextureFormat::BGRA8Unorm;
  output_tex_desc.usage = wgpu::TextureUsage::RenderAttachment |
                          wgpu::TextureUsage::CopySrc |
                          wgpu::TextureUsage::TextureBinding;
  output_tex_desc.mipLevelCount = 1;
  output_tex_desc.sampleCount = 1;
  output_tex_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture output_texture = device.CreateTexture(&output_tex_desc);
  REQUIRE(output_texture);

  wgpu::TextureView output_view = output_texture.CreateView();

  // 3. Create simple test shader module (no bindings)
  WGPUShaderSourceWGSL wgsl_desc = {};
  wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl_desc.code = WGPUStringView{.data = kSimpleTestShader,
                                  .length = strlen(kSimpleTestShader)};

  wgpu::ShaderModuleDescriptor shader_desc;
  shader_desc.nextInChain = reinterpret_cast<wgpu::ChainedStruct*>(&wgsl_desc);
  wgpu::ShaderModule shader_module = device.CreateShaderModule(&shader_desc);
  REQUIRE(shader_module);

  // 4. Create empty pipeline layout (no bindings)
  wgpu::PipelineLayoutDescriptor pl_desc;
  pl_desc.bindGroupLayoutCount = 0;
  pl_desc.bindGroupLayouts = nullptr;
  wgpu::PipelineLayout pipeline_layout = device.CreatePipelineLayout(&pl_desc);

  // 5. Create render pipeline
  wgpu::ColorTargetState color_target;
  color_target.format = wgpu::TextureFormat::BGRA8Unorm;
  color_target.writeMask = wgpu::ColorWriteMask::All;

  wgpu::FragmentState fragment_state;
  fragment_state.module = shader_module;
  fragment_state.entryPoint = WGPUStringView{.data = "fs_main", .length = 7};
  fragment_state.targetCount = 1;
  fragment_state.targets = &color_target;

  wgpu::RenderPipelineDescriptor pipeline_desc;
  pipeline_desc.layout = pipeline_layout;
  pipeline_desc.vertex.module = shader_module;
  pipeline_desc.vertex.entryPoint =
      WGPUStringView{.data = "vs_main", .length = 7};
  pipeline_desc.fragment = &fragment_state;
  pipeline_desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  pipeline_desc.primitive.cullMode = wgpu::CullMode::None;  // Disable culling
  pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;

  wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipeline_desc);
  REQUIRE(pipeline);

  // 6. Clear to green first
  {
    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = output_view;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.clearValue = {0.0, 1.0, 0.0, 1.0};  // Green
#ifndef __EMSCRIPTEN__
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.End();

    wgpu::CommandBuffer commands = encoder.Finish();
    queue.Submit(1, &commands);
  }

  // Wait for clear
  {
    bool work_done = false;
    queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowProcessEvents,
        [&work_done](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
          work_done = true;
        });
    while (!work_done) {
      instance.ProcessEvents();
      device.Tick();
    }
  }

  // 6b. Now draw to overwrite green
  {
    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

    wgpu::RenderPassColorAttachment color_attachment;
    color_attachment.view = output_view;
    color_attachment.loadOp = wgpu::LoadOp::Load;
    color_attachment.storeOp = wgpu::StoreOp::Store;
#ifndef __EMSCRIPTEN__
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color_attachment;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
    pass.SetViewport(0, 0, static_cast<float>(kWidth),
                     static_cast<float>(kHeight), 0.0f, 1.0f);
    pass.SetScissorRect(0, 0, kWidth, kHeight);
    pass.SetPipeline(pipeline);
    pass.Draw(3, 1, 0, 0);
    pass.End();

    wgpu::CommandBuffer commands = encoder.Finish();
    queue.Submit(1, &commands);
  }

  // 7. Wait for GPU
  {
    bool work_done = false;
    queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowProcessEvents,
        [&work_done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
          work_done = true;
        });
    while (!work_done) {
      instance.ProcessEvents();
      device.Tick();
    }
  }

  // 8. Read back BGRA8 texture
  TextureReadback readback(instance, device, queue);
  CpuImage image = readback.ReadTextureSync(output_texture, kWidth, kHeight,
                                            wgpu::TextureFormat::BGRA8Unorm);

  REQUIRE(image.GetWidth() == kWidth);
  REQUIRE(image.GetHeight() == kHeight);

  // 9. Debug output
  auto first_pixel = image.GetPixel(0, 0);
  INFO("First pixel RGBA: " << (int)first_pixel.r << " " << (int)first_pixel.g
                            << " " << (int)first_pixel.b << " "
                            << (int)first_pixel.a);
  INFO("If draw worked: R~191, G~64, B~26, A=255");

  // 10. Check if we got anything at all (at least the alpha should be 255)
  REQUIRE(first_pixel.a == 255);

  // If alpha is correct, check if draw worked (red) or just clear (green)
  if (first_pixel.r > 100) {
    // Draw worked - expect shader output
    constexpr uint8_t kExpectedR = 191;  // 0.75 * 255
    REQUIRE(std::abs((int)first_pixel.r - (int)kExpectedR) <= 1);
  } else if (first_pixel.g > 200) {
    // Only clear worked - draw failed
    INFO("Draw FAILED - got clear color instead");
    REQUIRE(false);  // Fail the test to indicate draw didn't work
  }
}

TEST_CASE("ReadTextureSync blocks without spinning",
          "[readback][await][!mayfail]") {
  // 1. Initialize WebGPU (headless)
  wgpu::InstanceDescriptor instance_desc = {};
  wgpu::Instance instance = wgpu::CreateInstance(&instance_desc);
  REQUIRE(instance);

  wgpu::Adapter adapter = RequestAdapter(instance);
  REQUIRE(adapter);

  wgpu::Device device = RequestDevice(adapter);
  REQUIRE(device);

  wgpu::Queue queue = device.GetQueue();
  REQUIRE(queue);

  // 2. Create depth texture
  constexpr uint32_t kWidth = 32;
  constexpr uint32_t kHeight = 32;

  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {kWidth, kHeight, 1};
  tex_desc.format = wgpu::TextureFormat::Depth32Float;
  tex_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture depth_texture = device.CreateTexture(&tex_desc);
  REQUIRE(depth_texture);

  wgpu::TextureViewDescriptor view_desc;
  view_desc.format = wgpu::TextureFormat::Depth32Float;
  view_desc.dimension = wgpu::TextureViewDimension::e2D;
  view_desc.mipLevelCount = 1;
  view_desc.arrayLayerCount = 1;
  wgpu::TextureView depth_view = depth_texture.CreateView(&view_desc);
  REQUIRE(depth_view);

  // 3. Clear depth
  constexpr float kClearDepth = 0.75f;

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::RenderPassDepthStencilAttachment depth_attachment;
  depth_attachment.view = depth_view;
  depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
  depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
  depth_attachment.depthClearValue = kClearDepth;

  wgpu::RenderPassDescriptor desc;
  desc.colorAttachmentCount = 0;
  desc.colorAttachments = nullptr;
  desc.depthStencilAttachment = &depth_attachment;

  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
  pass.End();

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // 4. Use ReadTextureSync (blocking without spinning)
  TextureReadback readback(instance, device, queue);
  CpuImage image = readback.ReadTextureSync(depth_texture, kWidth, kHeight,
                                            wgpu::TextureFormat::Depth32Float);

  REQUIRE(image.GetWidth() == kWidth);
  REQUIRE(image.GetHeight() == kHeight);

  // 5. Verify depth values
  constexpr float kEpsilon = 0.0001f;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      float depth = image.GetDepth(x, y);
      REQUIRE(std::abs(depth - kClearDepth) < kEpsilon);
    }
  }
}

// Badlands-specific (no sampo equivalent): CpuImage PNG I/O routes through the
// Rust `assets` crate (badlands_write_png / badlands_decode_image) rather than
// stb, so cover the write->read roundtrip end to end. Pure CPU, no GPU.
TEST_CASE("CpuImage WritePng/LoadFromFile roundtrip",
          "[readback][cpuimage][io]") {
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 3;

  CpuImage src(kWidth, kHeight, wgpu::TextureFormat::RGBA8Unorm);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      // Alpha forced to 255 (WritePng always writes opaque).
      src.SetPixel(x, y,
                   {static_cast<uint8_t>(x * 40u),
                    static_cast<uint8_t>(y * 60u),
                    static_cast<uint8_t>((x + y) * 20u), 255});
    }
  }

  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               "badlands_cpu_image_roundtrip.png";

  REQUIRE(src.WritePng(path.string()));

  auto loaded = CpuImage::LoadFromFile(path.string());
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->GetWidth() == kWidth);
  REQUIRE(loaded->GetHeight() == kHeight);
  REQUIRE(loaded->GetFormat() == wgpu::TextureFormat::RGBA8Unorm);

  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      auto a = src.GetPixel(x, y);
      auto b = loaded->GetPixel(x, y);
      INFO("Pixel (" << x << ", " << y << ")");
      REQUIRE(b.r == a.r);
      REQUIRE(b.g == a.g);
      REQUIRE(b.b == a.b);
      REQUIRE(b.a == 255);
    }
  }

  std::error_code ec;
  std::filesystem::remove(path, ec);
}
