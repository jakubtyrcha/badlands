#include "engine/rendering/water_material.hpp"

#include <glm/glm.hpp>

#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/scene_renderer.hpp"

namespace badlands {

namespace {

FactoryDescriptor BaseWaterDescriptor() {
  FactoryDescriptor desc;
  desc.shader_name = "water";
  desc.shader_path = "material/water.wesl";
  desc.depth_format = SceneRenderer::kDepthFormat;  // == GBuffer::kDepthFormat
  return desc;
}

}  // namespace

std::unique_ptr<MaterialInstanceFactory> BuildWaterForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen) {
  FactoryDescriptor desc = BaseWaterDescriptor();
  desc.supported_pass_types = {MaterialPassType::kForwardTransparent};
  desc.color_formats = {SceneRenderer::kAccumulationFormat};  // HDR
  desc.depth_write = false;  // tests depth, never writes it (read-only attachment)
  return BuildMaterialInstanceFactory(desc, device, queue, pipeline_gen);
}

std::unique_ptr<MaterialInstanceFactory> BuildWaterGBufferFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen) {
  FactoryDescriptor desc = BaseWaterDescriptor();
  desc.supported_pass_types = {MaterialPassType::kDeferred};
  desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                        GBuffer::kMaterialFormat};
  desc.depth_write = true;
  return BuildMaterialInstanceFactory(desc, device, queue, pipeline_gen);
}

InstanceParams DefaultWaterParams() {
  InstanceParams params;
  params.uniform_overrides = {
      {"deepColor", glm::vec4(0.02f, 0.09f, 0.16f, 1.0f)},
      {"shallowColor", glm::vec4(0.15f, 0.35f, 0.42f, 1.0f)},
      // x=absorption, y=refractStrength, z=coastWidth, w=roughness
      {"params", glm::vec4(0.12f, 0.03f, 1.5f, 0.06f)},
      {"params2", glm::vec4(0.15f, 0.0f, 0.0f, 0.0f)},  // x=detailStrength
      {"time", 0.0f},
  };
  return params;
}

}  // namespace badlands
