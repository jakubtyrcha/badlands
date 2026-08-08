#pragma once

// Ported from sampo's src/rendering/material/material_instance_cache.hpp,
// namespace sampo -> badlands (verbatim otherwise). entt is vendored at
// third_party/entt/single_include (entt 3.16.0; the resource_cache API this
// uses — contains/load/erase/size/operator[] — is unchanged from sampo's
// pinned 3.13.2).
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"

namespace badlands {

// Loader for entt::resource_cache — creates instances via factory
struct MaterialInstanceLoader {
  using result_type = std::shared_ptr<RenderingMaterialInstance>;

  result_type operator()(MaterialInstanceFactory& factory, GeometryType geo,
                         MaterialPassType material_pass, RenderPassType pass,
                         const InstanceParams& params) const {
    return factory.CreateInstance(geo, material_pass, pass, params);
  }
};

// Composite key → entt::id_type hash. Combines factory name, geometry type,
// pass type, and texture config hash into a single 32-bit key.
entt::id_type ComposeMaterialCacheKey(entt::id_type factory_id,
                                       GeometryType geo, RenderPassType pass,
                                       uint64_t texture_config_hash);

// Cache for resolved material instances. Backed by entt::resource_cache.
// Instances are shared across entities with the same key.
class MaterialInstanceCache {
 public:
  // Returns cached or creates new. Returns resource handle (bool-testable).
  // Null handle if the factory fails to create the instance, OR if `key`
  // collides with an entry built from different arguments (see the .cpp) --
  // so every caller must test the handle, not assume a hit.
  entt::resource<RenderingMaterialInstance> GetOrCreate(
      entt::id_type key, MaterialInstanceFactory& factory, GeometryType geo,
      MaterialPassType material_pass, RenderPassType pass,
      const InstanceParams& params = {});

  // Whether `key` is occupied -- NOT whether GetOrCreate will serve it. This
  // takes no factory/geometry/pass, so it cannot tell an entry built from the
  // caller's arguments from one that merely collides with them, and
  // GetOrCreate refuses the latter. Gate on GetOrCreate's handle instead.
  bool Contains(entt::id_type key) const { return cache_.contains(key); }
  void Erase(entt::id_type key) {
    cache_.erase(key);
    provenance_.erase(key);
  }
  void Clear() {
    cache_.clear();
    provenance_.clear();
  }
  [[nodiscard]] size_t Size() const { return cache_.size(); }

 private:
  // What a cached entry was actually built from, so a key hit can be checked
  // against what the caller is asking for rather than trusted. Holds only the
  // arguments GetOrCreate receives; the caller's `texture_config_hash` goes
  // into the key but is not passed here, so it is the one key input this
  // cannot verify (see GetOrCreate).
  struct Provenance {
    const MaterialInstanceFactory* factory = nullptr;
    GeometryType geo{};
    MaterialPassType material_pass{};
    RenderPassType pass{};
    bool reported = false;  // diagnostic emitted once, not once per frame
  };

  entt::resource_cache<RenderingMaterialInstance, MaterialInstanceLoader>
      cache_;
  std::unordered_map<entt::id_type, Provenance> provenance_;
};

}  // namespace badlands
