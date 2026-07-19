#pragma once

// Ported from sampo's src/rendering/material/material_requirements.{hpp,cpp},
// namespace sampo -> badlands (verbatim otherwise).

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/geometry_type.hpp"

namespace badlands {

// Forward declaration
struct ReflectedBinding;

// Texture type for material bindings
enum class TextureType { k2D, kCubemap, kArray };

// Single texture requirement for a material
struct TextureRequirement {
  std::string slot_name;       // "albedo", "height", "normal", "roughness"
  uint32_t texture_binding;    // Shader binding index for texture
  uint32_t sampler_binding;    // Shader binding index for sampler (usually 2)
  std::string default_texture;  // "white", "gray", "flat_normal", "full_roughness"

  // Optional: force specific texture type regardless of geometry
  // None = follow geometry (kSphericalMesh → cubemap, kTexturedMesh → 2D)
  std::optional<TextureType> forced_type;
};

// Requirements for a material with a specific geometry type
struct MaterialRequirements {
  std::string shader_name;
  std::vector<TextureRequirement> textures;
};

// Registry of material texture requirements
// Provides data-driven texture binding configuration per material/geometry combination
class MaterialRequirementsRegistry {
 public:
  MaterialRequirementsRegistry();

  // Get requirements for a material with specific geometry type
  // Returns empty requirements if material not registered
  MaterialRequirements GetRequirements(const std::string& material_name,
                                        GeometryType geometry_type) const;

  // Register a material with requirements for both geometry types
  void RegisterMaterial(const std::string& name,
                        const MaterialRequirements& flat_2d,
                        const MaterialRequirements& sphere_mode);

  // Register an alias for a material (e.g., inline materials use shader name)
  // alias_name will resolve to the same requirements as canonical_name
  void RegisterAlias(const std::string& alias_name,
                     const std::string& canonical_name);

  // Check if a material is registered
  bool HasMaterial(const std::string& name) const;

 private:
  // Key: material_name + geometry_type suffix
  std::unordered_map<std::string, MaterialRequirements> requirements_;

  // Alias mapping: alias_name -> canonical_name
  std::unordered_map<std::string, std::string> aliases_;

  // Resolve a name through aliases
  std::string ResolveName(const std::string& name) const;

  static std::string MakeKey(const std::string& name, GeometryType geo);
};

// Auto-derive texture requirements from shader reflection.
// Scans group 0 bindings for texture+sampler pairs.
MaterialRequirements DeriveRequirementsFromReflection(
    const std::string& shader_name,
    const std::vector<ReflectedBinding>& bindings);

// Determine texture type from requirement and geometry
inline TextureType DetermineTextureType(const TextureRequirement& req,
                                        GeometryType geometry_type) {
  // Material can force a specific type
  if (req.forced_type.has_value()) {
    return *req.forced_type;
  }

  // Default: geometry drives type
  return (geometry_type == GeometryType::kSphericalMesh) ? TextureType::kCubemap
                                                          : TextureType::k2D;
}

}  // namespace badlands
