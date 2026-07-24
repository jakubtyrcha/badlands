#pragma once

// Task S2.D (M3: swapped glTF URIs for per-pack material.json manifests).
// PBR material-pack loader + cache. Turns a PBR pack (a `material.json`
// manifest + albedo/normal/arm textures under `<dir>/`) into a cached
// deferred `normalmapped` material instance. Engine, game-agnostic: keyed
// purely by pack directory -- no game::MaterialId here (see
// src/game/material_pack.h for the game-side MaterialId -> pack mapping).
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

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

// Builds and caches deferred `normalmapped` materials from PBR packs.
//
// A "pack" is a directory (`dir`) containing a `material.json` manifest
// (albedo/normal/arm relative paths, e.g. under a `textures/` subdirectory)
// plus the textures it points at. The manifest is the source of truth for
// which file fills each slot -- not filename conventions.
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

  // Returns the cached deferred material for the pack at `dir` (e.g.
  // "assets/materials/rocky_terrain_02_1k"), loading + GPU-uploading its
  // three textures (with full mip chains) from `<dir>/material.json` on
  // first request.
  //
  // The ARM texture is bound whole (R=AO, G=roughness, B=metallic -- the
  // `normalmapped` shader samples `.g` for roughness and `.r` for baked AO;
  // metallic is currently unused). No repacking: the manifest's `arm` source
  // is loaded as-is.
  //
  // On a load failure (missing manifest, decode error), returns a
  // DeferredMaterial with the shared factory but texture_overrides pointing
  // at null views -- logged via spdlog::error, and marks ok() false. Callers
  // that build their scene through Get()/SolidColor MUST check ok() once
  // afterward and fail their own Initialize() if it's false (see ok()) --
  // otherwise a null-view material silently renders the factory's 1x1
  // white defaults instead of failing loudly.
  DeferredMaterial Get(const std::string& dir);

  // Returns a cached deferred `normalmapped` material with a flat 1x1 albedo
  // of `rgb` (linear 0..1, quantized to 8-bit) and a 1x1 ARM texture encoding
  // `roughness` (R=255 i.e. AO=1, G=roughness*255, B=0), both sampled
  // through the library's shared sampler. The normal slot falls back to the
  // factory's flat-normal default. Caches by the quantized (rgb, roughness)
  // tuple so repeated requests for the same color reuse one pair of
  // textures. DRYs the hand-rolled solid-color floor + capsule materials the
  // views used to build inline.
  DeferredMaterial SolidColor(glm::vec3 rgb, float roughness);

  // Returns a cached deferred `normalmapped` material whose albedo is a
  // procedurally-generated checkerboard (`tiles` tiles/side of `color_a` /
  // `color_b`, `texels` px/side), with a flat-normal default and a matte 1x1
  // ARM at `roughness`. The checkerboard (BuildCheckerboardRgba8) is uploaded
  // with a full mip chain via UploadTexture2DWithMips and sampled through the
  // shared trilinear+aniso sampler. Caches by (color_a, color_b, tiles, texels,
  // roughness). Colors are raw sRGB (same convention as SolidColor). A UV-debug
  // material — no new shader/factory, just a non-1x1 albedo.
  DeferredMaterial CheckerAlbedo(glm::vec3 color_a, glm::vec3 color_b,
                                 int tiles = 8, int texels = 512,
                                 float roughness = 1.0f);

  // Returns a forward-opaque, alpha-tested (cutout), double-sided, lit
  // material bound to the given `albedo` view + `sampler` (the caller owns and
  // keeps them alive). `cutoff` is the alpha discard threshold; `tint`
  // multiplies the sampled RGB (e.g. white leaf texture * green tint). The
  // material declares @group(2), so the forward-opaque pass binds the engine
  // shadow-map + IBL resources: it RECEIVES the sun's standard BRDF (the same
  // shared shadeStandard the deferred pass uses) + shadow-map PCF + IBL, and
  // still casts leaf-shaped shadows via its own alpha-tested shadow-pass
  // variant. General alpha-cutout material -- no foliage-specific logic. The
  // underlying forward-opaque factory (shader "standard_forward") is built once,
  // lazily, and shared by every call: only the per-instance albedo/tint/cutoff
  // vary. Meshes drawn
  // with it must be GeometryType::kTexturedMesh and added via
  // AddForwardOpaqueMeshEntity. Valid after Initialize().
  DeferredMaterial AlphaCutout(wgpu::TextureView albedo, wgpu::Sampler sampler,
                               float cutoff, glm::vec3 tint);

  // A terrain layer set: one texture_2d_array per PBR channel, layer i built
  // from the i'th pack passed to LoadTerrainArrays. Holds the textures (not
  // just views) so the caller keeps them alive by keeping this.
  struct TerrainArrays {
    LoadedTexture albedo;
    LoadedTexture normal;  // GL-convention (LoadPack flips DX packs at load)
    LoadedTexture arm;     // R=AO, G=roughness, B=metallic
  };

  // Loads N PBR packs and packs them into three texture_2d_arrays (albedo /
  // normal / arm), layer i = pack_dirs[i]. Engine-generic: "N packs -> 3
  // arrays", with no notion of what the layers mean (the game maps its own
  // concepts -- e.g. biomes -- onto layer indices).
  //
  // Reuses LoadPack (manifest-driven, full mip chains, DX->GL green flip) per
  // pack, then PackTexturesIntoArray per channel. Every pack must therefore
  // share one texture size (a texture array has one size for all layers) --
  // a mismatch fails the load.
  //
  // Unlike Get(), the per-pack 2D textures are NOT cached: they exist only to
  // be copied into the arrays, and keeping them would double terrain VRAM. A
  // pack used both here and via Get() is therefore decoded twice.
  //
  // On any failure returns a TerrainArrays with null members and marks ok()
  // false (same contract as Get()); callers MUST check ok().
  TerrainArrays LoadTerrainArrays(const std::vector<std::string>& pack_dirs);

  // Builds debug (blockout) terrain arrays from N solid albedo colors: layer i
  // = 1x1 (albedo = srgb_colors[i], flat normal, matte ARM). `srgb_colors` are
  // stored as raw sRGB bytes -- deferred_lighting re-linearizes G-buffer albedo,
  // so the surface renders as that sRGB color (same convention as SolidColor).
  // Layer index is the caller's concept (e.g. biome enum value). The returned
  // views keep their textures alive; the caller keeps this TerrainArrays to hold
  // them. The blockout counterpart of LoadTerrainArrays; pair with TerrainBlend.
  TerrainArrays DebugTerrainArrays(const std::vector<glm::vec3>& srgb_colors);

  // Returns a deferred terrain-blend material bound to the three
  // texture_2d_arrays of `arrays` (albedo/normal/arm layers), sampled through
  // the library's shared trilinear + 16x-aniso sampler (a bare sampler would
  // defeat the arrays' mip chains). Meshes drawn with it must use
  // GeometryType::kTerrainBlend (per-vertex blend_weights + layer_indices).
  // The caller owns/keeps `arrays` alive; all instances share one
  // terrain_blend kDeferred factory. Valid after Initialize().
  DeferredMaterial TerrainBlend(const TerrainArrays& arrays);

  // The shared normalmapped kDeferred factory (valid after Initialize()).
  MaterialInstanceFactory* factory() const { return factory_.get(); }

  // The shared terrain_blend kDeferred factory (valid after Initialize()).
  MaterialInstanceFactory* terrain_factory() const {
    return terrain_factory_.get();
  }

  // False if any pack has hard-failed to load since construction (missing
  // manifest, unparseable JSON, or a missing/undecodable texture) -- see
  // LoadPack. Callers MUST check this after building their scene (which
  // calls Get() for every pack the scene references) and fail their own
  // Initialize() if false: a load failure otherwise silently renders with
  // the material factory's 1x1 white-default textures instead of failing
  // loudly.
  bool ok() const { return !load_failed_; }

 private:
  struct PackTextures {
    LoadedTexture albedo;
    LoadedTexture normal;
    LoadedTexture arm;  // whole ARM: R=AO, G=roughness, B=metallic
  };

  // Loads the pack at `dir` from its material.json manifest. On ANY hard
  // failure (missing manifest, unparseable JSON, missing/undecodable
  // texture) sets load_failed_ = true (mutable: LoadPack is const, called
  // from Get()) and returns a PackTextures with null texture members --
  // logged via spdlog::error in every case.
  PackTextures LoadPack(const std::string& dir) const;

  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  std::unique_ptr<MaterialInstanceFactory> factory_;
  std::unique_ptr<MaterialInstanceFactory> terrain_factory_;
  // Forward-opaque "standard_forward" alpha-cutout factory. Built lazily on the
  // first AlphaCutout() call (its HDR color / reversed-Z depth targets match the
  // forward-opaque pass) and shared by every subsequent call.
  std::unique_ptr<MaterialInstanceFactory> alpha_cutout_factory_;
  wgpu::Sampler sampler_;

  std::unordered_map<std::string, PackTextures> cache_;  // key: dir
  // Get()'s built InstanceParams, cached per dir alongside `cache_`'s
  // textures -- avoids rebuilding the 3-entry texture_overrides vector on
  // every Get() call for an already-loaded pack.
  std::unordered_map<std::string, InstanceParams> params_cache_;
  // key: r<<24 | g<<16 | b<<8 | roughness (all 8-bit). The stored
  // InstanceParams own the 1x1 texture views (which keep their textures
  // alive) for SolidColor's whole lifetime.
  std::unordered_map<uint32_t, InstanceParams> solid_cache_;

  // key: (packed sRGB color_a, packed sRGB color_b, tiles, texels, roughness*255).
  // Stored InstanceParams own the albedo/ARM texture views (which keep their
  // textures alive) for the material's lifetime -- same ownership model as
  // solid_cache_.
  std::map<std::tuple<uint32_t, uint32_t, int, int, uint8_t>, InstanceParams>
      checker_cache_;

  mutable bool load_failed_ = false;
};

}  // namespace badlands
