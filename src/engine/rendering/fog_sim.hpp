#pragma once

// Fog-generator CPU core (Task: map fog generator). Pure CPU, glm-only (no Dawn),
// header-only so it is unit-testable without a GPU. Defines the world-static fog
// emitter (disc/OBB footprint + volumetric params) and a uniform-grid broadphase
// that buckets emitters by cell so the composer (compute/fog_fill.wesl via
// fog_emitters.wesl) evaluates only the handful overlapping each froxel. The GPU
// side mirrors the emitter struct and footprint math.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace badlands::fog {

// --- Emitter ----------------------------------------------------------------

// Footprint / radial-envelope shape (mirrored in fog_emitters.wesl::fogRadialNd):
//   Disc    — isotropic circle, radius = half_extent.x.
//   Obb     — oriented rectangle (max-norm) over half_extent, rotated by `rotation`.
//   Ellipse — the OBB frame evaluated through an L2 radial → an oriented ellipse
//             inscribed in half_extent (biome-fit emitters use this).
enum class EmitterShape : uint32_t { Disc = 0, Obb = 1, Ellipse = 2 };
enum class EmitterType : uint32_t { Disc = 0, Noise = 1 };  // evaluator (fill)

// A world-static volumetric fog source. It owns its full 3D volume: the composer
// evaluates it per froxel in emitter-local space and accumulates σ_t.
//   footprint  — disc (half_extent.x = radius) or oriented box (half_extent =
//                half sizes pre-rotation, `rotation` = yaw about Y); bounds the
//                broadphase AABB and the horizontal envelope.
//   envelope   — radial × vertical distribution (falloff curves) over the
//                footprint and [base_y, base_y+height], scaled by `magnitude`
//                (peak σ_t, physical m^-1 — written to the media directly).
//   type       — Disc = envelope only; Noise = envelope × a 3D-noise fill
//                (`noise3(local·freq + scroll·time + seed)`, `scroll` animates it).
struct Emitter {
  glm::vec2 center{0.0f, 0.0f};       // world XZ
  glm::vec2 half_extent{1.0f, 1.0f};  // footprint half-size (disc: radius in .x)
  float rotation{0.0f};               // yaw about Y (local frame)
  float base_y{0.0f};                 // world Y of the volume base
  float height{1.0f};                 // vertical extent above base_y
  EmitterShape shape{EmitterShape::Disc};
  EmitterType type{EmitterType::Disc};
  float magnitude{0.0f};              // peak σ_t (physical extinction, m^-1)
  float radial_falloff{0.0f};         // radial edge softness [0,1]
  float vertical_falloff{0.0f};       // vertical edge softness [0,1]
  float noise_freq{0.0f};             // noise fill (type == Noise)
  float noise_contrast{1.0f};
  glm::vec3 scroll{0.0f};             // noise animation velocity (units/s)
  float seed{0.0f};
};

// Half-size of the world-space AABB of an emitter footprint (conservative;
// accounts for OBB rotation via the projected-extent formula).
inline glm::vec2 EmitterHalfAabb(const Emitter& e) {
  if (e.shape == EmitterShape::Disc) {
    // A disc's radius is half_extent.x (matches the GPU evaluator's fogRadialNd,
    // which uses .x only); .y is unused, so the AABB is the bounding square.
    const float r = std::abs(e.half_extent.x);
    return glm::vec2(r, r);
  }
  // Obb and Ellipse share the oriented half_extent box, so both use the
  // projected-extent bound (an ellipse is inscribed in the OBB, so its AABB is
  // conservative here — never smaller than the true footprint).
  const float c = std::abs(std::cos(e.rotation));
  const float s = std::abs(std::sin(e.rotation));
  return glm::vec2(e.half_extent.x * c + e.half_extent.y * s,
                   e.half_extent.x * s + e.half_extent.y * c);
}
inline glm::vec2 EmitterAabbMin(const Emitter& e) {
  return e.center - EmitterHalfAabb(e);
}
inline glm::vec2 EmitterAabbMax(const Emitter& e) {
  return e.center + EmitterHalfAabb(e);
}

// --- Broadphase: uniform grid, cell -> emitter-index bucket -----------------

