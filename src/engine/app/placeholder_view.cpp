#include "engine/app/placeholder_view.hpp"

#include <utility>

#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/scene/scene_attachment.hpp"

namespace badlands {

namespace {

// Builds the sphere node in `scene`: textured_mesh geometry + material
// instance params referencing the sphere's albedo. Moved verbatim from
// Stage 1's src/main.cpp (build_test_scene()).
void BuildSphereScene(SceneGraph& scene, MaterialInstanceFactory* mat,
                      const InstanceParams& params) {
  auto sphere = GenerateSphereTexturedMesh(1.0f, 48);

  ResolvedMesh resolved_mesh{
      .vertices = std::move(sphere.mesh.vertices),
      .vertex_count = sphere.mesh.vertex_count,
      .geometry_type = sphere.mesh.geometry_type,
      .local_bounds = sphere.local_bounds,
  };

  auto node = scene.CreateNode("test_sphere");
  scene.AddAttachment(node, MeshAttachment{
      .mesh = std::move(resolved_mesh),
      .factory = mat,
      .pass_type = MaterialPassType::kDeferred,
      .params = params,
  });

  scene.SetSunDirection(glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)));
  scene.SetSunColor(glm::vec3(1.0f));
}

}  // namespace

void PlaceholderView::Initialize(const RenderContext& ctx) {
  // Sphere albedo (Stage 1 E1 loader): JPEG -> RGBA8 Dawn texture with a
  // GPU-generated mip chain. Kept alive for the whole view lifetime -- the
  // bind group ref-keeps the view+texture, but the owning handle stays in
  // scope regardless.
  albedo_ = LoadTexture2D(
      ctx.device, ctx.queue, *ctx.pipeline_gen,
      "assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg");
  if (!albedo_.texture) {
    spdlog::error(
        "PlaceholderView::Initialize: failed to load sphere albedo texture");
    return;
  }

  // Trilinear + anisotropic sampler: the material factory's default sampler
  // uses mipmapFilter=Nearest, which would defeat the GPU mip chain above.
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;  // trilinear
  samp_desc.addressModeU = wgpu::AddressMode::Repeat;  // UV sphere wraps in u
  samp_desc.addressModeV = wgpu::AddressMode::Repeat;
  samp_desc.maxAnisotropy = 16;  // grazing-angle sharpness
  sampler_ = ctx.device.CreateSampler(&samp_desc);

  // Build the normalmapped (deferred) material factory, targeting the
  // G-buffer's MRT color formats + reversed-Z depth so the kGBuffer pipeline
  // variant compiled through it matches what the G-buffer pass renders into.
  // Normal + roughness slots fall back to the factory's 1x1 defaults
  // (flat_normal / full_roughness); only albedo is overridden.
  FactoryDescriptor normalmapped_desc;
  normalmapped_desc.shader_name = "normalmapped";
  normalmapped_desc.shader_path = "material/normalmapped.wesl";
  normalmapped_desc.supported_pass_types = {MaterialPassType::kDeferred};
  normalmapped_desc.supported_geometry_types = {GeometryType::kTexturedMesh};
  normalmapped_desc.color_formats = {GBuffer::kNormalsFormat,
                                     GBuffer::kAlbedoFormat,
                                     GBuffer::kMaterialFormat};
  normalmapped_desc.depth_format = GBuffer::kDepthFormat;

  factory_ = BuildMaterialInstanceFactory(normalmapped_desc, ctx.device,
                                          ctx.queue, ctx.pipeline_gen,
                                          /*script_provider=*/nullptr);
  if (!factory_) {
    spdlog::error(
        "PlaceholderView::Initialize: failed to build normalmapped "
        "material factory");
    return;
  }

  InstanceParams sphere_params;
  sphere_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "albedo",
      .view = albedo_.view,
      .sampler = sampler_,
      .type = TextureType::k2D,
  });

  BuildSphereScene(scene_, factory_.get(), sphere_params);

  orbit_.FrameBounds(glm::vec3(0.0f), 1.0f);
  orbit_.UpdateCamera(camera_);
}

void PlaceholderView::HandleEvent(const SDL_Event& event, int /*width*/,
                                  int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = true;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (left_mouse_down_) {
        orbit_.HandleMouseDrag(event.motion.xrel, event.motion.yrel);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      orbit_.HandleMouseWheel(event.wheel.y);
      break;
    default:
      break;
  }
}

void PlaceholderView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
  orbit_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);
}

void PlaceholderView::DrawUI() {
  ImGui::Begin("badlands");
  ImGui::Text("%.1f FPS (%.2f ms)", dt_ > 0 ? 1.0f / dt_ : 0.0f, dt_ * 1000.0f);
  ImGui::End();
}

void PlaceholderView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height)
                : 1.0f;
}

}  // namespace badlands
