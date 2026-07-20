#pragma once

// Water surface material factories (shaders/material/water.wesl). Two factories
// off one shader because the forward and G-buffer variants target different
// render-target formats (a single HDR colour target vs the G-buffer MRT), and
// FactoryDescriptor carries one color_formats set:
//   - Forward (transparent): renders into the HDR accumulation target with a
//     depth-read-only attachment and does NOT write depth. Attach with
//     MaterialPassType::kForwardTransparent; the forward-transparent pass binds
//     scene depth/colour + IBL at group 2.
//   - G-buffer (deferred): writes the analytic wave normal into the G-buffer.
//     Used by badlands_water_gpu_test (normal readback cross-check) and as a
//     normals debug view. Attach with MaterialPassType::kDeferred.
//
// Engine, game-agnostic.

#include <memory>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

std::unique_ptr<MaterialInstanceFactory> BuildWaterForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen);

std::unique_ptr<MaterialInstanceFactory> BuildWaterGBufferFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen);

// Default per-instance water look parameters (colours + absorption / refraction
// strength / coast width / roughness / detail strength). Override fields as
// desired before attaching. `time` is left at 0 and is driven by the engine.
InstanceParams DefaultWaterParams();

}  // namespace badlands
