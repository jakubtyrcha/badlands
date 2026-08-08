// Catch2 suite for ComposeMaterialCacheKey
// (engine/rendering/material/material_instance_cache.{hpp,cpp}).
//
// Pure CPU: the key is an integer mix, so this needs no device and no shader.
//
// WHY THIS EXISTS. The key is the ONLY thing separating one cached material
// instance from another, and MaterialInstanceCache::GetOrCreate returns the
// cached entry on a key hit without re-checking what it was built from. Two
// entries that collide therefore share one instance, and the second caller
// silently gets the first's pipeline.
//
// That is not hypothetical. The instanced-LOD field asks for a G-buffer and a
// shadow instance of the SAME material, differing only in the `pass` argument,
// once per (model, lod) bucket. The original boost-style combine
//
//     h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2)
//
// does not diffuse: libc++'s std::hash<int>/std::hash<uint64_t> are the
// identity, so `pass` and `texture_config_hash` entered as tiny dense integers
// whose low-bit deltas cancelled each other. Measured on the shipping forest,
// 256 distinct (namespace, pass, bucket) triples produced 224 distinct keys --
// 32 shadow lookups returned a G-buffer instance, and Dawn rejected each one
// ("attachment state of [RenderPipeline] is not compatible" -- the 3-target
// G-buffer pipeline set on the depth-only shadow pass), invalidating the whole
// command buffer.
//
// So these tests pin the property that failure needed: within the space of
// arguments the engine actually varies, distinct arguments give distinct keys.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "core/geometry_type.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"

using badlands::ComposeMaterialCacheKey;
using badlands::GeometryType;
using badlands::MaterialInstanceCache;
using badlands::MaterialInstanceFactory;
using badlands::MaterialPassType;
using badlands::MaterialParameterId;
using badlands::MaterialParameterValue;
using badlands::RenderingMaterialInstance;
using badlands::RenderPassType;

namespace {

constexpr GeometryType kGeometries[] = {
    GeometryType::kTexturedMesh, GeometryType::kSphericalMesh,
    GeometryType::kTerrainBlend, GeometryType::kTerrainCluster,
    GeometryType::kInstancedMesh};

constexpr RenderPassType kPasses[] = {
    RenderPassType::kGBuffer, RenderPassType::kShadow, RenderPassType::kForward};

// The two namespaces the shipping forest uses, built the way NamespaceHash
// builds them (instanced_lod_field.cpp): entt::hashed_string of the spec's
// cache_namespace, XORed with submesh * 0x9E3779B9. Derived rather than
// pinned, so renaming a namespace moves the test with it.
const uint32_t kTreeBarkNs =
    entt::hashed_string{"tree_bark"}.value() ^ (0u * 0x9E3779B9u);
const uint32_t kTreeCrownNs =
    entt::hashed_string{"tree_crown"}.value() ^ (1u * 0x9E3779B9u);

// A material instance that records nothing but what it was built as. No
// device, no pipeline — the cache only ever compares provenance, so a null
// pipeline is enough to exercise the collision guard.
class FakeInstance : public RenderingMaterialInstance {
 public:
  FakeInstance(GeometryType geo, RenderPassType pass)
      : geo_(geo), pass_(pass) {}

  bool Bind(badlands::RenderPassContext&, badlands::FrameContext&) override {
    return true;
  }
  bool BindPerObject(badlands::RenderPassContext&,
                     badlands::FrameContext&) override {
    return true;
  }
  MaterialParameterId GetParameterId(const std::string&) const override {
    return MaterialParameterId{};
  }
  void SetParameter(MaterialParameterId,
                    const MaterialParameterValue&) override {}
  void SetWireframe(bool) override {}
  GeometryType GetGeometryType() const override { return geo_; }
  RenderPassType GetRenderPassType() const override { return pass_; }
  wgpu::RenderPipeline GetPipeline() const override { return nullptr; }
  bool DeclaresBindGroup(uint32_t) const override { return false; }
  bool IsValid() const override { return true; }

 private:
  GeometryType geo_;
  RenderPassType pass_;
};

class FakeFactory : public MaterialInstanceFactory {
 public:
  std::unique_ptr<RenderingMaterialInstance> CreateInstance(
      GeometryType geo, MaterialPassType, RenderPassType pass,
      const badlands::InstanceParams&) override {
    ++created;
    return std::make_unique<FakeInstance>(geo, pass);
  }

  int created = 0;
};

}  // namespace

// The exact shape that broke: one namespace, one geometry, a run of buckets,
// asked for in both the G-buffer and the shadow pass. Every one of those is a
// separate cache entry and none may alias another.
TEST_CASE("pass and bucket together never alias a cache key",
          "[material][cache]") {
  std::set<entt::id_type> keys;
  size_t emitted = 0;

  for (uint32_t ns : {kTreeBarkNs, kTreeCrownNs}) {
    for (RenderPassType pass : kPasses) {
      // 512 buckets covers the shipping forest (16 models x 8 lod slots) with
      // room for the prop fields, at the resolution BucketId actually uses.
      for (uint64_t bucket = 0; bucket < 512; ++bucket) {
        keys.insert(ComposeMaterialCacheKey(ns, GeometryType::kInstancedMesh,
                                            pass, bucket));
        ++emitted;
      }
    }
  }

  CHECK(keys.size() == emitted);
}

