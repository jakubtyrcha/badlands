#pragma once

// Ported from sampo's src/scene/scene_attachment.hpp, namespace sampo ->
// badlands.
//
// Deviation: `SceneAttachment` drops sampo's `SkyboxAttachment` and
// `WaterAttachment` variants. `WaterAttachment` maps to
// `SphericalWaterVolume` (sampo's `rendering/components/water_components.hpp`,
// not ported — a self-contained water-rendering subsystem out of scope here).
// `SkyboxAttachment` itself is a single `wgpu::TextureView` field, but its
// only effect (`SceneContext::skybox_cubemap`) was explicitly trimmed from
// the ported `SceneContext` (see scene_context.hpp's trim note, which calls
// out "skybox" by name as an unported subsystem not to pull in). Both are
// straightforward to re-add alongside whichever future task ports
// skybox/water rendering. `MeshAttachment`, `DirectionalLightAttachment`,
// `AmbientLightAttachment` (the task brief's named types) and
// `SphereAttachment`/`LodSphereAttachment`/`ScriptAttachment` (which need no
// unported subsystem — all their fields resolve to already-ported types) are
// kept verbatim.

#include <array>
#include <glm/glm.hpp>
#include <variant>
#include <vector>

#include "core/geometry_type.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"

namespace badlands {

// Resolved mesh data for scene graph attachments
struct ResolvedMesh {
  std::vector<float> vertices;
  std::vector<uint32_t> indices;
  uint32_t vertex_count{0};
  GeometryType geometry_type{GeometryType::kTexturedMesh};
  Aabb local_bounds;  // Pre-computed local-space AABB
};

// Mesh attachment - creates StaticTexturedMeshComponent + MaterialFactoryComponent
struct MeshAttachment {
  ResolvedMesh mesh;

  // Material factory (shared, not owned)
  MaterialInstanceFactory* factory{nullptr};
  MaterialPassType pass_type{MaterialPassType::kDeferred};
  InstanceParams params;
};

// Directional light config (affects SceneContext, not entity creation)
struct DirectionalLightAttachment {
  glm::vec3 direction{0.0f, 1.0f, 0.0f};
  glm::vec3 color{1.0f, 1.0f, 1.0f};
};

// Ambient light config (SH coefficients)
struct AmbientLightAttachment {
  std::array<glm::vec3, 9> sh_coefficients{};
};

// Sphere mesh (rendered with cubemap sampling)
struct SphereAttachment {
  float radius{1.0f};
  MaterialInstanceFactory* factory{nullptr};
  MaterialPassType pass_type{MaterialPassType::kDeferred};
  InstanceParams params;
  bool forward_pass{true};
};

// Dynamic LOD sphere with displacement
struct LodSphereAttachment {
  float radius{1.0f};
  MaterialInstanceFactory* factory{nullptr};
  MaterialPassType pass_type{MaterialPassType::kDeferred};
  InstanceParams params;
};

// Script attachment placeholder
struct ScriptAttachment {};

// The attachment variant
using SceneAttachment =
    std::variant<MeshAttachment, DirectionalLightAttachment,
                 AmbientLightAttachment, SphereAttachment, LodSphereAttachment,
                 ScriptAttachment>;

}  // namespace badlands
