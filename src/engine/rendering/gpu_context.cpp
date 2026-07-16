#include "engine/rendering/gpu_context.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

namespace badlands {

bool g_gpu_error_flag = false;

namespace {
void PlatformSleep(int ms) { SDL_Delay(ms); }
}  // namespace

GpuContext::GpuContext() = default;
GpuContext::~GpuContext() = default;
GpuContext::GpuContext(GpuContext&&) noexcept = default;
GpuContext& GpuContext::operator=(GpuContext&&) noexcept = default;

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

wgpu::Surface GpuContext::CreateSurface(wgpu::Instance instance,
                                        SDL_Window* window) {
#if defined(SDL_PLATFORM_MACOS)
  SDL_MetalView view = SDL_Metal_CreateView(window);
  if (!view) {
    std::fprintf(stderr, "Failed to create SDL Metal view: %s\n",
                 SDL_GetError());
    return nullptr;
  }
  void* layer = SDL_Metal_GetLayer(view);
  if (!layer) {
    std::fprintf(stderr, "Failed to get Metal layer: %s\n", SDL_GetError());
    return nullptr;
  }

  WGPUSurfaceSourceMetalLayer metalDesc = {};
  metalDesc.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
  metalDesc.layer = layer;
  wgpu::SurfaceDescriptor desc;
  desc.nextInChain = reinterpret_cast<const wgpu::ChainedStruct*>(&metalDesc);
  return instance.CreateSurface(&desc);
#else
  std::fprintf(stderr,
               "Surface creation not implemented for this platform yet.\n");
  return nullptr;
#endif
}

wgpu::Adapter GpuContext::RequestAdapter(
    wgpu::Instance instance, wgpu::RequestAdapterOptions const* options) {
  struct UserData {
    wgpu::Adapter adapter = nullptr;
    bool request_ended = false;
  };
  UserData user_data;

  instance.RequestAdapter(
      options, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter,
         wgpu::StringView message, UserData* data) {
        if (status == wgpu::RequestAdapterStatus::Success) {
          data->adapter = std::move(adapter);
        } else {
          std::fprintf(stderr, "Could not get WebGPU adapter: %s\n",
                       message.length > 0 ? message.data : "Unknown error");
        }
        data->request_ended = true;
      },
      &user_data);
  while (!user_data.request_ended) PlatformSleep(10);
  return user_data.adapter;
}

