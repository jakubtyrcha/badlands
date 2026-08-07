#include "executables/object_viewer/plane_mesh.hpp"

#include <spdlog/spdlog.h>

namespace badlands::object_viewer {

namespace {

// Compile-time, because they are structural. The extent and the tiling are
// runtime because they are size; the subdivision is not.
constexpr int kQuadsPerSide = 8;

}  // namespace

PlaneMesh BuildPlaneMesh(float half_extent, float uv_tiles) {
  PlaneMesh mesh;
  constexpr int kVertsPerSide = kQuadsPerSide + 1;
  mesh.vertices.reserve(size_t(kVertsPerSide) * kVertsPerSide);

  for (int z = 0; z < kVertsPerSide; ++z) {
    for (int x = 0; x < kVertsPerSide; ++x) {
      const float u = float(x) / float(kQuadsPerSide);
      const float v = float(z) / float(kQuadsPerSide);
      // [0,1] -> [-half, +half].
      const float px = (u * 2.0f - 1.0f) * half_extent;
      const float pz = (v * 2.0f - 1.0f) * half_extent;

      PlaneVertex vert;
      // Normal is +y everywhere: a flat plane. That is what makes the `normal`
      // debug view a closed-form assertion -- a flat normal map over a flat
      // plane must resolve to exactly (0,1,0).
      vert.pos_nx = {px, 0.0f, pz, 0.0f};        // w = normal.x
      vert.nyz_uv = {1.0f, 0.0f, u * uv_tiles, v * uv_tiles};  // normal.yz, uv
      // Tangent along +x, so the tangent frame is (T=+x, N=+y, B=T x N * sign).
      // w = +1: with B = w * cross(N, T) = cross(+y, +x) = -z, the frame is
      // right-handed against a UV layout whose v increases with +z.
      vert.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
      mesh.vertices.push_back(vert);
    }
  }

  mesh.indices.reserve(size_t(kQuadsPerSide) * kQuadsPerSide * 6);
  for (int z = 0; z < kQuadsPerSide; ++z) {
    for (int x = 0; x < kQuadsPerSide; ++x) {
      const uint32_t i0 = uint32_t(z * kVertsPerSide + x);
      const uint32_t i1 = i0 + 1;
      const uint32_t i2 = i0 + kVertsPerSide;
      const uint32_t i3 = i2 + 1;
      // Counter-clockwise seen from +y. Getting this backwards renders nothing
      // under CullMode::Back and reads as a missing draw call rather than as a
      // winding mistake, so it is asserted in the tests rather than trusted.
      mesh.indices.insert(mesh.indices.end(), {i0, i2, i1});
      mesh.indices.insert(mesh.indices.end(), {i1, i2, i3});
    }
  }
  return mesh;
}

bool ValidatePrimitiveCount(const PlaneMesh& mesh) {
  if (mesh.indices.size() % 3 != 0) {
    spdlog::error(
        "object_viewer: the plane mesh has {} indices, which is not a whole "
        "number of triangles",
        mesh.indices.size());
    return false;
  }
  if (mesh.TriangleCount() > kMaxPrimitivesPerDraw) {
    spdlog::error(
        "object_viewer: {} triangles exceeds the {} the visibility buffer can "
        "address -- the primitive field would overflow into the draw slot and "
        "the resolve would fetch an out-of-bounds DrawInfo",
        mesh.TriangleCount(), kMaxPrimitivesPerDraw);
    return false;
  }
  return true;
}

}  // namespace badlands::object_viewer
