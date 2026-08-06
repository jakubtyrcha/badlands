#include "engine/rendering/geometry/usd_mesh_adapter.hpp"

#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // glm::inverseTranspose
#include <glm/gtc/type_ptr.hpp>        // glm::make_mat4
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

// glm::normalize on a zero-length vector divides by zero and yields NaN, and a
// NaN tangent poisons the Gram-Schmidt frame in gbuffer_encode.wesl -- black or
// garbage pixels with nothing logged, the exact failure the missing-attribute
// guard exists to prevent. Zero tangents are not hypothetical: Tydra's solver
// produces one for any triangle whose UVs have zero area.
//
// `fallback` is returned instead, which is wrong for that vertex but bounded.
glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
  const float len2 = glm::dot(v, v);
  if (!(len2 > 1e-20f)) return fallback;  // also catches NaN input
  return v * glm::inversesqrt(len2);
}

// Some unit vector perpendicular to `n`.
//
// A CONSTANT fallback tangent is not good enough, and the shader is why:
// gbuffer_encode.wesl re-orthogonalises with normalize(T - N * dot(T, N)), so a
// tangent parallel to the normal makes that normalize(vec3(0)) -> NaN. A
// degenerate-UV triangle whose normal happens to point along the constant is
// exactly the case the fallback exists to survive, so the fallback has to be
// derived from the normal rather than fixed.
//
// Cross with whichever axis `n` is least aligned to, so the product is never
// near-zero.
glm::vec3 AnyPerpendicular(const glm::vec3& n) {
  const glm::vec3 axis = std::abs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                              : glm::vec3(0.0f, 1.0f, 0.0f);
  return glm::normalize(glm::cross(axis, n));
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
        src.tangents.size() != vertex_count * 4) {
      spdlog::warn("BuildImportedModels: mesh '{}' is missing normals, UVs or"
                   " tangents -- skipping",
                   src.name);
      continue;
    }

    ImportedModel model;
    model.name = src.name;
    model.pack_dir = ResolvePackDir(src.material_name, binding);

    // The prim's world placement. Points are authored in the prim's own local
    // space, so without this every part of a multi-prim model lands on the
    // origin (a treasure chest's lid inside its body).
    const glm::mat4 xform = glm::make_mat4(src.transform.data());
    // Normals need the inverse-transpose, not the matrix: under non-uniform
    // scale the plain matrix leaves them off the surface rather than
    // perpendicular to it. Tangents are direction vectors ALONG the surface,
    // so they take the matrix itself.
    const glm::mat3 normal_xform = glm::inverseTranspose(glm::mat3(xform));
    const glm::mat3 tangent_xform{xform};
    // Two independent things reverse triangle winding, so they XOR: a
    // mirroring transform (negative determinant), and USD `orientation` being
    // leftHanded. Either alone needs the indices flipped back or the part is
    // backface-culled into invisibility; both together cancel out.
    const bool mirrored = glm::determinant(glm::mat3(xform)) < 0.0f;
    const bool flip_winding = mirrored != !src.right_handed;

    StaticTexturedMeshComponent& mesh = model.mesh.mesh;
    mesh.vertices.resize(vertex_count * kTexturedMeshFloatsPerVertex);
    mesh.vertex_count = static_cast<uint32_t>(vertex_count);
    mesh.indices = src.indices;
    mesh.geometry_type = GeometryType::kTexturedMesh;
    if (flip_winding) {
      for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
      }
    }

    for (size_t i = 0; i < vertex_count; ++i) {
      // Place first, THEN convert units: metersPerUnit describes the stage, so
      // it applies to the already-placed point rather than to local coordinates
      // that the transform would then scale again.
      glm::vec3 position =
          glm::vec3(xform * glm::vec4(Vec3At(src.positions, i), 1.0f)) * scale;
      // Re-normalized because the transform may carry scale, and a shortened
      // normal darkens the surface. The fallbacks only fire on a degenerate
      // input (see SafeNormalize); they are arbitrary but finite, which a
      // NaN is not.
      glm::vec3 normal = SafeNormalize(normal_xform * Vec3At(src.normals, i),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
      const glm::vec3 tangent_xyz{src.tangents[i * 4 + 0], src.tangents[i * 4 + 1],
                                  src.tangents[i * 4 + 2]};
      glm::vec3 tangent =
          SafeNormalize(tangent_xform * tangent_xyz, AnyPerpendicular(normal));
      // Handedness describes the UV parameterisation, so a placement transform
      // leaves it alone -- except a MIRRORING one, which reverses the frame's
      // chirality and so flips it.
      float handedness = src.tangents[i * 4 + 3] < 0.0f ? -1.0f : 1.0f;
      if (mirrored) handedness = -handedness;

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
      v[11] = handedness;
    }

    model.mesh.local_bounds = ComputeLocalAabb(mesh);
    out.push_back(std::move(model));
  }

  return out;
}

}  // namespace badlands
