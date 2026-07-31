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

// The blockout/greybox water surface: a distinct forward-transparent factory
// (its own pipeline) driven by the same shader. The flat, no-reflection look is
// selected per instance by BlockoutWaterParams (params2.y = 1); kept a separate
// factory so the blockout and detailed water are swappable proxy materials and
// can diverge (e.g. its own shader) later.
std::unique_ptr<MaterialInstanceFactory> BuildWaterBlockoutForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen);

// Still (standing) water: the forward-transparent surface with the "still"
// shader feature compiled in -- no vertex wave displacement, a dead-flat +Y
// shading normal, and no detail perturbation. Everything else (the Beer-Lambert
// medium, the Fresnel sky reflection, the sun glint, shadowing) is the same
// shader. A separate factory rather than a runtime flag, matching the blockout
// water's precedent; the wave path stays compiled for the game.
//
// kForwardTransparent registers only RenderPassType::kForward, so this factory
// builds exactly ONE pipeline and the "still" feature never reaches the
// shadow_pass entry.
std::unique_ptr<MaterialInstanceFactory> BuildStillWaterForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen);

std::unique_ptr<MaterialInstanceFactory> BuildWaterGBufferFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen);

// Default per-instance water look parameters. Override fields as desired before
// attaching. `time` is left at 0 and is driven by the engine.
//
// The water body is a MEDIUM, not a tint (see water.wesl): `extinction` is a
// per-channel coefficient in 1/m and `scatterAlbedo` is what deep water
// asymptotes to. Extinction is always derived from a visibility depth --
// sigma = 3 / d_vis, i.e. the bed has faded by ~95% at d_vis metres -- never
// dialled in directly, so the numbers stay answerable ("you can see 8 m down").
InstanceParams DefaultWaterParams();

// Blockout water look: flat +Y shading normal + no sky reflection (params2.y=1),
// murk colour #264653, strong extinction so the depth gradient reads over a
// shallow greybox lake, and no refraction distortion. `time` is engine-driven.
InstanceParams BlockoutWaterParams();

// Murky standing lake water, calibrated to the Iron Water palette
// (/Users/jakub/repos/docs/palettes.html: water #41505a, deep water #2b3841,
// cold glint #6d8994). Extinction comes from the generator's measured lake
// bathymetry (median max_depth_m ~= 5 m across seeds 1-5 -> visibility depths
// 2.5 / 7 / 10 m); scatterAlbedo is fitted so the deep asymptote RENDERS as the
// deep-water swatch after exposure and tonemapping. Pair with
// BuildStillWaterForwardFactory.
InstanceParams StillLakeWaterParams();

}  // namespace badlands
