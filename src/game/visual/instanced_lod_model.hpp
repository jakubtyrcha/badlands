#pragma once

// The neutral description of a GPU-instanced model with a distance LOD chain:
// N levels, each holding M submeshes, plus the material each submesh draws
// with and the cutoffs between levels.
//
// NO tree, bark, leaf, foliage or prop vocabulary. That is the whole point --
// this type is what let the field builder and the impostor baker stop being
// tree-shaped. Producers sit above it and speak their own domain:
//
//   BuildTreeFieldModel  (tree_lod_model.hpp)  -> 2 submeshes: bark + voxel crown
//   BuildPropLodModel    (prop_lod_model.hpp)  -> 1 submesh:  the imported mesh
//
// and both hand the result to the same BuildInstancedLodField / BakeImpostorAtlas.
//
// PURE DATA, and deliberately DEVICE-FREE. BuildForestModels runs its producers
// under ParallelFor across ~28 models with no GPU in sight, so nothing here may
// require a wgpu::Device to construct. That is why a material that wants a flat
// support texture asks for one via SolidColorTextureSpec (a request the FIELD
// BUILDER materializes and owns) rather than carrying a wgpu::TextureView it
// would have needed a device to make.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
// Brings in DefaultTextureView/InstanceParams and, transitively,
// MaterialParameterValue (rendering_material_instance.hpp).
#include "engine/rendering/material/material_instance_factory.hpp"

namespace badlands {

// A 1x1 solid-colour texture bound to `slot_name`, created and owned by the
// field builder.
//
// Exists so a producer can ask for a flat support texture without a device --
// see this file's header note. The builder DEDUPES by rgba, so 28 models asking
// for the same matte ARM cost one texture, not 28.
struct SolidColorTextureSpec {
  std::string slot_name;
  std::array<uint8_t, 4> rgba{255, 255, 255, 255};
};

// One submesh's material, as data. The builder turns this into a
// FactoryDescriptor + InstanceParams and resolves it through a shared cache.
//
// `uniforms` must NOT contain "bucketId": the builder injects it per
// (model, lod), since only it knows the bucket. Supplying one would be
// overwritten, so the builder rejects it rather than silently winning.
struct InstancedMaterialSpec {
  std::string shader_name;  // e.g. "instanced_gbuffer"
  std::string shader_path;  // e.g. "material/instanced_gbuffer"
  // Namespaces this submesh's material-cache keys, so two submeshes sharing a
  // shader but differing in parameters do not collide. Falls back to
  // shader_name when empty.
  std::string cache_namespace;
  wgpu::CullMode cull_mode = wgpu::CullMode::Back;
  bool casts_shadow = true;
  std::map<std::string, MaterialParameterValue> uniforms;
  // Already-resolved views (a prop's PBR pack, an atlas). Bound by slot name.
  std::vector<DefaultTextureView> textures;
  // Requests for flat support textures the builder creates; see above.
  std::vector<SolidColorTextureSpec> solid_textures;
};

// One drawable in the impostor bake, naming its geometry by position in the
// model's own `levels` rather than by pointer -- there is then no way for the
// spec to outlive or disagree with the meshes it describes.
struct ImpostorBakeSubmesh {
  uint32_t lod = 0;
  uint32_t submesh = 0;
  glm::vec3 tint{1.0f};
  // Sampled for albedo when `voxel_brightness` is 0. Null binds the bake's own
  // opaque white 1x1, which makes tint alone the albedo.
  wgpu::TextureView albedo;
  // 0 = albedo comes from `albedo` * tint (a textured prop, or solid bark).
  // 1 = albedo comes from the vertex-baked uv.x brightness * tint, exactly as
  //     voxel_foliage.wesl computes it for a tet crown.
  float voxel_brightness = 0.0f;
};

// How to bake this model's impostor level. Empty `submeshes` means the model
// has no impostor and the chain ends at its last mesh level.
struct ImpostorBakeSpec {
  std::vector<ImpostorBakeSubmesh> submeshes;
  float transmission_strength = 0.0f;
  // The impostor's surface roughness. A constant rather than a sampled value
  // because the atlas has no roughness channel -- its two maps are already
  // full (albedo+coverage, octahedral normal+depth+thickness) -- so a textured
  // model's ARM cannot follow it to this level. One value per model is the
  // honest approximation at impostor range.
  float roughness = 0.9f;
  // Where this impostor takes over, in WORLD METRES, at native scale.
  //
  // Supplied by the producer rather than derived by the field builder, and that
  // is a layering fix rather than a convenience: the builder used to compute it
  // from kFoliageImpostorThresholdPreviewM, so the "neutral" layer included
  // foliage_voxel_config.hpp and every prop's cutoff silently depended on a
  // constant documented as independently screenshot-tunable. Retuning the
  // foliage number would have pushed props' cutoffs under their own last mesh
  // threshold, and GpuInstanceRenderer only LOGS a non-ascending chain -- the
  // field still builds and those levels simply never draw.
  //
  // Must exceed the model's last entry in `thresholds`.
  float threshold_m = 0.0f;
  // Skips the thickness pass entirely. A model with no transmitted term gets
  // nothing from it but pays a full render plus an R16Float readback per view
  // for a channel that would stay zero -- and `transmission_strength` 0 means
  // the runtime multiplies whatever it found by zero anyway.
  bool opaque = false;