struct Broadphase {
  glm::vec2 map_min{0.0f, 0.0f};
  float cell_size{1.0f};
  int nx{0};
  int nz{0};
  // Per cell (row-major, nx*nz): (offset, count) into `indices`.
  std::vector<glm::ivec2> cells;
  // Emitter indices grouped by cell.
  std::vector<uint32_t> indices;

  // Emitter indices whose bucket covers `world_xz` (empty if outside the grid).
  std::vector<uint32_t> CellEmitters(glm::vec2 world_xz) const {
    const int ix =
        static_cast<int>(std::floor((world_xz.x - map_min.x) / cell_size));
    const int iz =
        static_cast<int>(std::floor((world_xz.y - map_min.y) / cell_size));
    if (ix < 0 || iz < 0 || ix >= nx || iz >= nz) return {};
    const glm::ivec2 oc = cells[static_cast<size_t>(iz) * nx + ix];
    return std::vector<uint32_t>(indices.begin() + oc.x,
                                 indices.begin() + oc.x + oc.y);
  }
};

// Build a broadphase over `emitters` on a uniform grid of `bp_cell_size` cells
// spanning [map_min, map_max]. Each emitter is added to every cell its footprint
// AABB overlaps (clamped to the grid). Two-pass counting build.
inline Broadphase BuildBroadphase(std::span<const Emitter> emitters,
                                  glm::vec2 map_min, glm::vec2 map_max,
                                  float bp_cell_size) {
  Broadphase bp;
  bp.map_min = map_min;
  bp.cell_size = bp_cell_size;
  bp.nx = std::max(1, static_cast<int>(
                          std::ceil((map_max.x - map_min.x) / bp_cell_size)));
  bp.nz = std::max(1, static_cast<int>(
                          std::ceil((map_max.y - map_min.y) / bp_cell_size)));
  const int ncells = bp.nx * bp.nz;

  // Cell range [lo, hi] an emitter's AABB overlaps; false if fully off-grid.
  auto cell_range = [&](const Emitter& e, glm::ivec2& lo,
                        glm::ivec2& hi) -> bool {
    const glm::vec2 amin = EmitterAabbMin(e);
    const glm::vec2 amax = EmitterAabbMax(e);
    int x0 = static_cast<int>(std::floor((amin.x - map_min.x) / bp_cell_size));
    int x1 = static_cast<int>(std::floor((amax.x - map_min.x) / bp_cell_size));
    int z0 = static_cast<int>(std::floor((amin.y - map_min.y) / bp_cell_size));
    int z1 = static_cast<int>(std::floor((amax.y - map_min.y) / bp_cell_size));
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(bp.nx - 1, x1);
    z1 = std::min(bp.nz - 1, z1);
    if (x0 > x1 || z0 > z1) return false;
    lo = {x0, z0};
    hi = {x1, z1};
    return true;
  };

  // Pass 1: count per cell.
  std::vector<int> count(static_cast<size_t>(ncells), 0);
  for (const Emitter& e : emitters) {
    glm::ivec2 lo, hi;
    if (!cell_range(e, lo, hi)) continue;
    for (int z = lo.y; z <= hi.y; ++z)
      for (int x = lo.x; x <= hi.x; ++x) ++count[static_cast<size_t>(z) * bp.nx + x];
  }
  // Prefix-sum into (offset, count).
  bp.cells.resize(static_cast<size_t>(ncells));
  int total = 0;
  for (int c = 0; c < ncells; ++c) {
    bp.cells[static_cast<size_t>(c)] = glm::ivec2(total, count[static_cast<size_t>(c)]);
    total += count[static_cast<size_t>(c)];
  }
  bp.indices.resize(static_cast<size_t>(total));
  // Pass 2: scatter emitter indices using a per-cell cursor.
  std::vector<int> cursor(static_cast<size_t>(ncells), 0);
  for (size_t i = 0; i < emitters.size(); ++i) {
    glm::ivec2 lo, hi;
    if (!cell_range(emitters[i], lo, hi)) continue;
    for (int z = lo.y; z <= hi.y; ++z)
      for (int x = lo.x; x <= hi.x; ++x) {
        const size_t c = static_cast<size_t>(z) * bp.nx + x;
        bp.indices[static_cast<size_t>(bp.cells[c].x) + cursor[c]] =
            static_cast<uint32_t>(i);
        ++cursor[c];
      }
  }
  return bp;
}

}  // namespace badlands::fog
