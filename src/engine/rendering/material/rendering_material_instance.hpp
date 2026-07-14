#pragma once

// Ported from sampo's src/rendering/material/rendering_material_instance.hpp,
// namespace sampo -> badlands.
//
// Deviation: `#include "rendering/components/mesh_components.hpp"` is
// dropped — nothing in this header (or standard_rendering_material_instance
// .cpp, its sole non-abstract implementation in the ported set) actually
// references any symbol from it; it was an unused/vestigial include in
// sampo's version. mesh_components.hpp is an ECS component header (entt
// registry component types), out of scope for the material system port.
#include <cstdint>
#include <string>
#include <variant>

#include <glm/glm.hpp>
#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/material.hpp"

namespace badlands {

class RenderPassContext;
class FrameContext;

struct MaterialParameterId {
  uint32_t handle{0};
  enum class Type : uint8_t {
    kInt,
    kUInt,
    kFloat,
    kVec2,
    kVec3,
    kVec4,
    kMat4
  } type{Type::kFloat};

  bool IsValid() const { return handle != 0; }
};

using MaterialParameterValue = std::variant<int32_t, uint32_t, float, glm::vec2,
                                            glm::vec3, glm::vec4, glm::mat4>;

// Abstract interface for a bound material instance ready for rendering.
// Encapsulates pipeline selection, bind group creation, and parameter management.
class RenderingMaterialInstance {
 public:
  virtual ~RenderingMaterialInstance() = default;

  // Bind pipeline + group 0 (frame UBO + sampler + material textures).
  // Called once per material change (not per draw call).
  virtual bool Bind(RenderPassContext& pass, FrameContext& frame) = 0;

  // Upload per-object UBO + bind group 1. Called per draw call after
  // SetParameter.
  virtual bool BindPerObject(RenderPassContext& pass, FrameContext& frame) = 0;

  // Parameter API
  virtual MaterialParameterId GetParameterId(
      const std::string& name) const = 0;
  virtual void SetParameter(MaterialParameterId id,
                            const MaterialParameterValue& value) = 0;
  void SetParameterByName(const std::string& name,
                          const MaterialParameterValue& value);

  // Wireframe toggle — switches pipeline variant internally
  virtual void SetWireframe(bool enabled) = 0;

  // Runtime queries
  virtual GeometryType GetGeometryType() const = 0;
  virtual RenderPassType GetRenderPassType() const = 0;
  virtual wgpu::RenderPipeline GetPipeline() const = 0;
  virtual bool IsValid() const = 0;
};

}  // namespace badlands
