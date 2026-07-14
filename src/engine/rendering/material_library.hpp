#pragma once

// Task S2.D: PBR material-pack loader + cache. Turns a glTF PBR pack
// (albedo/normal/roughness textures under `<dir>/textures/`) into a cached
// deferred `normalmapped` material instance. Engine, game-agnostic: keyed
// purely by (pack directory, base filename) -- no game::MaterialId here (see
// src/game/material_pack.h for the game-side MaterialId -> pack mapping).
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_loader.hpp"

namespace badlands {

// A ready-to-attach deferred material: the shared `normalmapped` factory +
// per-pack InstanceParams (albedo/normal/roughness texture overrides).
// `factory` is owned by the MaterialLibrary that produced this -- it must
// outlive any use of the returned DeferredMaterial.
struct DeferredMaterial {
  MaterialInstanceFactory* factory = nullptr;
  InstanceParams params;
};

// Builds and caches deferred `normalmapped` materials from PBR glTF packs.
//
// A "pack" is a directory (`dir`) containing `<base>_1k.gltf` + a
// `textures/` subdirectory with albedo/normal/metallic-roughness JPEGs. The
// glTF document's first material is the source of truth for which texture
// file fills each slot (resolved via the `assets` crate's
// `badlands_gltf_pack_textures`, by texture URI) -- not filename
// conventions, since packs are inconsistent about the roughness source
// file's name (`_arm_1k.jpg` vs `_rough_1k.jpg` vs other suffixes).
class MaterialLibrary {
 public:
  // Builds the shared normalmapped kDeferred factory (color formats =
  // GBuffer's normals/albedo/material targets, depth = GBuffer's depth) and
  // a shared trilinear + 16x-anisotropic repeat sampler used by every loaded
  // pack. Must be called once before Get(). Returns false (after logging) if
  // the factory build failed -- callers MUST propagate the failure, since
  // every DeferredMaterial produced afterwards would carry a null factory and
  // render nothing.
  bool Initialize(wgpu::Device device, wgpu::Queue queue,
                  GpuPipelineGenerator* pipeline_gen);

  // Returns the cached deferred material for the pack at `dir`/`base`,
  // loading + GPU-uploading its three textures (with full mip chains) on
  // first request. `dir` is the pack directory (e.g.
  // "assets/materials/rocky_trail_1k.gltf"); `base` is the glTF/texture
  // filename stem (e.g. "rocky_trail" -> "<dir>/rocky_trail_1k.gltf").
  //
  // The roughness texture is always produced with roughness in the R
  // channel (the `normalmapped` shader samples `.r`): the glTF's
  // metallic-roughness source is decoded and its G channel (glTF 2.0's
  // roughness channel) is copied into R/G/B of a new texture. This is a
  // no-op for already-grayscale `_rough` sources (R==G==B) and repacks
  // `_arm` sources (R=AO, G=roughness, B=metallic) correctly.
  //
  // On a load failure (missing pack, decode error), returns a
  // DeferredMaterial with the shared factory but texture_overrides pointing
  // at null views -- logged via spdlog::error.
  DeferredMaterial Get(const std::string& dir, const std::string& base);

  // Returns a cached deferred `normalmapped` material with a flat 1x1 albedo
  // of `rgb` (linear 0..1, quantized to 8-bit) and a 1x1 grayscale roughness
  // of `roughness`, both sampled through the library's shared sampler. The
  // normal slot falls back to the factory's flat-normal default. Caches by
  // the quantized (rgb, roughness) tuple so repeated requests for the same
  // color reuse one pair of textures. DRYs the hand-rolled solid-color floor
  // + capsule materials the views used to build inline.
  DeferredMaterial SolidColor(glm::vec3 rgb, float roughness);

  // The shared normalmapped kDeferred factory (valid after Initialize()).
  MaterialInstanceFactory* factory() const { return factory_.get(); }

 private:
  struct PackTextures {
    LoadedTexture albedo;
    LoadedTexture normal;
    LoadedTexture roughness;  // repacked so roughness is in R
  };

  PackTextures LoadPack(const std::string& dir, const std::string& base) const;

  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  std::unique_ptr<MaterialInstanceFactory> factory_;
  wgpu::Sampler sampler_;

  std::unordered_map<std::string, PackTextures> cache_;  // key: "dir/base"
  // key: r<<24 | g<<16 | b<<8 | roughness (all 8-bit). The stored
  // InstanceParams own the 1x1 texture views (which keep their textures
  // alive) for SolidColor's whole lifetime.
  std::unordered_map<uint32_t, InstanceParams> solid_cache_;
};

}  // namespace badlands
