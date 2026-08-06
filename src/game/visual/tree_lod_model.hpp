#pragma once

// The TREE producer: turns a TreeOptions into the neutral InstancedLodModel
// that BuildInstancedLodField and BakeImpostorAtlas consume.
//
// Everything tree-shaped lives on this side of the line -- two submeshes (bark
// and a voxelized crown), the bark decimation policy, the leaf tint and
// transmission, the solid bark colour. The layer below knows none of it; see
// game/visual/instanced_lod_model.hpp.
//
// This is what remains of the old tree_field.hpp after the field builder was
// generalized. The materials it names:
//   submesh 0  bark   instanced_gbuffer, solid colour via `tint`, Back-culled
//   submesh 1  crown  voxel_foliage, textureless, Back-culled (solid tets)
// both kDeferred and both shadow-casting.
//
// PURE CPU and device-free, so BuildForestModels can run it under ParallelFor
// across every model in a forest.

#include <glm/glm.hpp>

#include "game/geometry/tree_options.hpp"
#include "game/visual/instanced_lod_model.hpp"

namespace badlands {

// The solid colour every tree's bark renders with. Shared because the impostor
// bake has to reproduce it exactly -- if the two drift, a tree visibly changes
// hue at the impostor switch distance, which reads as a lighting bug rather
// than as a mismatched constant.
inline constexpr glm::vec3 kTreeBarkColor{0.30f, 0.19f, 0.10f};

// Matte roughness for the bark ARM support texture, the voxel-leaf material's
// `params.x`, and the impostor level. Same value as MaterialLibrary's local
// kFoliageRoughness convention (material_library.cpp).
inline constexpr float kTreeBarkRoughness = 0.9f;

// Which submesh of a tree's InstancedLodModel is which.
//
// Exported rather than file-local because the forest measures its spacing from
// the CROWN specifically (see forest_renderer.cpp's SilhouetteBounds) and rests
// instances on the BARK -- both of which need to name a submesh. A bare 0/1
// there would be the kind of literal that silently means the wrong thing after
// a submesh is inserted.
inline constexpr size_t kTreeBarkSubmesh = 0;
inline constexpr size_t kTreeCrownSubmesh = 1;

// The two material specs a tree's submeshes draw with, in submesh order:
//   0 bark   instanced_gbuffer, solid `tint`, no cull, shadow-casting
//   1 crown  voxel_foliage, textureless, back-culled, shadow-casting
//
// Exported so a caller assembling a tree model with its OWN LOD chain -- the
// GPU test suites pick bespoke cell sizes and thresholds to pin specific LOD
// behaviour -- still exercises the real materials rather than a stand-in that
// could drift from what ships.
std::vector<InstancedMaterialSpec> TreeSubmeshMaterials(
    const TreeOptions& options);

// Prepares ONE tree that stands `target_height_m` tall in the world: skeleton,
// bark decimated per level, and one voxelized crown per level of the standard
// foliage chain (kFoliageVoxelWorldSizes), with cell sizes and distance cutoffs
// both retargeted from the 8 m preview tree -- see FoliageLodThresholdsForHeight
// for why both scale by the same ratio.
//
// The bark decimation happens HERE, not in the field builder. It used to live
// there, which meant the builder held one specific model's LOD policy and no
// other kind of model could use it.
InstancedLodModel BuildTreeFieldModel(const TreeOptions& options,
                                      float target_height_m);

}  // namespace badlands