  bool active() const { return !submeshes.empty(); }
};

// Per-submesh local-space bounds, each unioned over every LOD.
//
// Per submesh rather than one box, because two different questions get asked
// and they need different answers. `submesh_bounds[i]` is what a caller rests
// on the ground: a tree stands on its BARK, and resting it on the union would
// sink it by the crown's overhang (voxel tets overscale past the cards they
// came from). Combined() is the other question -- an instance's GPU bounds
// sphere and the cell-cull padding, where the union is exactly right.
//
// Unioned over LODs because no single level is conservative for the others.
struct LodModelBounds {
  std::vector<Aabb> submesh_bounds;

  Aabb Combined() const {
    Aabb out = Aabb::Empty();
    for (const Aabb& b : submesh_bounds) out = out.Union(b);
    return out;
  }
};

// One model: its LOD chain, the materials its submeshes draw with, and the
// cutoffs between levels.
//
// `levels` is indexed [lod][submesh]. A level may hold FEWER submeshes than
// `submesh_materials` describes, and any entry may legitimately be EMPTY --
// a voxel crown can clear no cell at a coarse cell size (see
// leaf_voxelizer.hpp's known gap), and that level then renders without it
// rather than failing the build.
struct InstancedLodModel {
  std::vector<std::vector<TexturedMeshResult>> levels;
  std::vector<InstancedMaterialSpec> submesh_materials;
  // Ascending distance cutoffs in WORLD METRES, exactly levels.size() - 1 of
  // them. The impostor's own cutoff is NOT here: a model has no reason to know
  // whether one was baked, so the field builder appends it.
  std::vector<float> thresholds;
  // Native model units -> world metres. Callers building instance transforms
  // need it, so it travels with the model rather than being re-derived.
  float native_to_world_scale = 1.0f;
  // What this model stands at in metres, used to scale the impostor cutoff.
  float target_height_m = 1.0f;
  ImpostorBakeSpec impostor;

  size_t lod_count() const { return levels.size(); }
  size_t submesh_count() const { return submesh_materials.size(); }
};

// Per-submesh bounds for one model, unioned across its levels. Empty meshes
// contribute nothing (Aabb::Union is a no-op against the Empty() sentinel).
LodModelBounds ComputeLodModelBounds(const InstancedLodModel& model);

// Why `model` cannot be built, or an empty string if it can.
//
// Returns a REASON rather than a bool so the builder can name the offending
// model and the specific rule in one log line; a bare false would leave the
// caller to guess between "no levels", "wrong cutoff count" and "a submesh
// with no material".
std::string ValidateLodModel(const InstancedLodModel& model);

}  // namespace badlands
