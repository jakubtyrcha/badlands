#pragma once

// Bakes models into an ImpostorAtlas -- the billboard level that ends a LOD
// chain.
//
// Runs at load, immediately after the CPU meshes exist -- not in the same loop
// as the producers, which are pure CPU and run in parallel across models, while
// this needs the device and runs serially.
//
// What it does per model, per octahedral view: renders every submesh the
// model's ImpostorBakeSpec names through an orthographic camera along that
// view's direction, into one tile of the atlas' two maps.
//
// WHICH submeshes is the producer's call, and that is the point of the spec.
// A tree names its LOD0 bark and its VOXEL crown -- not its leaf cards, since
// the field's own LOD0 is the voxel crown and an impostor baked from cards
// would be a picture of a tree the chain never draws. A prop names its one
// mesh, with its albedo texture bound.
//
// This used to take std::span<const TreeFieldModel> and read
// `options.leaves.*` and kTreeBarkColor directly.

#include <cstdint>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "game/visual/impostor_atlas.hpp"
#include "game/visual/instanced_lod_model.hpp"

namespace badlands {

class GpuPipelineGenerator;

struct ImpostorBakeResult {
  ImpostorAtlas atlas;
  std::vector<ImpostorPlacement> placement;  // one per model
  bool ok = false;
};

// Bakes every model into a fresh atlas.
//
// Takes whole models rather than bare specs: ImpostorBakeSubmesh names its
// geometry as (lod, submesh) INTO the model's own levels, so passing the model
// is what makes those references impossible to dangle or disagree with. The
// reference is range-checked by ValidateLodModel.
//
// Takes no wgpu::Instance: it derives one via device.GetAdapter().GetInstance()
// for the mip readback's event pump. RenderContext does not carry an instance,
// and widening it would be an engine interface change for something the device
// already knows.
//
// A model whose ImpostorBakeSpec is inactive, or all of whose named submeshes
// are empty, is a hard failure rather than a skip: its atlas layer would render
// as a hole at the impostor level with nothing in the log. Callers that do not
// want an impostor should not call this.
//
// Returns `ok = false` (after logging) on that, or on any pipeline, texture or
// readback failure.
ImpostorBakeResult BakeImpostorAtlas(wgpu::Device device, wgpu::Queue queue,
                                     GpuPipelineGenerator& pipeline_gen,
                                     std::span<const InstancedLodModel> models);

}  // namespace badlands
