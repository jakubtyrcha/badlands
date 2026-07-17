#pragma once

// Volumetric-fog cascade addressing math — a height-band clipmap (Task: fog
// rendering). Pure CPU, glm-only (no Dawn), header-only so it is unit-testable
// without a GPU. The GPU side (VolumetricFog + shaders/common/fog_cascade.wesl)
// mirrors this same world<->voxel + toroidal-wrap math so a filled voxel reads
// back exactly what was written.
//
// Model: N cascades share a fixed vertical band [floor_y, floor_y + height]
// (res_y slices). Cascade i nests only horizontally, with XZ half-extent
// base_half_extent * 2^i (voxel size doubles per level), centered on the
// camera's XZ. Storage is toroidal: texel = worldVoxel mod res_xz, so the
// world-space volume is stable as the camera roams and only newly-exposed
// voxel columns need refilling.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

namespace badlands::fog {

struct CascadeLayout {
  int cascade_count = 3;
  int res_xz = 128;
  int res_y = 32;
  float base_half_extent = 64.0f;  // cascade 0 XZ half-extent (world metres)
  float floor_y = 0.0f;            // bottom of the fog band (water line)
  float height = 64.0f;            // band height (metres)
};

// A texel-space sub-box of a cascade to (re)fill: XZ offset + size, full Y.
struct DirtyBox {
  int ox, oz, sx, sz;
  bool operator==(const DirtyBox&) const = default;
};

// --- Cascade geometry -------------------------------------------------------

// Cascade i XZ half-extent (metres): base * 2^i.
inline float CascadeHalfExtent(const CascadeLayout& L, int i) {
  return L.base_half_extent * std::exp2f(static_cast<float>(i));
}
// Full world width covered by cascade i in X (and Z).
inline float CascadeFullExtent(const CascadeLayout& L, int i) {
  return 2.0f * CascadeHalfExtent(L, i);
}
// World size of one voxel in X/Z for cascade i.
inline float CascadeVoxelSizeXZ(const CascadeLayout& L, int i) {
  return CascadeFullExtent(L, i) / static_cast<float>(L.res_xz);
}
// World height of one voxel (shared across cascades — same vertical band).
inline float CascadeVoxelSizeY(const CascadeLayout& L) {
  return L.height / static_cast<float>(L.res_y);
}

// --- Toroidal addressing ----------------------------------------------------

// Positive modulo: result in [0, n). Used to map a world voxel index to its
// stable texel (texel = worldVoxel mod res).
inline int PosMod(int a, int n) { return ((a % n) + n) % n; }

// World-voxel index of a cascade's min corner along one axis, snapped so the
// camera sits within a voxel of the window centre. Window covers world voxels
// [min, min + res_xz), i.e. world extent = res_xz * voxelSize = full extent.
inline int CascadeMinVoxel(const CascadeLayout& L, int i, float cam_axis) {
  const float voxel = CascadeVoxelSizeXZ(L, i);
  return static_cast<int>(std::floor(cam_axis / voxel)) - L.res_xz / 2;
}

// Finest (smallest-index) cascade whose half-extent covers `horizontal_distance`
// from the camera; -1 when the distance is beyond the coarsest cascade.
inline int SelectCascade(const CascadeLayout& L, float horizontal_distance) {
  for (int i = 0; i < L.cascade_count; ++i) {
    if (CascadeHalfExtent(L, i) >= horizontal_distance) return i;
  }
  return -1;
}

// --- Dirty-region computation (toroidal incremental fill) -------------------

namespace detail {
// Append the texel range [first_texel, first_texel+count) along X (full Z) as
// 1-2 boxes, splitting at the toroidal seam.
inline void AddWrappedX(std::vector<DirtyBox>& out, int first_texel, int count,
                        int res) {
  if (count <= 0) return;
  const int start = PosMod(first_texel, res);
  const int first_len = std::min(count, res - start);
  out.push_back(DirtyBox{start, 0, first_len, res});
  if (count > first_len) out.push_back(DirtyBox{0, 0, count - first_len, res});
}
// Same along Z (full X).
inline void AddWrappedZ(std::vector<DirtyBox>& out, int first_texel, int count,
                        int res) {
  if (count <= 0) return;
  const int start = PosMod(first_texel, res);
  const int first_len = std::min(count, res - start);
  out.push_back(DirtyBox{0, start, res, first_len});
  if (count > first_len) out.push_back(DirtyBox{0, 0, res, count - first_len});
}
}  // namespace detail

// Texel sub-boxes that must be refilled after the cascade window scrolls from
// `old_min` to `new_min` (per-axis world-voxel min corners). Returns the whole
// cascade on first fill / config change (`force`) or on a scroll of >= res on
// either axis (a teleport). Otherwise an X slab (entered columns, full Z) and/or
// a Z slab (entered rows, full X); their corner overlap is refilled twice, which
// is harmless (the fill is idempotent).
inline std::vector<DirtyBox> ComputeDirtyBoxes(int res, glm::ivec2 old_min,
                                               glm::ivec2 new_min, bool force) {
  // 64-bit deltas: old_min may be the INT_MIN "force full" sentinel (first
  // frame / after a resize), and a large camera jump could otherwise overflow
  // int subtraction (UB). Any |delta| >= res falls into the full-refill branch
  // below before old_min is used in the incremental arithmetic.
  const long long dx = static_cast<long long>(new_min.x) - old_min.x;
  const long long dz = static_cast<long long>(new_min.y) - old_min.y;
  if (force || std::llabs(dx) >= res || std::llabs(dz) >= res) {
    return {DirtyBox{0, 0, res, res}};
  }
  // Here |dx|,|dz| < res, so old_min is a real value within res of new_min
  // (never the sentinel) and old_min.x + res cannot overflow.
  std::vector<DirtyBox> out;
  if (dx > 0)
    detail::AddWrappedX(out, old_min.x + res, static_cast<int>(dx), res);
  else if (dx < 0)
    detail::AddWrappedX(out, new_min.x, static_cast<int>(-dx), res);
  if (dz > 0)
    detail::AddWrappedZ(out, old_min.y + res, static_cast<int>(dz), res);
  else if (dz < 0)
    detail::AddWrappedZ(out, new_min.y, static_cast<int>(-dz), res);
  return out;
}

}  // namespace badlands::fog
