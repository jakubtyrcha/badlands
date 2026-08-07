#pragma once

// Reader for the ANIMATION IMPORT INTERMEDIATE — the tree `animexport` writes
// and rigpack consumes:
//
//   <root>/manifest.json              index of families -> rig.json paths
//   <root>/<family-slug>/rig.json     joints, sockets, clip table
//   <root>/<family-slug>/clips/*.bin  float32[frames][joints][16]
//
// The canonical format reference is `source/tools/animexport/README.md` on the
// `export/badlands-anim` branch of the 0ad repo. THIS IS THE ONE FILE A SCHEMA
// CHANGE TOUCHES — everything downstream works in the structs below. Keep it
// that way.
//
// The intermediate is NOT checked into this repo: it is 212 MB of derived data
// regenerated from the 0ad checkout and named on rigpack's command line.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>

namespace badlands::rigpack {

// How a 4x4 is laid out in 16 floats. The intermediate is ALWAYS column-major --
// "the writer transposes, the reader must not" -- so this is not a setting. It
// exists only so the packer can VERIFY that, since a silently transposed rig
// loads and animates and is wrong everywhere.
enum class MatrixOrder {
  kRowMajor,     // COLLADA's own order
  kColumnMajor,  // what the intermediate actually carries
};

struct IntermediateJoint {
  std::string name;
  int parent = -1;  // index into joints; -1 only for joint 0 (`__root__`)
};

struct IntermediateSocket {
  std::string name;        // already stripped of its prop- / prop_ prefix
  int parent = -1;         // index into joints
  glm::mat4 offset{1.0f};  // parent-local
  float drift = 0.0f;      // how far it moves over a clip; ~0 for a real socket
};

struct IntermediateClip {
  std::string name;    // the source path flattened: "biped__infantry__capturing_a"
  std::string source;  // provenance: the .dae it came from (CC-BY-SA)
  std::string data;    // .bin path, relative to the family directory
  int frames = 0;

  // FRAME TIMES ARE AUTHORITATIVE, and `frame_rate` deliberately is not carried.
  // 545 clips are 24 fps, 449 are 30, and 135 are not evenly sampled at all --
  // wolf_idle_01 holds a pose for 5.1 s between frames 0.067 s apart. Assuming a
  // constant rate is the single most likely way to get this wrong.
  std::vector<float> frame_times;  // seconds, one per frame
  float duration = 0.0f;
  bool uniform = true;

  std::vector<std::pair<std::string, float>> markers;  // normalized [0,1]
  // One clip serves several states, and 0 A.D. is inconsistent about case, so
  // this carries both "attack_melee" and "Attack_melee". A recipe may name a
  // clip by any of these or by `name`.
  std::vector<std::string> logical_names;

  // Net displacement the exporter REMOVED from the payload. Non-zero on 4 clips
  // corpus-wide, all animal deaths. Reported, not restored.
  float root_motion = 0.0f;
};

struct Intermediate {
  std::string family;
  std::filesystem::path dir;  // where `data` resolves from
  std::vector<IntermediateJoint> joints;
  std::vector<IntermediateSocket> sockets;
  std::vector<IntermediateClip> clips;

  // The clip a recipe key names: by `name` first, then by any of its
  // `logical_names` case-insensitively. nullptr when nothing matches.
  //
  // `matches`, when given, receives how many clips the name matched. LOGICAL
  // NAMES ARE NOT UNIQUE and are not close to it -- on Biped, `idle` matches 240
  // clips, `walk` 117 and `attack_melee` 89, because 0 A.D. picks among variants
  // at random by weight. This returns the first, in file order, and the count is
  // how a caller can say so out loud instead of shipping an arbitrary one.
  // A match on `name` sets it to 1: those are unique by construction.
  const IntermediateClip* FindClip(const std::string& name,
                                   int* matches = nullptr) const;
};

// One family as <root>/manifest.json indexes it.
struct FamilyEntry {
  std::string family;          // authored name, e.g. "Biped" or "Ship Row"
  std::string slug;            // its directory, e.g. "biped" -- also the rig's
  std::filesystem::path rig;   // absolute path to its rig.json
};

// Every family the intermediate carries, in manifest order. nullopt when there
// is no manifest to read -- a single-family extract need not leave one.
std::optional<std::vector<FamilyEntry>> LoadManifest(
    const std::filesystem::path& root, std::string* error);

// Resolves `family` through <root>/manifest.json and reads its rig.json. Falls
// back to <root>/<family>/rig.json when there is no manifest, which is what a
// single-family `--family NAME` extract can leave behind.
//
// nullopt on any structural problem, with the reason in `error` — a malformed
// intermediate is a content error, so it is reported rather than asserted.
std::optional<Intermediate> LoadIntermediate(const std::filesystem::path& root,
                                             const std::string& family,
                                             std::string* error);

// Reads one clip's .bin as [frame][joint] matrices. nullopt when the file is
// missing or its size is not exactly frames * joints * 16 floats.
//
// `detected`, when given, receives the order the BYTES appear to be in. The
// caller compares it against column-major; a disagreement means the writer's
// convention changed under us.
std::optional<std::vector<std::vector<glm::mat4>>> LoadClipMatrices(
    const Intermediate& intermediate, const IntermediateClip& clip,
    std::string* error, std::optional<MatrixOrder>* detected = nullptr);

// Which order 16 floats appear to be in, judged by where the affine row/column
// (0,0,0,1) sits. nullopt when both readings agree (a zero translation), which
// is ambiguous rather than an answer.
std::optional<MatrixOrder> DetectMatrixOrder(const float* floats);

}  // namespace badlands::rigpack
