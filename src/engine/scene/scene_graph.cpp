// Ported from sampo's src/scene/scene_graph.cpp, namespace sampo -> badlands.
//
// Deviations (see the deviation notes in scene_attachment.hpp and
// scene_context.hpp for the full rationale):
// - `ProcessAttachment`'s `WaterAttachment` and `SkyboxAttachment` branches
//   are dropped along with those two variant alternatives (unported
//   water/skybox subsystems).
// - `#include "core/math/spherical_harmonics.hpp"` is dropped: sampo's
//   scene_graph.cpp includes it but never calls anything in the `sh::`
//   namespace (confirmed by grep against sampo's source) — an
//   unused/vestigial include, same category as the one D1 dropped from
//   rendering_material_instance.hpp.
// - `#include "rendering/components/material_components.hpp"` is dropped;
//   `MapRenderable` (the only symbol from it this file uses) now lives in
//   forward_component.hpp — see that header's deviation note.
#include "engine/scene/scene_graph.hpp"

#include <entt/entt.hpp>

#include "engine/rendering/components/forward_component.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/components/transform.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

SceneGraph::SceneGraph() {
  // Create implicit root node
  root_ = AllocateHandle();
  auto& root_node = nodes_[root_];
  root_node.handle = root_;
  root_node.name = "root";
  root_node.parent = kNullNode;

  // Initialize ambient SH with default L0 term
  ambient_sh_[0] = glm::vec3(0.35f);
}

SceneGraph::~SceneGraph() = default;

SceneGraph::SceneGraph(SceneGraph&&) noexcept = default;
SceneGraph& SceneGraph::operator=(SceneGraph&&) noexcept = default;

NodeHandle SceneGraph::AllocateHandle() {
  return NodeHandle{next_id_++};
}

NodeHandle SceneGraph::CreateNode(std::string_view name, NodeHandle parent) {
  NodeHandle handle = AllocateHandle();

  auto& node = nodes_[handle];
  node.handle = handle;
  node.name = std::string(name);

  // Determine parent
  if (parent.IsValid()) {
    node.parent = parent;
  } else {
    node.parent = root_;
  }

  // Add to parent's children list
  if (auto* parent_node = GetNode(node.parent)) {
    parent_node->children.push_back(handle);
  }

  return handle;
}

void SceneGraph::DestroyNode(NodeHandle handle) {
  if (handle == root_) {
    return;  // Cannot destroy root
  }

  auto* node = GetNode(handle);
  if (!node) {
    return;
  }

  // Recursively destroy children first
  auto children_copy = node->children;  // Copy to avoid iterator invalidation
  for (auto child : children_copy) {
    DestroyNode(child);
  }

  // Remove from parent's children list
  if (auto* parent_node = GetNode(node->parent)) {
    std::erase(parent_node->children, handle);
  }

  // Remove this node
  nodes_.erase(handle);
}

SceneNode* SceneGraph::GetNode(NodeHandle handle) {
  auto it = nodes_.find(handle);
  return it != nodes_.end() ? &it->second : nullptr;
}

const SceneNode* SceneGraph::GetNode(NodeHandle handle) const {
  auto it = nodes_.find(handle);
  return it != nodes_.end() ? &it->second : nullptr;
}

NodeHandle SceneGraph::FindNode(std::string_view name) const {
  for (const auto& [handle, node] : nodes_) {
    if (node.name == name) {
      return handle;
    }
  }
  return kNullNode;
}

void SceneGraph::SetLocalTransform(NodeHandle handle, const Trs& transform) {
  if (auto* node = GetNode(handle)) {
    node->local_transform = transform;
  }
}

Trs SceneGraph::GetLocalTransform(NodeHandle handle) const {
  if (const auto* node = GetNode(handle)) {
    return node->local_transform;
  }
  return Trs::Identity();
}

glm::mat4 SceneGraph::GetWorldTransform(NodeHandle handle) const {
  const auto* node = GetNode(handle);
  if (!node) {
    return glm::mat4(1.0f);
  }

  // Walk up the hierarchy and accumulate transforms
  glm::mat4 world = node->local_transform.ToMatrix();

  NodeHandle current_parent = node->parent;
  while (current_parent.IsValid() && current_parent != root_) {
    if (const auto* parent_node = GetNode(current_parent)) {
      world = parent_node->local_transform.ToMatrix() * world;
      current_parent = parent_node->parent;
    } else {
      break;
    }
  }

  // Include root's transform if it's not identity
  if (current_parent == root_) {
    if (const auto* root_node = GetNode(root_)) {
      world = root_node->local_transform.ToMatrix() * world;
    }
  }

  return world;
}

void SceneGraph::SetPosition(NodeHandle handle, glm::vec3 pos) {
  if (auto* node = GetNode(handle)) {
    node->local_transform.position = pos;
  }
}

void SceneGraph::SetRotation(NodeHandle handle, glm::quat rot) {
  if (auto* node = GetNode(handle)) {
    node->local_transform.rotation = rot;
  }
}

void SceneGraph::SetScale(NodeHandle handle, glm::vec3 scale) {
  if (auto* node = GetNode(handle)) {
    node->local_transform.scale = scale;
  }
}

void SceneGraph::Reparent(NodeHandle handle, NodeHandle new_parent) {
  if (handle == root_ || !handle.IsValid()) {
    return;
  }

  auto* node = GetNode(handle);
  if (!node) {
    return;
  }

  // Remove from old parent
  if (auto* old_parent = GetNode(node->parent)) {
    std::erase(old_parent->children, handle);
  }

  // Set new parent
  node->parent = new_parent.IsValid() ? new_parent : root_;

  // Add to new parent's children
  if (auto* new_parent_node = GetNode(node->parent)) {
    new_parent_node->children.push_back(handle);
  }
}

