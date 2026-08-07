#include "executables/object_viewer/mesh_types.hpp"

#include <spdlog/spdlog.h>

namespace badlands::object_viewer {

bool ValidateSceneMesh(const SceneMesh& mesh) {
  if (mesh.indices.size() % 3 != 0) {
    spdlog::error(
        "object_viewer: the mesh has {} indices, which is not a whole number "
        "of triangles",
        mesh.indices.size());
    return false;
  }
  if (mesh.vertices.empty() || mesh.indices.empty()) {
    spdlog::error("object_viewer: the visibility buffer was given an empty mesh");
    return false;
  }
  if (mesh.draws.empty()) {
    spdlog::error(
        "object_viewer: the mesh has no draws -- an instanced draw of zero "
        "instances renders nothing, which reads as a missing pass");
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
  if (mesh.InstanceCount() > kMaxDrawSlots) {
    spdlog::error(
        "object_viewer: {} instances exceeds the {} draw slots the visibility "
        "buffer can address -- the draw field would wrap and instances would "
        "alias onto each other's materials",
        mesh.InstanceCount(), kMaxDrawSlots);
    return false;
  }
  return true;
}

}  // namespace badlands::object_viewer
