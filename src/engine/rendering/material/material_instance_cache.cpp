// Ported from sampo's src/rendering/material/material_instance_cache.cpp,
// namespace sampo -> badlands. Deviation: ComposeMaterialCacheKey's mixing —
// see the note on it below.
#include "engine/rendering/material/material_instance_cache.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>

namespace badlands {

namespace {

// splitmix64's finalizer. Avalanches: a one-bit input change moves every
// output bit with probability ~1/2, which is the property the combine below
// needs and the reason it is not a plain multiply-shift.
uint64_t MixBits(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

}  // namespace

// DEVIATION FROM SAMPO, and the reason this is not the boost-style combine it
// was ported as:
//
//     h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2)
//
// That form assumes `v` is already a well-distributed hash. Three of the four
// arguments here are not — `geo` and `pass` are small enum ordinals and
// `texture_config_hash` is, at every instanced call site, a dense bucket index
// — and libc++'s std::hash for integers is the identity, so nothing diffused
// them before they were combined. Each term then perturbed `h` by an amount
// small enough for the NEXT argument's delta to cancel exactly: `pass` and
// `texture_config_hash` traded off one-for-one.
//
// That is not a birthday coincidence, it is structural. Measured on the
// shipping forest, 256 distinct (namespace, pass, bucket) triples collapsed to
// 224 keys, and across the full argument space 7680 triples gave 4162 keys —
// a 46% collision rate, with a dense bucket run landing entirely inside one
// sixteenth of the output range. Since GetOrCreate returns the cached entry on
// a key hit, each collision handed a shadow-pass lookup a G-buffer instance,
// and Dawn rejected the 3-target pipeline set on the depth-only shadow pass.
//
// Mixing every argument through a finalizer first removes the cancellation:
// the same spaces now give zero collisions, and a 200k-key stress lands at the
// birthday rate for a 32-bit key, which is the floor for this return type.
// See src/engine/tests/material_cache_key_tests.cpp.
entt::id_type ComposeMaterialCacheKey(entt::id_type factory_id,
                                       GeometryType geo, RenderPassType pass,
                                       uint64_t texture_config_hash) {
  uint64_t h = MixBits(static_cast<uint64_t>(factory_id));
  h = MixBits(h ^ static_cast<uint64_t>(geo));
  h = MixBits(h ^ static_cast<uint64_t>(pass));
  h = MixBits(h ^ texture_config_hash);
  // The high half: splitmix64's finalizer leaves its strongest bits there.
  return static_cast<entt::id_type>(h >> 32);
}

entt::resource<RenderingMaterialInstance> MaterialInstanceCache::GetOrCreate(
    entt::id_type key, MaterialInstanceFactory& factory, GeometryType geo,
    MaterialPassType material_pass, RenderPassType pass,
    const InstanceParams& params) {
  const Provenance asked{.factory = &factory,
                         .geo = geo,
                         .material_pass = material_pass,
                         .pass = pass};

  if (cache_.contains(key)) {
    // A 32-bit key cannot be collision-free — even a perfectly diffusing hash
    // hits the birthday bound eventually, and ComposeMaterialCacheKey's return
    // type fixes the width. What a collision must NOT do is what it used to:
    // silently hand back an instance built for a different material and let
    // the wrong pipeline reach a render pass, where it surfaces only as a Dawn
    // attachment-state rejection that invalidates the whole command buffer.
    //
    // So compare against what the entry was actually built FROM, not against
    // what the instance reports: the instance can only be asked its geometry
    // and render pass, while the key also mixes the factory and the caller's
    // texture-config hash. Two entries from different factories collide just
    // as silently as two passes did.
    //
    // NOT verified: `texture_config_hash`. The caller folds it into the key
    // but does not pass it here (see the header's Provenance note), so two
    // entries differing only in texture config remain indistinguishable at
    // this seam. Everything the signature carries is checked.
    //
    // On a mismatch, skip the draw — a missing shadow caster is recoverable, a
    // rejected command buffer drops the frame — and say so once per key, since
    // the textured-mesh path resolves through here every frame.
    auto prov = provenance_.find(key);
    if (prov != provenance_.end() &&
        (prov->second.factory != asked.factory ||
         prov->second.geo != asked.geo ||
         prov->second.material_pass != asked.material_pass ||
         prov->second.pass != asked.pass)) {
      if (!prov->second.reported) {
        prov->second.reported = true;
        spdlog::error(
            "MaterialInstanceCache: key {:#x} collides — the cached entry was "
            "built from (factory {}, geometry {}, material pass {}, render "
            "pass {}) but the caller asked for (factory {}, geometry {}, "
            "material pass {}, render pass {}); skipping the draw rather than "
            "binding the wrong pipeline",
            key, static_cast<const void*>(prov->second.factory),
            static_cast<int>(prov->second.geo),
            static_cast<int>(prov->second.material_pass),
            static_cast<int>(prov->second.pass),
            static_cast<const void*>(asked.factory), static_cast<int>(asked.geo),
            static_cast<int>(asked.material_pass),
            static_cast<int>(asked.pass));
      }
      return entt::resource<RenderingMaterialInstance>{};
    }
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

  provenance_[key] = asked;
  return handle;
}

}  // namespace badlands