wgpu::Device GpuContext::RequestDevice(
    wgpu::Instance instance, wgpu::Adapter adapter,
    wgpu::DeviceDescriptor const* descriptor) {
  struct UserData {
    wgpu::Device device = nullptr;
    bool request_ended = false;
  };
  UserData user_data;

  (void)instance;
  adapter.RequestDevice(
      descriptor, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::RequestDeviceStatus status, wgpu::Device device,
         wgpu::StringView message, UserData* data) {
        if (status == wgpu::RequestDeviceStatus::Success) {
          data->device = std::move(device);
        } else {
          std::fprintf(stderr, "Could not get WebGPU device: %s\n",
                       message.length > 0 ? message.data : "Unknown error");
        }
        data->request_ended = true;
      },
      &user_data);
  while (!user_data.request_ended) PlatformSleep(10);
  return user_data.device;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool GpuContext::Initialize(SDL_Window* window) {
  wgpu::InstanceDescriptor instance_desc = {};
  instance_ = wgpu::CreateInstance(&instance_desc);
  if (!instance_) {
    std::fprintf(stderr, "Could not initialize WebGPU instance!\n");
    return false;
  }

  surface_ = CreateSurface(instance_, window);
  if (!surface_) {
    std::fprintf(stderr, "Could not create surface!\n");
    return false;
  }

  wgpu::RequestAdapterOptions adapter_opts;
  adapter_opts.compatibleSurface = surface_;
  adapter_ = RequestAdapter(instance_, &adapter_opts);
  if (!adapter_) {
    std::fprintf(stderr, "Could not get WebGPU adapter!\n");
    return false;
  }

  wgpu::DeviceDescriptor device_desc;
  device_desc.label = "GpuContext Device";

  // Opportunistically request TextureFormatsTier1 (Dawn's current name for
  // the R8Unorm storage-texture capability the old standalone
  // `R8UnormStorage` feature exposed) — Stage 3 M6's GTAO compute pass
  // writes an R8Unorm storage texture and needs it. Never required: an
  // adapter that lacks a requested feature fails device creation outright,
  // so this is gated on adapter_.HasFeature and simply omitted otherwise;
  // HasR8UnormStorage() reports the outcome post-creation.
  std::vector<wgpu::FeatureName> required_features;
  if (adapter_.HasFeature(wgpu::FeatureName::TextureFormatsTier1)) {
    required_features.push_back(wgpu::FeatureName::TextureFormatsTier1);
  }
  // Opportunistically request TimestampQuery for GPU per-pass timing (GpuTimer,
  // used by SceneRenderer when the profiler is enabled). Same gating rationale
  // as TextureFormatsTier1: never required, omitted if the adapter lacks it.
  if (adapter_.HasFeature(wgpu::FeatureName::TimestampQuery)) {
    required_features.push_back(wgpu::FeatureName::TimestampQuery);
  }
  if (!required_features.empty()) {
    device_desc.requiredFeatureCount = required_features.size();
    device_desc.requiredFeatures = required_features.data();
  }

  device_desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        std::fprintf(stderr, "WebGPU Error: %d - %s\n",
                     static_cast<int>(type),
                     message.length > 0 ? message.data : "Unknown");
        g_gpu_error_flag = true;
      });

  // AllowSpontaneous so Dawn invokes this the moment the device is lost
  // (including normal teardown/Destroy), not only inside an explicit
  // ProcessEvents pump. Setting it on the descriptor also silences Dawn's
  // "No Dawn device lost callback was set" warning.
  device_desc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason,
         wgpu::StringView message) {
        if (reason == wgpu::DeviceLostReason::FailedCreation) {
          std::string_view msg(message.data, message.length);
          std::fprintf(stderr, "WebGPU device lost (FailedCreation): %.*s\n",
                       static_cast<int>(msg.size()), msg.data());
        }
      });

  device_ = RequestDevice(instance_, adapter_, &device_desc);
  if (!device_) {
    std::fprintf(stderr, "Could not get WebGPU device!\n");
    return false;
  }

  has_r8unorm_storage_ =
      device_.HasFeature(wgpu::FeatureName::TextureFormatsTier1);
  spdlog::info("GpuContext: HasR8UnormStorage() = {}", has_r8unorm_storage_);

  has_timestamp_query_ = device_.HasFeature(wgpu::FeatureName::TimestampQuery);
  spdlog::info("GpuContext: HasTimestampQuery() = {}", has_timestamp_query_);

  queue_ = device_.GetQueue();
  surface_format_ = GetPreferredFormat();

  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window, &w, &h);
  Configure(static_cast<uint32_t>(w), static_cast<uint32_t>(h));

  return true;
}

void GpuContext::Shutdown() {
  surface_ = nullptr;
  queue_ = nullptr;
  device_ = nullptr;
  adapter_ = nullptr;
  instance_ = nullptr;
}

// ---------------------------------------------------------------------------
// Surface management
// ---------------------------------------------------------------------------

wgpu::TextureFormat GpuContext::GetPreferredFormat() {
  wgpu::SurfaceCapabilities capabilities;
  surface_.GetCapabilities(adapter_, &capabilities);

  if (capabilities.formatCount > 0) {
    return static_cast<wgpu::TextureFormat>(capabilities.formats[0]);
  }
  return wgpu::TextureFormat::BGRA8Unorm;
}

void GpuContext::Configure(uint32_t width, uint32_t height) {
  wgpu::SurfaceConfiguration config = {};
  config.device = device_;
  config.format = surface_format_;
  config.width = width;
  config.height = height;
  config.usage = wgpu::TextureUsage::RenderAttachment;
  config.presentMode = present_mode_;
  config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
  surface_.Configure(&config);
}

wgpu::TextureView GpuContext::AcquireSurfaceTexture() {
  wgpu::SurfaceTexture surface_texture = {};
  surface_.GetCurrentTexture(&surface_texture);

  if (surface_texture.status !=
          wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
      surface_texture.status !=
          wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
    std::fprintf(stderr, "Failed to acquire surface texture: %d\n",
                 static_cast<int>(surface_texture.status));
    return nullptr;
  }

  return surface_texture.texture.CreateView();
}

void GpuContext::Present() { surface_.Present(); }

}  // namespace badlands
