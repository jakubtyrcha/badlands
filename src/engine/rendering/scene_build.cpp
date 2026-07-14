#include "engine/rendering/scene_build.hpp"

#include <utility>

#include "engine/scene/scene_attachment.hpp"

namespace badlands {

NodeHandle AddMeshEntity(SceneGraph& scene, const char* name,
                         TexturedMeshResult&& mesh, const DeferredMaterial& mat,
                         const glm::mat4& transform) {
  NodeHandle node = scene.CreateNode(name);
  scene.SetLocalTransform(node, Trs::FromMatrix(transform));

  ResolvedMesh resolved{
      .vertices = std::move(mesh.mesh.vertices),
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

}  // namespace badlands
