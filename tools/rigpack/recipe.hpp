#pragma once

// The pack recipe: which clips of an imported family ship, and what badlands
// calls them.
//
// This is the ONLY place 0 A.D.'s animation vocabulary meets badlands'. The
// corpus is 1,220 clips with names like "Attack_melee"; a game asks for
// "attack". Keeping that mapping as checked-in data means the shipped rig is
// reproducible from the repo and reviewable in a diff, rather than living in
// whoever-ran-the-tool's shell history.
//
//   assets/characters/0ad_biped/pack.json
//   { "family": "Biped",
//     "yaw_offset_degrees": 0,
//     "clips": { "idle":   "biped/infantry/idle_relax_01",
//                "attack": "biped/infantry/swordsman/attack_melee_shield_01" } }
//
// The packed rig is written BESIDE the recipe, so the recipe's own location is
// the output directory and no path in the file is machine-specific.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace badlands::rigpack {

struct Recipe {
  // Exactly one of these is set. `family` packs one rig BESIDE the recipe;
  // `families` packs several, one per subdirectory named for its slug:
  //
  //   { "families": ["Biped", "Horse", "Wolf"], "clips": "*" }
  //   -> <recipe dir>/biped/  <recipe dir>/horse/  <recipe dir>/wolf/
  //
  // One recipe rather than one per family, because 31 near-identical files
  // would be boilerplate -- but still a CHECKED-IN LIST, so every shipped rig
  // is named somewhere a reviewer can see.
  std::string family;
  std::vector<std::string> families;

  float yaw_offset_degrees = 0.0f;

  // `"clips": "*"` — take EVERY clip the family carries, each keeping the
  // intermediate's own name. For a rig meant to be browsed rather than played
  // by the game: `Biped` is 651 clips, and picking them one at a time is not how
  // you find out what is in there. `clips` is empty when this is set.
  bool all_clips = false;

  // logical badlands name -> intermediate clip key, in authored order (which
  // becomes the manifest's clip order, and so the viewer's list order).
  std::vector<std::pair<std::string, std::string>> clips;

  std::filesystem::path out_dir;  // the recipe's own directory
};

// nullopt on any structural problem, with the reason in `error`.
std::optional<Recipe> LoadRecipe(const std::filesystem::path& path,
                                 std::string* error);

}  // namespace badlands::rigpack
