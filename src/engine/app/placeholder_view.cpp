#include "engine/app/placeholder_view.hpp"

#include <utility>

#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/components/mesh_components.hpp"
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
      .pass_type = MaterialPassType::kForwardOpaque,
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

  // Build the textured_mesh material factory, targeting the forward pass's
  // render-target formats (SceneRenderer's fixed HDR + reversed-Z depth
  // formats) so pipelines compiled through it match what the forward pass
  // actually renders into.
  FactoryDescriptor textured_mesh_desc;
  textured_mesh_desc.shader_name = "textured_mesh";
  textured_mesh_desc.shader_path = "material/textured_mesh.wesl";
  textured_mesh_desc.supported_pass_types = {MaterialPassType::kForwardOpaque};
  textured_mesh_desc.supported_geometry_types = {GeometryType::kTexturedMesh};
  textured_mesh_desc.color_formats = {SceneRenderer::kAccumulationFormat};
  textured_mesh_desc.depth_format = SceneRenderer::kDepthFormat;

  factory_ = BuildMaterialInstanceFactory(textured_mesh_desc, ctx.device,
                                          ctx.queue, ctx.pipeline_gen,
                                          /*script_provider=*/nullptr);
  if (!factory_) {
    spdlog::error(
        "PlaceholderView::Initialize: failed to build textured_mesh "
        "material factory");
    return;
  }

  InstanceParams sphere_params;
  sphere_params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "mesh_texture",
      .view = albedo_.view,
      .sampler = sampler_,
      .type = TextureType::k2D,
  });

  BuildSphereScene(scene_, factory_.get(), sphere_params);

  camera_.position = glm::vec3(0.0f, 1.5f, 4.0f);
  camera_.LookAt(glm::vec3(0.0f));
}

void PlaceholderView::HandleEvent(const SDL_Event& /*event*/, int /*width*/,
                                  int /*height*/) {}

void PlaceholderView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
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