// The same, widened over every geometry type — RenderTexturedMeshes keys on
// the mesh's geometry, so a terrain cluster and a textured mesh sharing a
// factory rely on that argument alone to stay apart.
TEST_CASE("geometry, pass and config together never alias a cache key",
          "[material][cache]") {
  std::set<entt::id_type> keys;
  size_t emitted = 0;

  for (uint32_t ns : {kTreeBarkNs, kTreeCrownNs}) {
    for (GeometryType geo : kGeometries) {
      for (RenderPassType pass : kPasses) {
        for (uint64_t config = 0; config < 256; ++config) {
          keys.insert(ComposeMaterialCacheKey(ns, geo, pass, config));
          ++emitted;
        }
      }
    }
  }

  CHECK(keys.size() == emitted);
}

// Each argument must be load-bearing on its own: holding the other three
// fixed, varying one has to move the key. A combine that lets one argument's
// delta be cancelled by another's is what produced the shadow/G-buffer alias.
TEST_CASE("every argument alone changes the key", "[material][cache]") {
  const uint32_t ns = kTreeCrownNs;
  const auto base = ComposeMaterialCacheKey(
      ns, GeometryType::kInstancedMesh, RenderPassType::kGBuffer, 64);

  CHECK(ComposeMaterialCacheKey(ns + 1, GeometryType::kInstancedMesh,
                                RenderPassType::kGBuffer, 64) != base);
  CHECK(ComposeMaterialCacheKey(ns, GeometryType::kTexturedMesh,
                                RenderPassType::kGBuffer, 64) != base);
  CHECK(ComposeMaterialCacheKey(ns, GeometryType::kInstancedMesh,
                                RenderPassType::kShadow, 64) != base);
  CHECK(ComposeMaterialCacheKey(ns, GeometryType::kInstancedMesh,
                                RenderPassType::kGBuffer, 65) != base);
}

// A hash that avalanches spreads a dense input across the whole 32-bit range.
// The old combine left the shipping key set packed into a handful of
// contiguous runs (0x81b86ec4, ..c5, ..c6, ..c7, ..cc, ...), which is what
// made a structural collision likely rather than a birthday coincidence.
TEST_CASE("keys for a dense bucket run are spread, not clustered",
          "[material][cache]") {
  std::vector<entt::id_type> keys;
  for (uint64_t bucket = 0; bucket < 256; ++bucket) {
    keys.push_back(ComposeMaterialCacheKey(kTreeCrownNs,
                                           GeometryType::kInstancedMesh,
                                           RenderPassType::kShadow, bucket));
  }

  // Bucket the keys by their top 4 bits; a diffusing hash hits most of the 16
  // bins, while the old combine put all 256 into one or two.
  std::set<entt::id_type> occupied_bins;
  for (entt::id_type k : keys) {
    occupied_bins.insert(k >> 28);
  }
  CHECK(occupied_bins.size() >= 12);
}

// The guard behind the hash. No 32-bit key can be collision-free, so a hit
// must be checked against what the entry was built from rather than trusted.
// Forcing one key to serve two different requests is exactly the shape the
// forest hit; the cache must refuse rather than hand back the wrong instance.
TEST_CASE("a colliding key is refused, not served", "[material][cache]") {
  MaterialInstanceCache cache;
  FakeFactory factory;
  const entt::id_type key = 0x1234abcd;  // deliberately reused below
  const badlands::InstanceParams params;

  auto gbuffer =
      cache.GetOrCreate(key, factory, GeometryType::kInstancedMesh,
                        MaterialPassType::kDeferred,
                        RenderPassType::kGBuffer, params);
  REQUIRE(gbuffer);
  CHECK(gbuffer->GetRenderPassType() == RenderPassType::kGBuffer);

  SECTION("a different render pass on the same key is refused") {
    auto shadow =
        cache.GetOrCreate(key, factory, GeometryType::kInstancedMesh,
                          MaterialPassType::kDeferred,
                          RenderPassType::kShadow, params);
    CHECK_FALSE(shadow);
  }

  SECTION("a different geometry on the same key is refused") {
    auto other = cache.GetOrCreate(key, factory, GeometryType::kTexturedMesh,
                                   MaterialPassType::kDeferred,
                                   RenderPassType::kGBuffer, params);
    CHECK_FALSE(other);
  }

  SECTION("a different factory on the same key is refused") {
    // The instance cannot report which factory built it, which is why the
    // cache records provenance instead of interrogating the instance.
    FakeFactory other_factory;
    auto other = cache.GetOrCreate(key, other_factory,
                                   GeometryType::kInstancedMesh,
                                   MaterialPassType::kDeferred,
                                   RenderPassType::kGBuffer, params);
    CHECK_FALSE(other);
    CHECK(other_factory.created == 0);
  }

  SECTION("the identical request still hits the cache") {
    auto again =
        cache.GetOrCreate(key, factory, GeometryType::kInstancedMesh,
                          MaterialPassType::kDeferred,
                          RenderPassType::kGBuffer, params);
    REQUIRE(again);
    CHECK(again.operator->() == gbuffer.operator->());
    CHECK(factory.created == 1);  // served from the cache, not rebuilt
  }
}
