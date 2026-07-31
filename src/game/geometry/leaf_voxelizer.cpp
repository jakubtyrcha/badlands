#include "game/geometry/leaf_voxelizer.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>    // glm::two_pi
#include <glm/gtc/quaternion.hpp>   // glm::angleAxis
#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/mesh_builder_utils.hpp"  // PushVertex
#include "game/geometry/leaf_texture.hpp"                     // BuildLeafRgba8

namespace badlands {

// Cell-key packing: 21 bits/axis (x in [0,20], y in [21,41], z in [42,62]),
// collision-free for any dims component <= 512 (SplatLeafCards' fail-loudly
// guard) -- 2^21 = 2097152 is far past that, leaving headroom.
namespace {
constexpr uint64_t kCellKeyMask = (uint64_t{1} << 21) - 1;
}  // namespace

uint64_t PackCellKey(glm::ivec3 cell) {
  return (static_cast<uint64_t>(cell.x) & kCellKeyMask) |
         ((static_cast<uint64_t>(cell.y) & kCellKeyMask) << 21) |
         ((static_cast<uint64_t>(cell.z) & kCellKeyMask) << 42);
}

glm::ivec3 UnpackCellKey(uint64_t key) {
  return glm::ivec3(static_cast<int>(key & kCellKeyMask),
                    static_cast<int>((key >> 21) & kCellKeyMask),
                    static_cast<int>((key >> 42) & kCellKeyMask));
}

namespace {

// pcg3d (Jarzynski/Olano)-style integer hash -- stateless, no RNG object, so
// output depends only on (cell, salt).
glm::uvec3 Pcg3d(glm::uvec3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return v;
}

// Deterministic per-cell pseudo-random value in [0,1). `salt` distinguishes
// independent draws at the same cell (EmitTetMesh: 0=roll, 1-3=axis jitter
// components, 4=brightness).
float hash01(glm::ivec3 cell, uint32_t salt) {
  const glm::uvec3 h = Pcg3d(glm::uvec3(static_cast<uint32_t>(cell.x),
                                        static_cast<uint32_t>(cell.y),
                                        static_cast<uint32_t>(cell.z) ^ (salt * 0x9E3779B9u)));
  return static_cast<float>(h.x) * (1.0f / 4294967296.0f);  // / 2^32
}

glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
  const float len = glm::length(v);
  return (len > 1e-8f) ? v / len : fallback;
}

}  // namespace

