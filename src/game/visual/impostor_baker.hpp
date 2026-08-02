#pragma once

// Bakes tree models into an ImpostorAtlas (volumetric-foliage LOD4).
//
// Runs at load, immediately after BuildForestModels has produced the CPU
// meshes -- "along the voxels", though not in the same loop: voxelization is
// pure CPU and runs in parallel across models, while this needs the device and
// runs serially after it.
//
// What it does per model, per octahedral view: renders the LOD0 bark and the
// VOXEL L0 CROWN through an orthographic camera along that view's direction,
// into one tile of the atlas' two maps.
//
// The crown, NOT the leaf cards. The field's own LOD0 is the voxel crown -- it
// never draws cards -- so an impostor baked from cards would be a picture of a
// tree the chain does not show, and the L2 -> L4 switch would change the tree's
// appearance rather than only its cost.

#include <cstdint>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "game/visual/impostor_atlas.hpp"
#include "game/visual/tree_field.hpp"

namespace badlands {

class GpuPipelineGenerator;

struct ImpostorBakeResult {
  ImpostorAtlas atlas;
  std::vector<ImpostorPlacement> placement;  // one per model
  bool ok = false;
};

// Bakes every model into a fresh atlas.
//
// Takes no wgpu::Instance: it derives one via device.GetAdapter().GetInstance()
// for the mip readback's event pump. RenderContext does not carry an instance,
// and widening it would be an engine interface change for something the device
// already knows.
//
// Returns `ok = false` (after logging) on any pipeline, texture or readback
// failure. A model whose bark mesh is empty is a hard failure, not a skip: an
// unbaked layer renders as a hole at LOD4 with nothing in the log.
ImpostorBakeResult BakeImpostorAtlas(wgpu::Device device, wgpu::Queue queue,
                                     GpuPipelineGenerator& pipeline_gen,
                                     std::span<const TreeFieldModel> models);

}  // namespace badlands