std::vector<NodeHandle> SceneGraph::GetChildren(NodeHandle handle) const {
  if (const auto* node = GetNode(handle)) {
    return node->children;
  }
  return {};
}

NodeHandle SceneGraph::GetParent(NodeHandle handle) const {
  if (const auto* node = GetNode(handle)) {
    return node->parent;
  }
  return kNullNode;
}

void SceneGraph::ClearAttachments(NodeHandle handle) {
  if (auto* node = GetNode(handle)) {
    node->attachments.clear();
  }
}

void SceneGraph::SetSunDirection(glm::vec3 dir) {
  sun_direction_ = glm::normalize(dir);
}

void SceneGraph::SetSunColor(glm::vec3 color) {
  sun_color_ = color;
}

void SceneGraph::SetAmbient(glm::vec3 color) {
  // Set only L0 term for flat ambient
  ambient_sh_ = {};
  ambient_sh_[0] = color;
}

void SceneGraph::SetAmbientSH(const std::array<glm::vec3, 9>& sh) {
  ambient_sh_ = sh;
}

void SceneGraph::SetClearColor(glm::vec4 color) {
  clear_color_ = color;
}

void SceneGraph::SyncToRegistry(entt::registry& registry,
                                SceneContext& context) {
  registry.clear();

  context.sun_direction = glm::normalize(sun_direction_);
  context.sun_color = sun_color_;
  context.ambient_sh = ambient_sh_;
  context.clear_color = clear_color_;
  context.registry = &registry;

  SyncNodeRecursive(root_, glm::mat4(1.0f), registry, context);
}

void SceneGraph::SyncNodeRecursive(NodeHandle handle,
                                   const glm::mat4& parent_world,
                                   entt::registry& registry,
                                   SceneContext& context) {
  const auto* node = GetNode(handle);
  if (!node || !node->visible) {
    return;
  }

  glm::mat4 world = parent_world * node->local_transform.ToMatrix();

  for (const auto& attachment : node->attachments) {
    ProcessAttachment(attachment, world, registry, context);
  }

  for (const auto& child : node->children) {
    SyncNodeRecursive(child, world, registry, context);
  }
}

void SceneGraph::ProcessAttachment(const SceneAttachment& attachment,
                                   const glm::mat4& world_transform,
                                   entt::registry& registry,
                                   SceneContext& context) {
  std::visit(
      [&](const auto& att) {
        using T = std::decay_t<decltype(att)>;

        if constexpr (std::is_same_v<T, MeshAttachment>) {
          auto entity = registry.create();

          auto& transform = registry.emplace<Transform>(entity);
          transform.matrix = world_transform;

          // Create mesh component from pre-resolved mesh
          auto& mesh = registry.emplace<StaticTexturedMeshComponent>(entity);
          mesh.vertices = att.mesh.vertices;
          mesh.vertex_count = att.mesh.vertex_count;
          mesh.dirty = true;
          mesh.geometry_type = att.mesh.geometry_type;

          // Cache local-space AABB from pre-resolved bounds
          registry.emplace<StaticMeshAabbComponent>(
              entity, StaticMeshAabbComponent{.local = att.mesh.local_bounds});

          // Create MaterialFactoryComponent
          auto& fmc = registry.emplace<MaterialFactoryComponent>(entity);
          fmc.factory = att.factory;
          fmc.pass_type = att.pass_type;
          fmc.params = att.params;
          fmc.config_hash = ComputeFactoryConfigHash(fmc);

          // Route entity to appropriate render pass
          switch (att.pass_type) {
            case MaterialPassType::kForwardOpaque:
              registry.emplace<ForwardOpaqueRenderable>(entity);
              break;
            case MaterialPassType::kForwardTransparent:
              registry.emplace<ForwardTransparentRenderable>(entity);
              break;
            case MaterialPassType::kDeferred:
            default:
              registry.emplace<MapRenderable>(entity);
              break;
          }

        } else if constexpr (std::is_same_v<T, DirectionalLightAttachment>) {
          context.sun_direction = glm::normalize(att.direction);
          context.sun_color = att.color;

        } else if constexpr (std::is_same_v<T, AmbientLightAttachment>) {
          context.ambient_sh = att.sh_coefficients;

        } else if constexpr (std::is_same_v<T, SphereAttachment>) {
          auto entity = registry.create();

          auto& transform = registry.emplace<Transform>(entity);
          transform.matrix = world_transform;

          auto mesh = GenerateSphereCubeMapTexturedMesh(att.radius, 16);
          registry.emplace<StaticCubeMapTexturedMesh>(entity, std::move(mesh));

          // Create MaterialFactoryComponent
          auto& fmc = registry.emplace<MaterialFactoryComponent>(entity);
          fmc.factory = att.factory;
          fmc.pass_type = att.pass_type;
          fmc.params = att.params;
          fmc.config_hash = ComputeFactoryConfigHash(fmc);

          if (att.forward_pass) {
            registry.emplace<ForwardOpaqueRenderable>(entity);
          }

        } else if constexpr (std::is_same_v<T, LodSphereAttachment>) {
          (void)att;

        } else if constexpr (std::is_same_v<T, ScriptAttachment>) {
          (void)att;
        }
      },
      attachment);
}

}  // namespace badlands