LeafVoxelGrid SplatLeafCards(const StaticTexturedMeshComponent& leaf_mesh,
                             LeafSilhouette silhouette, const LeafVoxelizeOptions& opts) {
  LeafVoxelGrid grid;
  grid.cell_size = opts.cell_size;
  const float h = opts.cell_size;

  // Empty mesh -> empty grid. Guard explicitly: ComputeLocalAabb on a
  // vertex-less mesh returns Aabb::Empty() (min=FLT_MAX, max=lowest), which
  // would otherwise feed huge/negative values into the floor/ceil dims math
  // below.
  if (leaf_mesh.indices.empty()) return grid;

  const Aabb aabb = ComputeLocalAabb(leaf_mesh);
  grid.origin = glm::vec3(std::floor(aabb.min.x / h), std::floor(aabb.min.y / h),
                          std::floor(aabb.min.z / h)) * h;
  const glm::vec3 span = aabb.max - grid.origin;
  grid.dims = glm::ivec3(static_cast<int>(std::ceil(span.x / h)),
                         static_cast<int>(std::ceil(span.y / h)),
                         static_cast<int>(std::ceil(span.z / h)));

  // >512 must fail loudly UNCONDITIONALLY -- checked before, and
  // independently of, the degenerate-dims guard below, so a mesh that is
  // simultaneously flat on one axis and oversized on another still logs the
  // error (the degenerate branch's early-out must never mask this one).
  if (grid.dims.x > 512 || grid.dims.y > 512 || grid.dims.z > 512) {
    spdlog::error(
        "SplatLeafCards: leaf mesh AABB needs a {}x{}x{} cell grid at cell_size={} "
        "(> 512 on some axis) -- returning an empty grid",
        grid.dims.x, grid.dims.y, grid.dims.z, h);
    grid.cells.clear();
    return grid;
  }

  // Degenerate (zero-volume, e.g. an AABB axis landing exactly on a
  // cell_size multiple) -- nothing to voxelize, not an error.
  if (grid.dims.x <= 0 || grid.dims.y <= 0 || grid.dims.z <= 0) {
    grid.dims = glm::ivec3(0);
    return grid;
  }

  const std::vector<uint8_t> coverage =
      BuildLeafRgba8(opts.coverage_texture_size, glm::vec3(1.0f), silhouette);
  const int tex_size = opts.coverage_texture_size;
  const float d = h / 3.0f;  // lattice step target (Splat bullet)

  const std::vector<float>& v = leaf_mesh.vertices;
  const std::vector<uint32_t>& idx = leaf_mesh.indices;
  auto pos_at = [&](uint32_t vert_index) {
    const size_t off = static_cast<size_t>(vert_index) * kTexturedMeshFloatsPerVertex;
    return glm::vec3(v[off + 0], v[off + 1], v[off + 2]);
  };

  const glm::ivec3 max_cell = grid.dims - glm::ivec3(1);

  for (size_t i = 0; i + 5 < idx.size(); i += 6) {
    const uint32_t base = idx[i];
    const glm::vec3 v0 = pos_at(base + 0);
    const glm::vec3 v1 = pos_at(base + 1);
    const glm::vec3 v2 = pos_at(base + 2);
    // v3 (base+3) isn't needed: W/H already span the full quad from v1.

    const glm::vec3 growth_axis = SafeNormalize(v0 - v1, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 w = v2 - v1;
    const glm::vec3 hgt = v0 - v1;
    const float w_len = glm::length(w);
    const float h_len = glm::length(hgt);

    const int nu = std::max(1, static_cast<int>(std::ceil(w_len / d)));
    const int nv = std::max(1, static_cast<int>(std::ceil(h_len / d)));
    const float dA = (w_len / static_cast<float>(nu)) * (h_len / static_cast<float>(nv));

    for (int sv = 0; sv < nv; ++sv) {
      const float vf = (static_cast<float>(sv) + 0.5f) / static_cast<float>(nv);
      for (int su = 0; su < nu; ++su) {
        const float uf = (static_cast<float>(su) + 0.5f) / static_cast<float>(nu);
        const glm::vec3 p = v1 + w * uf + hgt * vf;

        const int px = std::clamp(static_cast<int>(uf * static_cast<float>(tex_size)), 0,
                                  tex_size - 1);
        const int py = std::clamp(static_cast<int>(vf * static_cast<float>(tex_size)), 0,
                                  tex_size - 1);
        const size_t tex_idx =
            (static_cast<size_t>(py) * static_cast<size_t>(tex_size) + static_cast<size_t>(px)) *
            4;
        const float alpha = static_cast<float>(coverage[tex_idx + 3]) / 255.0f;
        if (alpha <= 0.0f) continue;  // no contribution
        const float gray = static_cast<float>(coverage[tex_idx + 0]) / 255.0f;  // RGB=gray at white

        glm::ivec3 cell = glm::ivec3(glm::floor((p - grid.origin) / h));
        cell = glm::clamp(cell, glm::ivec3(0), max_cell);

        LeafVoxelGrid::Accum& acc = grid.cells[PackCellKey(cell)];
        acc.area_alpha += alpha * dA;
        acc.area_gray += alpha * gray * dA;
        acc.area_axis += alpha * dA * growth_axis;
      }
    }
  }

  return grid;
}

TexturedMeshResult EmitTetMesh(const LeafVoxelGrid& grid, const LeafVoxelizeOptions& opts) {
  StaticTexturedMeshComponent mesh;
  mesh.geometry_type = GeometryType::kTexturedMesh;

  const float h = grid.cell_size;
  const float occ_threshold = opts.occupancy_fraction * h * h;
  const glm::vec3 aabb_center = grid.origin + glm::vec3(grid.dims) * h * 0.5f;

  // Canonical regular tet, apex +Y, circumradius 1 (scaled by
  // 0.5*overscale*h per instance below).
  const float sqrt2 = std::sqrt(2.0f);
  const float sqrt6 = std::sqrt(6.0f);
  const glm::vec3 canonical[4] = {
      glm::vec3(0.0f, 1.0f, 0.0f),
      glm::vec3(2.0f * sqrt2 / 3.0f, -1.0f / 3.0f, 0.0f),
      glm::vec3(-sqrt2 / 3.0f, -1.0f / 3.0f, sqrt6 / 3.0f),
      glm::vec3(-sqrt2 / 3.0f, -1.0f / 3.0f, -sqrt6 / 3.0f),
  };
  // Outward-CCW per tet face (back-culling); Phase 2's readback test
  // verifies this empirically once the material is wired up.
  constexpr uint32_t kTetTriIndices[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
  const float radius = 0.5f * opts.overscale * h;

  // unordered_map iteration order isn't stable run-to-run (or across STL
  // implementations) -- sort keys once so emit order, and therefore output
  // bytes, are deterministic.
  std::vector<uint64_t> keys;
  keys.reserve(grid.cells.size());
  for (const auto& [key, acc] : grid.cells) keys.push_back(key);
  std::sort(keys.begin(), keys.end());

  for (uint64_t key : keys) {
    const LeafVoxelGrid::Accum& acc = grid.cells.at(key);
    if (acc.area_alpha < occ_threshold) continue;

    const glm::ivec3 cell = UnpackCellKey(key);
    const glm::vec3 cell_center = grid.origin + (glm::vec3(cell) + 0.5f) * h;

    const float area_axis_len = glm::length(acc.area_axis);
    const glm::vec3 axis = (area_axis_len > 1e-8f)
        ? acc.area_axis / area_axis_len
        : SafeNormalize(cell_center - aabb_center, glm::vec3(0.0f, 1.0f, 0.0f));

    const glm::vec3 hash3(hash01(cell, 1), hash01(cell, 2), hash01(cell, 3));
    const glm::vec3 a_prime =
        SafeNormalize(axis + opts.axis_jitter * (2.0f * hash3 - 1.0f), axis);

    // Orthonormal basis with a_prime as the "up" axis (canonical tet's +Y),
    // built so [t | a_prime | b] has det=+1 (a proper rotation) and the
    // canonical tet's positive signed volume survives the transform.
    const glm::vec3 helper =
        (std::fabs(a_prime.y) < 0.999f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 t = SafeNormalize(glm::cross(helper, a_prime), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 b = glm::cross(t, a_prime);
    const glm::mat3 basis(t, a_prime, b);

    const float theta = glm::two_pi<float>() * hash01(cell, 0);
    const glm::quat roll = glm::angleAxis(theta, glm::vec3(0.0f, 1.0f, 0.0f));

    const float brightness = std::clamp(
        (acc.area_alpha > 1e-8f) ? (acc.area_gray / acc.area_alpha) : 0.0f, 0.0f, 1.0f);
    const float brightness_jittered =
        std::clamp(brightness * (1.0f - opts.brightness_jitter * hash01(cell, 4)), 0.0f, 1.0f);
    const glm::vec2 uv(brightness_jittered, 0.0f);

    const uint32_t base = mesh.vertex_count;
    for (const glm::vec3& p : canonical) {
      const glm::vec3 world = cell_center + radius * (basis * (roll * p));
      PushVertex(mesh.vertices, world, uv, a_prime, t);
    }
    mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() / kTexturedMeshFloatsPerVertex);
    for (uint32_t k : kTetTriIndices) mesh.indices.push_back(base + k);
  }

  mesh.dirty = true;
  const Aabb bounds = ComputeLocalAabb(mesh);
  return {.mesh = std::move(mesh), .local_bounds = bounds};
}

TexturedMeshResult VoxelizeLeafCards(const StaticTexturedMeshComponent& leaf_mesh,
                                     LeafSilhouette silhouette, const LeafVoxelizeOptions& opts) {
  return EmitTetMesh(SplatLeafCards(leaf_mesh, silhouette, opts), opts);
}

}  // namespace badlands
