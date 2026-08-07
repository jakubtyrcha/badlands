#include "executables/object_viewer/sphere_grid.hpp"

#include <cmath>

namespace badlands::object_viewer {

namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

// The grid spans (columns-1) * spacing across and (rows-1) down, centred on the
// origin. Shared by the mesh build, the extent query and the headless oracle,
// so none of the three can disagree about where a sphere is.
glm::vec3 SphereGridCenter(uint32_t column, uint32_t row, float spacing) {
  const float x = (float(column) - float(kRoughnessSteps - 1) * 0.5f) * spacing;
  const float y = (float(kMetallicSteps - 1) * 0.5f - float(row)) * spacing;
  return {x, y, 0.0f};
}

float SphereRoughness(uint32_t column) {
  // 0 at the left, 1 at the right. The endpoints are included deliberately:
  // roughness 0 is the mirror that must reproduce the environment, and 1 is the
  // fully rough end where the multi-scatter energy deficit shows.
  return float(column) / float(kRoughnessSteps - 1);
}

float SphereMetallic(uint32_t row) { return row == 0 ? 0.0f : 1.0f; }

SphereGridBounds SphereGridExtent(float sphere_radius, float spacing) {
  SphereGridBounds b;
  b.sphere_radius = sphere_radius;
  const glm::vec3 first = SphereGridCenter(0, 0, spacing);
  const glm::vec3 last = SphereGridCenter(kRoughnessSteps - 1, kMetallicSteps - 1,
                                      spacing);
  b.center = (first + last) * 0.5f;
  b.radius = glm::length(last - b.center) + sphere_radius;
  return b;
}

SceneMesh BuildSphereGrid(float sphere_radius, float spacing) {
  SceneMesh mesh;

  // A UV sphere. Rings run pole to pole, segments around; the seam duplicates
  // the first column of vertices so u reaches 1 rather than wrapping to 0 --
  // without that the seam quad samples the whole texture backwards.
  const uint32_t cols = kSphereSegments + 1;
  mesh.vertices.reserve(size_t(cols) * (kSphereRings + 1));
  for (uint32_t ring = 0; ring <= kSphereRings; ++ring) {
    const float v = float(ring) / float(kSphereRings);
    const float theta = v * kPi;  // 0 at +Y, pi at -Y
    const float sin_t = std::sin(theta);
    const float cos_t = std::cos(theta);
    for (uint32_t seg = 0; seg <= kSphereSegments; ++seg) {
      const float u = float(seg) / float(kSphereSegments);
      const float phi = u * 2.0f * kPi;
      const float sin_p = std::sin(phi);
      const float cos_p = std::cos(phi);

      const glm::vec3 n{sin_t * cos_p, cos_t, sin_t * sin_p};
      const glm::vec3 p = n * sphere_radius;
      // Tangent along increasing phi, which is the direction u runs -- so the
      // normal map's U response follows the UV layout rather than fighting it.
      const glm::vec3 t{-sin_p, 0.0f, cos_p};

      MeshVertex vert;
      vert.pos_nx = {p.x, p.y, p.z, n.x};
      vert.nyz_uv = {n.y, n.z, u, v};
      // w = +1: B = w * cross(N, T), which is right-handed against v growing
      // towards -Y. The plane uses the same convention.
      vert.tangent = {t.x, t.y, t.z, 1.0f};
      mesh.vertices.push_back(vert);
    }
  }

  mesh.indices.reserve(size_t(kSphereSegments) * kSphereRings * 6);
  for (uint32_t ring = 0; ring < kSphereRings; ++ring) {
    for (uint32_t seg = 0; seg < kSphereSegments; ++seg) {
      const uint32_t i0 = ring * cols + seg;
      const uint32_t i1 = i0 + 1;
      const uint32_t i2 = i0 + cols;
      const uint32_t i3 = i2 + 1;
      // Counter-clockwise seen from OUTSIDE, matching FrontFace::Ccw with
      // CullMode::Back.
      //
      // NOT the plane's order, and that is the trap this got wrong first time.
      // The plane's rows advance along +z with the normal at +y; here ring
      // advances DOWNWARD (theta from the +Y pole) while seg advances
      // counter-clockwise seen from above, so the pair is handed the other way
      // round. Reversed, a convex sphere still renders as a filled disc -- you
      // simply see its inside back surface with inverted normals -- so the
      // image cannot show this and a test has to.
      mesh.indices.insert(mesh.indices.end(), {i0, i1, i2});
      mesh.indices.insert(mesh.indices.end(), {i1, i3, i2});
    }
  }

  // One instance per sphere. The draw slot IS the instance id, which is what
  // the visibility buffer packs, so no per-draw uniform is needed at all.
  mesh.draws.reserve(kSphereCount);
  for (uint32_t row = 0; row < kMetallicSteps; ++row) {
    for (uint32_t col = 0; col < kRoughnessSteps; ++col) {
      DrawInfo d;
      d.offset = glm::vec4(SphereGridCenter(col, row, spacing), 0.0f);
      d.material = glm::vec4(SphereRoughness(col), SphereMetallic(row), 0, 0);
      // BOTH overridden: the point of the chart is a controlled sweep, so the
      // pack's ARM map must not decide either axis.
      d.override_mask = kOverrideRoughness | kOverrideMetallic;
      mesh.draws.push_back(d);
    }
  }
  return mesh;
}

}  // namespace badlands::object_viewer
