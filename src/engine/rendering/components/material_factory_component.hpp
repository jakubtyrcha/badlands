#pragma once

// Ported from sampo's
// src/rendering/components/material_factory_component.hpp, namespace sampo
// -> badlands, verbatim otherwise (includes adapted to badlands paths).

#include <cstring>
#include <entt/entt.hpp>
#include <functional>
#include <string>

#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/material_requirements.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"

namespace badlands {

// Component for entities with factory-created material instances (bypasses cache).
// Each entity owns its own RenderingMaterialInstance with pre-set parameters.
struct FactoryMaterialInstanceComponent {
  std::unique_ptr<RenderingMaterialInstance> instance;
};

// Replaces MaterialDataComponent. Entities carry a factory reference +
// per-instance overrides. The factory creates RenderingMaterialInstance at
// render time (via MaterialInstanceCache).
struct MaterialFactoryComponent {
  MaterialInstanceFactory* factory{nullptr};
  MaterialPassType pass_type{MaterialPassType::kDeferred};
  InstanceParams params;

  // Cache key component — changes when params change (invalidates cache)
  uint64_t config_hash{0};
};

inline uint64_t ComputeFactoryConfigHash(
    const MaterialFactoryComponent& fmc) {
  uint64_t hash = std::hash<void*>{}(fmc.factory);
  hash ^= std::hash<int>{}(static_cast<int>(fmc.pass_type)) + 0x9e3779b9 +
           (hash << 6) + (hash >> 2);
  for (const auto& tex : fmc.params.texture_overrides) {
    hash ^= std::hash<std::string>{}(tex.param_name) + 0x9e3779b9 +
             (hash << 6) + (hash >> 2);
    // Hash texture view and sampler handles to distinguish different textures.
    // Extract raw pointer via memcpy to avoid depending on C API type names.
    uintptr_t view_handle = 0;
    static_assert(sizeof(tex.view) == sizeof(uintptr_t));
    std::memcpy(&view_handle, &tex.view, sizeof(uintptr_t));
    hash ^= std::hash<uintptr_t>{}(view_handle) + 0x9e3779b9 +
             (hash << 6) + (hash >> 2);
    uintptr_t sampler_handle = 0;
    static_assert(sizeof(tex.sampler) == sizeof(uintptr_t));
    std::memcpy(&sampler_handle, &tex.sampler, sizeof(uintptr_t));
    hash ^= std::hash<uintptr_t>{}(sampler_handle) + 0x9e3779b9 +
             (hash << 6) + (hash >> 2);
  }
  for (const auto& [name, value] : fmc.params.uniform_overrides) {
    hash ^= std::hash<std::string>{}(name) + 0x9e3779b9 + (hash << 6) +
             (hash >> 2);
  }
  return hash;
}

}  // namespace badlands
