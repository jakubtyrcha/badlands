// Ported from sampo's src/rendering/material/material_instance_cache.cpp,
// namespace sampo -> badlands (verbatim otherwise).
#include "engine/rendering/material/material_instance_cache.hpp"

#include <functional>

namespace badlands {

entt::id_type ComposeMaterialCacheKey(entt::id_type factory_id,
                                       GeometryType geo, RenderPassType pass,
                                       uint64_t texture_config_hash) {
  // Simple hash combine via FNV-1a-style mixing
  auto h = static_cast<size_t>(factory_id);
  h ^= std::hash<int>{}(static_cast<int>(geo)) + 0x9e3779b9 + (h << 6) +
       (h >> 2);
  h ^= std::hash<int>{}(static_cast<int>(pass)) + 0x9e3779b9 + (h << 6) +
       (h >> 2);
  h ^= std::hash<uint64_t>{}(texture_config_hash) + 0x9e3779b9 + (h << 6) +
       (h >> 2);
  return static_cast<entt::id_type>(h);
}

entt::resource<RenderingMaterialInstance> MaterialInstanceCache::GetOrCreate(
    entt::id_type key, MaterialInstanceFactory& factory, GeometryType geo,
    MaterialPassType material_pass, RenderPassType pass,
    const InstanceParams& params) {
  if (cache_.contains(key)) {
    return cache_[key];
  }

  auto [it, loaded] = cache_.load(key, factory, geo, material_pass, pass, params);
  if (!loaded) {
    // Factory returned null — don't cache it
    cache_.erase(key);
    return entt::resource<RenderingMaterialInstance>{};
  }

  // Check if the created instance is valid
  auto handle = cache_[key];
  if (!handle || !handle->IsValid()) {
    cache_.erase(key);
    return entt::resource<RenderingMaterialInstance>{};
  }

  return handle;
}

}  // namespace badlands
