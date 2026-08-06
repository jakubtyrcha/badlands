#include "engine/rendering/geometry/usd_mesh_adapter.hpp"

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

namespace badlands {
namespace {

// USD's Z-up basis expressed in the engine's Y-up one: (x, y, z)_usd becomes
// (x, z, -y)_engine.
//
// This is a rotation of -90 degrees about X, i.e. the usual Z-up -> Y-up
// convention, and it is orientation-PRESERVING (determinant +1). That matters:
// a mirror would flip triangle winding and quietly backface-cull the whole
// model, so the index buffer is copied through untouched.
glm::vec3 ZUpToYUp(const glm::vec3& v) { return {v.x, v.z, -v.y}; }

// Reads vertex `i` of a flat xyz array.
glm::vec3 Vec3At(const std::vector<float>& src, size_t i) {
  return {src[i * 3 + 0], src[i * 3 + 1], src[i * 3 + 2]};
}

}  // namespace

std::vector<ImportedModel> BuildImportedModels(
    const UsdSceneData& scene, const UsdMaterialBinding& binding) {
  std::vector<ImportedModel> out;
  if (!scene.ok) return out;

  const float scale = scene.meters_per_unit;

  for (const auto& src : scene.meshes) {
    const size_t vertex_count = src.vertex_count();
    if (vertex_count == 0 || src.indices.empty()) continue;

    // Every attribute is required: the engine's vertex layout has no way to
    // say "this one is absent", so a partial mesh would ship zeroed floats.
    if (src.normals.size() != vertex_count * 3 ||
        src.uvs.size() != vertex_count * 2 ||
        src.tangents.size() != vertex_count * 3) {
      spdlog::warn("BuildImportedModels: mesh '{}' is missing normals, UVs or"
                   " tangents -- skipping",
                   src.name);
      continue;
    }

    ImportedModel model;
    model.name = src.name;
    model.pack_dir = ResolvePackDir(src.material_name, binding);

    StaticTexturedMeshComponent& mesh = model.mesh.mesh;
    mesh.vertices.resize(vertex_count * kTexturedMeshFloatsPerVertex);
    mesh.vertex_count = static_cast<uint32_t>(vertex_count);
    mesh.indices = src.indices;
    mesh.geometry_type = GeometryType::kTexturedMesh;

    for (size_t i = 0; i < vertex_count; ++i) {
      glm::vec3 position = Vec3At(src.positions, i) * scale;
      glm::vec3 normal = Vec3At(src.normals, i);
      glm::vec3 tangent = Vec3At(src.tangents, i);

      if (scene.z_up) {
        position = ZUpToYUp(position);
        // Directions rotate too, but are NOT scaled -- metersPerUnit is a
        // length conversion and a normal is not a length.
        normal = ZUpToYUp(normal);
        tangent = ZUpToYUp(tangent);
      }

      float* v = &mesh.vertices[i * kTexturedMeshFloatsPerVertex];
      v[0] = position.x;
      v[1] = position.y;
      v[2] = position.z;
      v[3] = src.uvs[i * 2 + 0];
      // V flips: USD's `st` puts (0,0) at the texture's LOWER-left, while the
      // sampler addresses row 0 at the TOP. Passing it through would mirror
      // every model vertically -- which on an atlased texture is not a subtle
      // error but a wrong-region read (rock_moss_set_01's atlas has a black
      // bottom third that the unflipped V sampled directly).
      v[4] = 1.0f - src.uvs[i * 2 + 1];
      v[5] = normal.x;
      v[6] = normal.y;
      v[7] = normal.z;
      v[8] = tangent.x;
      v[9] = tangent.y;
      v[10] = tangent.z;
    }

    model.mesh.local_bounds = ComputeLocalAabb(mesh);
    out.push_back(std::move(model));
  }

  return out;
}

}  // namespace badlands
