#pragma once

// Ported from sampo's src/scene/scene_graph.hpp, namespace sampo -> badlands,
// verbatim otherwise (includes adapted to badlands paths).

#include <array>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string_view>
#include <unordered_map>

#include "engine/scene/scene_node.hpp"

namespace badlands {

struct SceneContext;

// SceneGraph is a hierarchical, data-driven scene representation.
// It serves as the "Document" in the Document/View architecture:
// - Hierarchical node structure for intuitive scene authoring
// - Pre-resolved data with MaterialInstanceFactory references
// - Serialization-friendly design
//
// The runtime "View" is an entt::registry populated via SyncToRegistry().
//
// Usage:
//   SceneGraph scene;
//   auto node = scene.CreateNode("my_object");
//   scene.SetLocalTransform(node, {.position = {1, 2, 3}});
//   scene.AddAttachment(node, MeshAttachment{...});
//   scene.SyncToRegistry(registry, context);
class SceneGraph {
 public:
  SceneGraph();
  ~SceneGraph();

  // Non-copyable, movable
  SceneGraph(const SceneGraph&) = delete;
  SceneGraph& operator=(const SceneGraph&) = delete;
  SceneGraph(SceneGraph&&) noexcept;
  SceneGraph& operator=(SceneGraph&&) noexcept;

  // === Node Management ===

  // Create a new node (optionally under a parent)
  // Returns handle to the created node
  NodeHandle CreateNode(std::string_view name = "", NodeHandle parent = {});

  // Remove a node and all its descendants
  void DestroyNode(NodeHandle node);

  // Get the root node handle (always valid after construction)
  NodeHandle GetRoot() const { return root_; }

  // === Node Access ===

  SceneNode* GetNode(NodeHandle handle);
  const SceneNode* GetNode(NodeHandle handle) const;

  // Get node by name (linear search, use sparingly)
  NodeHandle FindNode(std::string_view name) const;

  // === Transform API ===

  void SetLocalTransform(NodeHandle node, const Trs& transform);
  Trs GetLocalTransform(NodeHandle node) const;

  // Compute world transform (walks up hierarchy)
  glm::mat4 GetWorldTransform(NodeHandle node) const;

  // Convenience setters for individual components
  void SetPosition(NodeHandle node, glm::vec3 pos);
  void SetRotation(NodeHandle node, glm::quat rot);
  void SetScale(NodeHandle node, glm::vec3 scale);

  // === Hierarchy API ===

  void Reparent(NodeHandle node, NodeHandle new_parent);
  std::vector<NodeHandle> GetChildren(NodeHandle node) const;
  NodeHandle GetParent(NodeHandle node) const;

  // === Attachment API ===

  // Add an attachment to a node
  template <typename T>
  void AddAttachment(NodeHandle node, T&& attachment);

  // Remove all attachments of a specific type from a node
  template <typename T>
  void RemoveAttachments(NodeHandle node);

  // Clear all attachments from a node
  void ClearAttachments(NodeHandle node);

  // === Scene-Level Lighting (convenience) ===

  void SetSunDirection(glm::vec3 dir);
  void SetSunColor(glm::vec3 color);
  void SetAmbient(glm::vec3 color);  // Sets flat L0 SH only
  void SetAmbientSH(const std::array<glm::vec3, 9>& sh);  // Full SH L2
  void SetClearColor(glm::vec4 color);

  glm::vec3 GetSunDirection() const { return sun_direction_; }
  glm::vec3 GetSunColor() const { return sun_color_; }
  const std::array<glm::vec3, 9>& GetAmbientSH() const { return ambient_sh_; }
  glm::vec4 GetClearColor() const { return clear_color_; }

  // === Synchronization ===

  // Sync scene graph to ECS registry
  // Clears registry and rebuilds all entities from the scene graph
  void SyncToRegistry(entt::registry& registry, SceneContext& context);

  // === Iteration ===

  // Iterate all nodes (unspecified order)
  template <typename Fn>
  void ForEachNode(Fn&& fn);

  template <typename Fn>
  void ForEachNode(Fn&& fn) const;

 private:
  // Node storage - dense map for cache-friendly iteration
  std::unordered_map<NodeHandle, SceneNode, NodeHandleHash> nodes_;

  // Root node (implicit, always exists)
  NodeHandle root_;

  // ID generation
  uint64_t next_id_{1};
  NodeHandle AllocateHandle();

  // Scene-level lighting (applied to SceneContext during sync)
  glm::vec3 sun_direction_{0.0f, 1.0f, 0.0f};
  glm::vec3 sun_color_{1.0f, 1.0f, 1.0f};
  std::array<glm::vec3, 9> ambient_sh_{};
  glm::vec4 clear_color_{0.1f, 0.1f, 0.1f, 1.0f};

  // Sync helpers
  void SyncNodeRecursive(NodeHandle handle, const glm::mat4& parent_world,
                         entt::registry& registry, SceneContext& context);

  void ProcessAttachment(const SceneAttachment& attachment,
                         const glm::mat4& world_transform,
                         entt::registry& registry, SceneContext& context);
};

// === Template Implementations ===

template <typename T>
void SceneGraph::AddAttachment(NodeHandle node, T&& attachment) {
  if (auto* n = GetNode(node)) {
    n->attachments.emplace_back(std::forward<T>(attachment));
  }
}

template <typename T>
void SceneGraph::RemoveAttachments(NodeHandle node) {
  if (auto* n = GetNode(node)) {
    std::erase_if(n->attachments, [](const SceneAttachment& att) {
      return std::holds_alternative<T>(att);
    });
  }
}

template <typename Fn>
void SceneGraph::ForEachNode(Fn&& fn) {
  for (auto& [handle, node] : nodes_) {
    fn(node);
  }
}

template <typename Fn>
void SceneGraph::ForEachNode(Fn&& fn) const {
  for (const auto& [handle, node] : nodes_) {
    fn(node);
  }
}

}  // namespace badlands
