#pragma once

// Hemi-octahedral view parameterization for the foliage impostor (LOD4).
//
// The impostor bakes the tree from a fixed set of directions and, at runtime,
// reconstructs an arbitrary view by blending the nearest baked ones. This
// header is the whole mapping between "a direction" and "which baked views, in
// what proportion" -- shared by the baker (which needs each view's direction)
// and the material (which needs the blend). Pure, header-only, glm only, so
// both sides cannot drift and the mapping is testable without a GPU.
//
// HEMISPHERE, not sphere: a tree is never viewed from underneath, and a full
// sphere would spend half the atlas on directions nothing samples.
//
// The square maps to the hemisphere with the CENTRE at the zenith (0,1,0) and
// the whole BOUNDARY at the horizon; the four corners are -Z, +X, +Z, -X.

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/glm.hpp>

namespace badlands {

// Baked views per axis, so kImpostorViewsPerAxis^2 views in total. The atlas
// stores them as a matching square grid of tiles (see impostor_atlas.hpp).
inline constexpr int kImpostorViewsPerAxis = 4;
inline constexpr int kImpostorViewCount =
    kImpostorViewsPerAxis * kImpostorViewsPerAxis;

// Flat index of the view at grid position (i, j). Row-major, matching the
// atlas' tile order.
inline constexpr int ImpostorViewIndex(int i, int j) {
  return j * kImpostorViewsPerAxis + i;
}

// Unit direction (y >= 0) -> uv in [0,1]^2.
//
// A direction below the horizon is projected onto it rather than rejected: a
// free camera can dip below a tree standing on a rise, and the nearest
// in-gamut answer is a far better failure than a NaN or a flipped view.
inline glm::vec2 HemiOctEncode(glm::vec3 d) {
  d.y = std::max(d.y, 0.0f);
  const float l1 = std::abs(d.x) + std::abs(d.y) + std::abs(d.z);
  if (!(l1 > 0.0f)) return glm::vec2(0.5f);  // degenerate input -> zenith
  const glm::vec3 n = d / l1;
  return glm::vec2(n.x + n.z, n.z - n.x) * 0.5f + 0.5f;
}

// uv in [0,1]^2 -> unit direction with y >= 0. Inverse of HemiOctEncode.
inline glm::vec3 HemiOctDecode(glm::vec2 uv) {
  const glm::vec2 f = uv * 2.0f - 1.0f;
  const float x = (f.x - f.y) * 0.5f;
  const float z = (f.x + f.y) * 0.5f;
  const float y = 1.0f - std::abs(x) - std::abs(z);
  return glm::normalize(glm::vec3(x, y, z));
}

// The direction view (i, j) is baked from.
//
// Views sit at TILE CENTRES, uv = (i + 0.5) / N, not at grid vertices. With a
// vertex layout, uv 0 and 1 are on the square's boundary -- which is the
// horizon -- so 12 of the 16 views would bake a horizontal elevation that a
// 50-58 degree game camera never looks from, leaving only 4 for everything it
// does. Centres put every view strictly above the horizon (elevations run
// ~18 to ~72 degrees) and spend the whole grid on directions that get sampled.
inline glm::vec3 ImpostorViewDirection(int i, int j) {
  const float n = static_cast<float>(kImpostorViewsPerAxis);
  return HemiOctDecode(glm::vec2((static_cast<float>(i) + 0.5f) / n,
                                 (static_cast<float>(j) + 0.5f) / n));
}

// The three baked views to blend for a given direction, with barycentric
// weights that are non-negative and sum to 1.
struct ImpostorBlend {
  std::array<int, 3> view{0, 0, 0};       // flat indices, see ImpostorViewIndex
  std::array<float, 3> weight{1.0f, 0.0f, 0.0f};
};

// Which views to blend for `dir_local` -- the direction from the tree TOWARD
// the viewer, in the tree's own (yaw-undone) space.
//
// Three and not four because the containing grid quad is split along its
// diagonal and only the containing TRIANGLE contributes. Note this is not the
// same as "the three nearest views": near a quad's corner the excluded corner
// can be closer than one of the three chosen. Triangle membership is the
// property that matters, because it is what makes the blend continuous -- a
// nearest-3 rule flips discontinuously as the query crosses a bisector, and a
// discontinuous blend is exactly the popping the impostor exists to avoid.
inline ImpostorBlend ImpostorBlendFor(glm::vec3 dir_local) {
  const glm::vec2 uv = HemiOctEncode(dir_local);
  const float n = static_cast<float>(kImpostorViewsPerAxis);

  // Into the lattice of tile CENTRES: centre (i,j) sits at lattice coord (i,j).
  const glm::vec2 g = uv * n - 0.5f;
  const int i0 = std::clamp(static_cast<int>(std::floor(g.x)), 0,
                            kImpostorViewsPerAxis - 2);
  const int j0 = std::clamp(static_cast<int>(std::floor(g.y)), 0,
                            kImpostorViewsPerAxis - 2);

  // Clamped, so a direction outside the outermost centres (the band between
  // the edge views and the true horizon) collapses onto the nearest edge views
  // instead of producing a negative weight.
  const float fx = std::clamp(g.x - static_cast<float>(i0), 0.0f, 1.0f);
  const float fy = std::clamp(g.y - static_cast<float>(j0), 0.0f, 1.0f);

  ImpostorBlend out;
  if (fx + fy <= 1.0f) {
    out.view = {ImpostorViewIndex(i0, j0), ImpostorViewIndex(i0 + 1, j0),
                ImpostorViewIndex(i0, j0 + 1)};
    out.weight = {1.0f - fx - fy, fx, fy};
  } else {
    out.view = {ImpostorViewIndex(i0 + 1, j0 + 1),
                ImpostorViewIndex(i0 + 1, j0), ImpostorViewIndex(i0, j0 + 1)};
    out.weight = {fx + fy - 1.0f, 1.0f - fy, 1.0f - fx};
  }
  return out;
}

}  // namespace badlands
