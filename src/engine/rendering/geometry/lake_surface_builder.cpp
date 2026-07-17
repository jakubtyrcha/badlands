#include "engine/rendering/geometry/lake_surface_builder.hpp"

#include <algorithm>
#include <cmath>

#include "engine/rendering/geometry/mesh_builder_utils.hpp"

namespace badlands {

bool PointInPolygon(const glm::vec2& p, const std::vector<glm::vec2>& path) {
  bool inside = false;
  const size_t n = path.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const glm::vec2& a = path[i];
    const glm::vec2& b = path[j];
    // Ray-crossing: does the horizontal ray from p to +x cross edge (a,b)?
    if (((a.y > p.y) != (b.y > p.y)) &&
        (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
      inside = !inside;
    }
  }
  return inside;
}

TexturedMeshResult GenerateLakeSurfaceMesh(const std::vector<glm::vec2>& path,
                                           float density, float y) {
  TexturedMeshResult result;
  result.mesh.geometry_type = GeometryType::kTexturedMesh;
  if (path.size() < 3 || density <= 0.0f) {
    return result;
  }

  // Polygon bbox, snapped outward to the grid.
  glm::vec2 lo = path[0], hi = path[0];
  for (const auto& v : path) {
    lo = glm::min(lo, v);
    hi = glm::max(hi, v);
  }
  const float x0 = std::floor(lo.x / density) * density;
  const float z0 = std::floor(lo.y / density) * density;
  const int nx = static_cast<int>(std::ceil((hi.x - x0) / density)) + 1;
  const int nz = static_cast<int>(std::ceil((hi.y - z0) / density)) + 1;

  const glm::vec3 normal(0.0f, 1.0f, 0.0f);
  const glm::vec3 tangent(1.0f, 0.0f, 0.0f);
  auto& v = result.mesh.vertices;

  for (int iz = 0; iz + 1 < nz; ++iz) {
    for (int ix = 0; ix + 1 < nx; ++ix) {
      const float xa = x0 + static_cast<float>(ix) * density;
      const float xb = xa + density;
      const float za = z0 + static_cast<float>(iz) * density;
      const float zb = za + density;

      // Rasterize the polygon at cell resolution: keep a cell iff its CENTRE is
      // inside. Tests each cell once (no re-testing shared corners) and, unlike
      // an all-corners rule, never drops a boundary-straddling cell — so the
      // interior fills with no holes for any region at least ~one cell wide. The
      // boundary may extend up to half a cell past the edge; the coast blend
      // (and water self-hiding where terrain rises above the level) hides it.
      if (!PointInPolygon(glm::vec2(xa + 0.5f * density, za + 0.5f * density),
                          path)) {
        continue;
      }

      const glm::vec3 a(xa, y, za);
      const glm::vec3 b(xb, y, za);
      const glm::vec3 c(xb, y, zb);
      const glm::vec3 d(xa, y, zb);
      // Two triangles, wound CCW as seen from +Y (world XZ = UV).
      PushVertex(v, a, glm::vec2(a.x, a.z), normal, tangent);
      PushVertex(v, c, glm::vec2(c.x, c.z), normal, tangent);
      PushVertex(v, b, glm::vec2(b.x, b.z), normal, tangent);
      PushVertex(v, a, glm::vec2(a.x, a.z), normal, tangent);
      PushVertex(v, d, glm::vec2(d.x, d.z), normal, tangent);
      PushVertex(v, c, glm::vec2(c.x, c.z), normal, tangent);
    }
  }

  result.mesh.vertex_count =
      static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
  result.local_bounds =
      ComputeLocalAabbFromVertices(v, kTexturedMeshFloatsPerVertex);
  return result;
}

}  // namespace badlands
