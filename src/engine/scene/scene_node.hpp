#pragma once

// Ported from sampo's src/scene/scene_node.hpp, namespace sampo -> badlands,
// verbatim otherwise.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

#include "engine/scene/scene_attachment.hpp"

namespace badlands {

// Unique identifier for scene nodes (stable across edits)
struct NodeHandle {
  uint64_t id{0};

  constexpr bool IsValid() const { return id != 0; }
  constexpr explicit operator bool() const { return IsValid(); }

  constexpr bool operator==(const NodeHandle& other) const {
    return id == other.id;
  }
  constexpr bool operator!=(const NodeHandle& other) const {
    return id != other.id;
  }
  constexpr auto operator<=>(const NodeHandle& other) const = default;
};

// Hash support for containers
struct NodeHandleHash {
  std::size_t operator()(const NodeHandle& h) const noexcept {
    return std::hash<uint64_t>{}(h.id);
  }
};

// Null node constant
inline constexpr NodeHandle kNullNode{};

// Local transform in TRS (translation, rotation, scale) form
// More intuitive for editing than matrices
struct Trs {
  glm::vec3 position{0.0f};
  glm::quat rotation{glm::identity<glm::quat>()};  // wxyz order
  glm::vec3 scale{1.0f};

  // Convert to mat4
  glm::mat4 ToMatrix() const {
    glm::mat4 m = glm::mat4(1.0f);
    m = glm::translate(m, position);
    m = m * glm::mat4_cast(rotation);
    m = glm::scale(m, scale);
    return m;
  }

  // Create from mat4 (decompose) - for import/conversion
  static Trs FromMatrix(const glm::mat4& m) {
    Trs result;
    result.position = glm::vec3(m[3]);

    // Extract scale from column vectors
    result.scale.x = glm::length(glm::vec3(m[0]));
    result.scale.y = glm::length(glm::vec3(m[1]));
    result.scale.z = glm::length(glm::vec3(m[2]));

    // Extract rotation (normalize columns first)
    glm::mat3 rot_mat;
    rot_mat[0] = glm::vec3(m[0]) / result.scale.x;
    rot_mat[1] = glm::vec3(m[1]) / result.scale.y;
    rot_mat[2] = glm::vec3(m[2]) / result.scale.z;
    result.rotation = glm::quat_cast(rot_mat);

    return result;
  }

  // Identity transform
  static Trs Identity() { return Trs{}; }
};

// A node in the scene hierarchy
struct SceneNode {
  NodeHandle handle;
  std::string name;
  Trs local_transform;

  // Hierarchy
  NodeHandle parent;  // kNullNode = root or top-level
  std::vector<NodeHandle> children;

  // Attachments
  std::vector<SceneAttachment> attachments;

  // Visibility flag
  bool visible{true};
};

}  // namespace badlands
