// Unit tests for the USD material -> pack directory remap
// (src/engine/assets/usd_material_binding.hpp).
//
// Entirely fabricated inputs: no .usdc is opened, no directory is touched, no
// device exists. That is the point -- the remap is the piece that has to be
// correct when a model's internals and the assets on disk disagree, so it is
// pinned independently of whether any particular asset happens to parse.

#include <catch_amalgamated.hpp>

#include "engine/assets/usd_material_binding.hpp"

using badlands::ResolvePackDir;
using badlands::UsdMaterialBinding;

TEST_CASE("an unmapped material falls back to the default pack", "[usd]") {
  UsdMaterialBinding binding;
  binding.default_pack_dir = "assets/models/wooden_crate_01_1k";

  CHECK(ResolvePackDir("wooden_crate_01", binding) ==
        "assets/models/wooden_crate_01_1k");
}

TEST_CASE("an override wins over the default", "[usd]") {
  UsdMaterialBinding binding;
  binding.default_pack_dir = "assets/models/rock_moss_set_01_1k";
  binding.by_material["moss"] = "assets/materials/mossy_rock_1k";

  CHECK(ResolvePackDir("moss", binding) == "assets/materials/mossy_rock_1k");
  // A sibling material in the same model still takes the default -- overriding
  // one prim must not redirect the rest.
  CHECK(ResolvePackDir("rock_moss_set_01", binding) ==
        "assets/models/rock_moss_set_01_1k");
}

TEST_CASE("a mesh with no bound material takes the default", "[usd]") {
  UsdMaterialBinding binding;
  binding.default_pack_dir = "assets/models/boulder_01_1k";
  binding.by_material[""] = "assets/materials/should_not_be_used";

  // "" means unbound, not a material literally named "". An override keyed on
  // the empty string must not hijack every unbound mesh.
  CHECK(ResolvePackDir("", binding) == "assets/models/boulder_01_1k");
}

TEST_CASE("lookup is exact, not prefix or case insensitive", "[usd]") {
  UsdMaterialBinding binding;
  binding.default_pack_dir = "assets/models/treasure_chest_1k";
  binding.by_material["treasure_chest"] = "assets/materials/chest_override";

  CHECK(ResolvePackDir("treasure_chest", binding) ==
        "assets/materials/chest_override");
  CHECK(ResolvePackDir("Treasure_Chest", binding) ==
        "assets/models/treasure_chest_1k");
  CHECK(ResolvePackDir("treasure_chest_lid", binding) ==
        "assets/models/treasure_chest_1k");
}

TEST_CASE("an empty binding resolves everything to an empty pack", "[usd]") {
  // Not an error case to guard against here -- the caller (the adapter) is what
  // decides an empty pack dir is unusable. This just pins that the remap does
  // not invent a path of its own.
  CHECK(ResolvePackDir("anything", UsdMaterialBinding{}).empty());
}
