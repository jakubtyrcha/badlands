#pragma once

// The PROP producer: turns an imported USD mesh plus its PBR pack into the
// neutral InstancedLodModel that BuildInstancedLodField and BakeImpostorAtlas
// consume.
//
// One submesh, drawn with `instanced_gbuffer` against the pack's real
// albedo/normal/ARM. TRIANGLE LODs, from meshoptimizer -- voxelization stays
// reserved for foliage, whose crowns are card clouds rather than surfaces.
//
// The counterpart of tree_lod_model.hpp: both produce the same type, and the
// field builder and impostor baker below them know neither.

#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/geometry/usd_mesh_adapter.hpp"  // ImportedModel
#include "game/visual/instanced_lod_model.hpp"
#include "game/visual/lod_screen_space.hpp"

namespace badlands {

// The pack's three maps, already decoded and uploaded.
//
// Views rather than a pack directory: MaterialLibrary owns the decode, the mip
// chain and the DX->GL normal flip, and re-doing any of that here would be a
// second implementation of a thing that already exists. The caller pulls these
// out of MaterialLibrary::Get(dir) -- see the viewer.
//
// A null view is legal and falls back to the material's registered default
// (white / flat_normal / white), which is why instanced_gbuffer's entry in
// material_requirements.cpp matters: before it existed the normal slot's
// default was white, decoding to a ~54 degree tilt.
struct PropMaterialTextures {
  wgpu::TextureView albedo;
  wgpu::TextureView normal;
  wgpu::TextureView arm;
  wgpu::Sampler sampler;
};

struct PropLodOptions {
  LodLadderOptions ladder;
  // Levels at or below this index use error-bounded edge collapse
  // (SimplifyMesh), which preserves the silhouette; coarser ones use vertex
  // clustering (SimplifyMeshSloppy), which ignores topology and so actually
  // reaches an aggressive target on a mesh of separate shells. Same policy
  // split, for the same reason, as the tree's bark.
  size_t last_edge_collapse_level = 2;
  // Impostor surface roughness -- the atlas has no roughness channel, so a
  // textured prop's ARM cannot follow it to that level.
  float impostor_roughness = 0.7f;
};

// Builds one prop's LOD chain.
//
// EVERY level, LOD 0 included, is re-welded on position+UV and has its normals
// and tangents regenerated. Welding is mandatory for the coarser levels to
// decimate at all (four of the ten shipped props are flat-shaded, so no two of
// their vertices are alike and edge collapse cannot start -- see
// WeldMeshByPrefix). Applying it at LOD 0 as well is a deliberate choice: every
// level then shades identically, so there is no faceted-to-smooth pop at the
// first switch, and boulder_01's vertex buffer falls from 198,336 to ~33,000.
//
// Returns a model with a single level and no impostor if the mesh is empty or
// has no triangles; the caller should not field such a model.
InstancedLodModel BuildPropLodModel(const ImportedModel& imported,
                                    const PropMaterialTextures& textures,
                                    const PropLodOptions& options = {});

}  // namespace badlands
