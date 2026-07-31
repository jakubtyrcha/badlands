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

std::unique_ptr<MaterialInstanceFactory> BuildWaterBlockoutForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen) {
  // Same forward-transparent descriptor as the detailed water: identical shader,
  // pass, targets, and (crucially) @group(2) layout — the flat/no-reflection
  // look is a per-instance uniform (BlockoutWaterParams), not a pipeline
  // difference. A separate factory only so the two are swappable materials.
  FactoryDescriptor desc = BaseWaterDescriptor();
  desc.supported_pass_types = {MaterialPassType::kForwardTransparent};
  desc.color_formats = {SceneRenderer::kAccumulationFormat};  // HDR
  desc.depth_write = false;
  return BuildMaterialInstanceFactory(desc, device, queue, pipeline_gen);
}

std::unique_ptr<MaterialInstanceFactory> BuildStillWaterForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen) {
  // Same forward-transparent descriptor as the detailed water, plus the "still"
  // shader feature. kForwardTransparent registers only RenderPassType::kForward
  // (standard_material_factory.cpp kVariants), so this builds exactly one
  // pipeline -- "still" and "shadow_pass" are never both defined.
  FactoryDescriptor desc = BaseWaterDescriptor();
  desc.supported_pass_types = {MaterialPassType::kForwardTransparent};
  desc.color_formats = {SceneRenderer::kAccumulationFormat};  // HDR
  desc.depth_write = false;
  desc.extra_features = {"still"};
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
      // Clear-ish open water: visibility depths ~ (7.5, 19, 25) m, so the bed
      // stays readable a long way down and the hue creeps blue-green with
      // depth. sigma = 3 / d_vis.
      {"extinction", glm::vec4(0.40f, 0.16f, 0.12f, 0.0f)},
      {"scatterAlbedo", glm::vec4(0.15f, 0.35f, 0.42f, 0.0f)},
      // x=refractStrength, y=roughness, z/w reserved
      {"params", glm::vec4(0.03f, 0.06f, 0.0f, 0.0f)},
      {"params2", glm::vec4(0.15f, 0.0f, 0.0f, 0.0f)},  // x=detailStrength
      {"time", 0.0f},
  };
  return params;
}

InstanceParams BlockoutWaterParams() {
  InstanceParams params;
  params.uniform_overrides = {
      // Visibility depths ~ (1.0, 1.4, 1.7) m: the greybox lake reads as tinted
      // water (its shape stays legible) over the light debug lake-bottom, while
      // still showing a gradient across the first couple of metres.
      {"extinction", glm::vec4(3.0f, 2.2f, 1.8f, 0.0f)},
      // The murk asymptote, #264653 (web sRGB) converted to linear -- the shader
      // treats these as linear.
      {"scatterAlbedo", glm::vec4(0.019f, 0.065f, 0.091f, 0.0f)},
      // x=refractStrength (0: no distortion), y=roughness (matte).
      {"params", glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)},
      // x=detailStrength (unused when flat), y=1 -> FLAT (flat normal, no IBL).
      {"params2", glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)},
      {"time", 0.0f},
  };
  return params;
}

InstanceParams StillLakeWaterParams() {
  InstanceParams params;
  params.uniform_overrides = {
      // Visibility depths (2.5, 7.0, 10.0) m -> sigma = 3 / d_vis. Derived from
      // the generator's measured bathymetry: median lake max_depth_m ~= 5 m
      // across seeds 1-5. Red dies first, so the water turns blue as it deepens.
      {"extinction", glm::vec4(1.20f, 0.43f, 0.30f, 0.0f)},
      // Fitted against Iron Water's deep-water swatch #2b3841 = (43, 56, 65).
      // Measured deep asymptote: (43, 72, 89) -- red lands exactly, green and
      // blue sit above. That residual is NOT slack in this constant: with the
      // albedo driven to zero the surface still renders (16, 58, 76), because
      // the Fresnel sky reflection alone is brighter and bluer than the swatch
      // in those two channels. The palette section is specified for OVERCAST
      // light; mapview renders a clear Hosek-Wilkie sky at noon, which really
      // does put more blue on the water. Closing the gap would mean either a
      // dimmer sky or an unphysical Fresnel, so the hue ratio is matched
      // (0.65:0.78:1.0 against the swatch's 0.66:0.86:1.0) and the brightness
      // is left where the lighting puts it.
      {"scatterAlbedo", glm::vec4(0.0048f, 0.0058f, 0.0074f, 0.0f)},
      // x=refractStrength (inert: a still surface has N.xz == 0),
      // y=roughness -- low, for the palette's tight "cold glint".
      {"params", glm::vec4(0.0f, 0.05f, 0.0f, 0.0f)},
      {"params2", glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)},
      {"time", 0.0f},
  };
  return params;
}

}  // namespace badlands
