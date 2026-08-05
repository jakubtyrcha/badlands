#pragma once

// Maps a USD material onto a material pack directory on disk.
//
// This exists because the two do not agree and cannot be made to. The shipped
// props reference textures the download does not contain -- their USD asks for
// `<name>_nor_gl_1k.exr` and `<name>_rough_1k.exr` while Poly Haven ships
// `<name>_nor_dx_1k.jpg` and `<name>_arm_1k.jpg` -- so a prop's real material
// is authored as a `material.json` pack next to the model and the USD's own
// texture paths are never loaded (they survive only as diagnostics on
// UsdMaterialData).
//
// What still has to be decided is WHICH pack each mesh uses, and that is what
// this maps. Every shipped prop has exactly one material, so `default_pack_dir`
// answers it alone; `by_material` is the escape hatch for a model whose prims
// do not all share one.
//
// Pure: no filesystem access, no tinyusdz, no Dawn. Testable with fabricated
// strings and nothing else.

#include <string>
#include <string_view>
#include <unordered_map>

namespace badlands {

struct UsdMaterialBinding {
  // The pack directory used by any mesh with no entry in `by_material` --
  // normally the model's own directory, e.g. "assets/models/wooden_crate_01_1k".
  std::string default_pack_dir;

  // Overrides keyed by UsdMeshData::material_name. Empty for every prop shipped
  // today; populated when one model needs more than one pack.
  std::unordered_map<std::string, std::string> by_material;
};

// The pack directory `material_name` resolves to: its override if one exists,
// otherwise `binding.default_pack_dir`.
//
// An empty `material_name` (a mesh with no bound USD material) takes the
// default, since "unbound" is not a name an override can sensibly key on.
std::string ResolvePackDir(std::string_view material_name,
                           const UsdMaterialBinding& binding);

}  // namespace badlands
