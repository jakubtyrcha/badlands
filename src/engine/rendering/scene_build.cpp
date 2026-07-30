#include "engine/rendering/scene_build.hpp"

#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/scene/scene_attachment.hpp"

namespace badlands {

NodeHandle AddMeshEntity(SceneGraph& scene, const char* name,
                         TexturedMeshResult&& mesh, const DeferredMaterial& mat,
                         const glm::mat4& transform) {
  NodeHandle node = scene.CreateNode(name);
  scene.SetLocalTransform(node, Trs::FromMatrix(transform));

  ResolvedMesh resolved{
      .vertices = std::move(mesh.mesh.vertices),
      .indices = std::move(mesh.mesh.indices),
      .vertex_count = mesh.mesh.vertex_count,
      .geometry_type = mesh.mesh.geometry_type,
      .local_bounds = mesh.local_bounds,
  };

  scene.AddAttachment(node, MeshAttachment{
                                .mesh = std::move(resolved),
                                .factory = mat.factory,
                                .pass_type = MaterialPassType::kDeferred,
                                .params = mat.params,
                            });

  return node;
}

NodeHandle AddTransparentMeshEntity(SceneGraph& scene, const char* name,
                                    TexturedMeshResult&& mesh,
                                    MaterialInstanceFactory* factory,
                                    const InstanceParams& params,
                                    const glm::mat4& transform) {
  NodeHandle node = scene.CreateNode(name);
  scene.SetLocalTransform(node, Trs::FromMatrix(transform));

  ResolvedMesh resolved{
      .vertices = std::move(mesh.mesh.vertices),
      .indices = std::move(mesh.mesh.indices),
      .vertex_count = mesh.mesh.vertex_count,
      .geometry_type = mesh.mesh.geometry_type,
      .local_bounds = mesh.local_bounds,
  };

  scene.AddAttachment(node,
                      MeshAttachment{
                          .mesh = std::move(resolved),
                          .factory = factory,
                          .pass_type = MaterialPassType::kForwardTransparent,
                          .params = params,
                      });

  return node;
}

NodeHandle AddForwardOpaqueMeshEntity(SceneGraph& scene, const char* name,
                                      TexturedMeshResult&& mesh,
                                      MaterialInstanceFactory* factory,
                                      const InstanceParams& params,
                                      const glm::mat4& transform) {
  NodeHandle node = scene.CreateNode(name);
  scene.SetLocalTransform(node, Trs::FromMatrix(transform));

  ResolvedMesh resolved{
      .vertices = std::move(mesh.mesh.vertices),
      .indices = std::move(mesh.mesh.indices),
      .vertex_count = mesh.mesh.vertex_count,
      .geometry_type = mesh.mesh.geometry_type,
      .local_bounds = mesh.local_bounds,
  };

  scene.AddAttachment(node,
                      MeshAttachment{
                          .mesh = std::move(resolved),
                          .factory = factory,
                          .pass_type = MaterialPassType::kForwardOpaque,
                          .params = params,
                      });

  return node;
}

namespace {

// Shared quad/transform construction for both AddFloor overloads below --
// only how `mat` is obtained differs between them.
NodeHandle AddFloorQuad(SceneGraph& scene, float size, float uv_scale,
                        const DeferredMaterial& mat) {
  auto quad = GenerateQuadTexturedMesh(size, /*resolution=*/1, uv_scale);

  // GenerateQuadTexturedMesh spans X/Y at Z=0 with normal +Z; rotate -90deg
  // about X so the normal becomes +Y (up) and the quad spans X/Z at Y=0.
  const glm::mat4 transform = glm::rotate(
      glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

  return AddMeshEntity(scene, "floor", std::move(quad), mat, transform);
}

}  // namespace

NodeHandle AddFloor(SceneGraph& scene, MaterialLibrary& matlib, float size,
                    const std::string& pack_dir, float uv_scale) {
  return AddFloorQuad(scene, size, uv_scale, matlib.Get(pack_dir));
}

NodeHandle AddFloor(SceneGraph& scene, float size, const DeferredMaterial& mat,
                    float uv_scale) {
  return AddFloorQuad(scene, size, uv_scale, mat);
}

}  // namespace badlands
