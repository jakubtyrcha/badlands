#include "engine/assets/usd_material_binding.hpp"

namespace badlands {

std::string ResolvePackDir(std::string_view material_name,
                           const UsdMaterialBinding& binding) {
  if (material_name.empty()) return binding.default_pack_dir;

  // std::unordered_map<std::string, ...> has no transparent hash, so the
  // string_view has to be materialised to look it up. Fine here: this runs once
  // per mesh at load, not per frame.
  const auto it = binding.by_material.find(std::string(material_name));
  if (it != binding.by_material.end()) return it->second;

  return binding.default_pack_dir;
}

}  // namespace badlands
