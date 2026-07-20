#pragma once

// Lake surface mesh: a flat water plane covering the interior of a closed 2D
// polygon (world XZ), tessellated on a regular grid at `density` metre spacing.
// No LOD — uniform density. Concave-safe (even-odd point-in-polygon). The
// boundary is stair-stepped to the grid; the water material's coast blend hides
// it. Waves/normals are added in the shader from an analytic wave function
// (engine/rendering/water_waves.hpp) — this mesh is flat at `y`.

#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

// Even-odd (ray-crossing) point-in-polygon test in the XZ plane. Exposed so the
// generator and its tests share one predicate (a vertex the generator keeps
// re-tests as inside). `path` is a closed ring given as ordered vertices (the
// closing edge from last->first is implicit).
bool PointInPolygon(const glm::vec2& p, const std::vector<glm::vec2>& path);

// Tessellated flat water surface at world height `y`, spanning the interior of
// the closed `path` on a `density`-spaced XZ grid. A grid cell is emitted (two
// triangles, CCW seen from +Y) iff its CENTRE is inside the polygon
// (rasterize-at-cell-resolution) — so the interior fills with no holes for any
// region at least ~one cell wide, at the cost of the boundary possibly extending
// up to half a cell past the edge (hidden by the shader's coast blend). Normal
// is +Y, tangent +X, UV = world XZ. Non-indexed triangle list. Empty for < 3
// path vertices or a degenerate/empty interior.
TexturedMeshResult GenerateLakeSurfaceMesh(const std::vector<glm::vec2>& path,
                                           float density = 1.0f, float y = 0.0f);

}  // namespace badlands
