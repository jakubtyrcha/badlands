#pragma once

// The conversion: import intermediate + recipe -> a runtime rig asset.
//
//   <out>/skeleton.ozz          the joint hierarchy
//   <out>/clips/<logical>.ozz   one per recipe clip
//   <out>/rig.json              the manifest AnimationSet loads
//
// Everything the run should be judged on comes back in a PackReport rather than
// being logged, so the caller decides how to present it and the tests can assert
// on it. Nothing here writes to a log.

#include <filesystem>
#include <string>
#include <vector>

#include "rigpack/recipe.hpp"

namespace badlands::rigpack {

struct ClipReport {
  std::string logical;
  std::string source_key;
  int frames = 0;
  float duration_seconds = 0.0f;
  bool uniform = true;  // false when the source frames are unevenly spaced

  // How far the ROOT joint's origin travels IN THE PAYLOAD. The exporter strips
  // root motion, so this should be ~0 -- it is the check that it did, rather
  // than the assumption. The sim owns movement, and a clip that also moves its
  // root moves a character twice.
  float root_translation = 0.0f;
  // What the exporter says it REMOVED. Non-zero on four clips corpus-wide, all
  // animal deaths. Informational: rigpack does not put it back.
  float root_motion_declared = 0.0f;
};

struct PackReport {
  bool ok = false;
  std::string error;  // why, when ok is false

  std::string family;
  std::filesystem::path out_dir;
  int joints = 0;
  int sockets_kept = 0;

  // The largest `drift` among the sockets that survived. A socket is baked to
  // ONE offset, so this should be ~0: the exporter promotes anything that
  // measurably moves into a real joint. A number here means that stopped
  // holding, and those sockets are frozen where they should not be.
  float max_socket_drift = 0.0f;

  // The intermediate carries no bind pose, so the skeleton's rest transforms
  // come from frame 0 of this clip. Empty when no clip could be read and every
  // joint fell back to identity.
  std::string rest_pose_from;

  // Sockets dropped because their name is also a joint's. Expected and correct
  // -- `weapon_R` and `prop-weapon_R` are duplicates -- but listed so the
  // collapse stays reviewable instead of becoming folklore.
  std::vector<std::string> dropped_collisions;

  // Everything else worth a human's attention: a recipe clip the intermediate
  // does not carry, a matrix that would not decompose, a declared matrix order
  // that disagrees with the data.
  std::vector<std::string> warnings;

  std::vector<ClipReport> clips;
};

// Reads <intermediate_root>/<recipe.family>/ and writes the packed rig beside
// the recipe. Never throws; every failure comes back in the report.
PackReport Pack(const Recipe& recipe,
                const std::filesystem::path& intermediate_root);

// A `families` recipe: one rig per named family, each into its own
// <recipe.out_dir>/<slug>/. Returns one report per family, in recipe order.
//
// A family that fails does NOT stop the rest -- its report simply carries the
// error. Packing 31 rigs and losing all of them because the 9th is malformed
// would be the wrong trade for an offline tool whose output a human reads.
//
// `families_override`, when non-empty, replaces the recipe's list; that is how
// --all packs everything the manifest carries without a recipe to declare it.
std::vector<PackReport> PackFamilies(
    const Recipe& recipe, const std::filesystem::path& intermediate_root,
    const std::vector<std::string>& families_override = {});

}  // namespace badlands::rigpack
