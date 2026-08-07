#include "rigpack/pack.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>

#include "rigpack/intermediate.hpp"

namespace badlands::rigpack {
namespace {

ozz::math::Float4x4 ToOzz(const glm::mat4& m) {
  ozz::math::Float4x4 out;
  for (int c = 0; c < 4; ++c) {
    out.cols[c] = ozz::math::simd_float4::Load(m[c][0], m[c][1], m[c][2], m[c][3]);
  }
  return out;
}

// 4x4 -> translation/rotation/scale, which is the only form ozz's runtime
// stores. Fails on a matrix that is not decomposable (shear, or a degenerate
// basis); the caller reports it rather than substituting something plausible.
bool Decompose(const glm::mat4& m, ozz::math::Transform* out) {
  return ozz::math::ToAffine(ToOzz(m), out);
}

// Builds the ozz joint tree. The reader guarantees parents precede children, so
// this recursion terminates on well-formed input.
void FillJoint(ozz::animation::offline::RawSkeleton::Joint& out, int index,
               const std::vector<IntermediateJoint>& joints,
               const std::vector<glm::mat4>& rest,
               const std::vector<std::vector<int>>& children,
               std::vector<std::string>* warnings) {
  out.name = joints[static_cast<size_t>(index)].name.c_str();
  out.transform = ozz::math::Transform::identity();
  if (!Decompose(rest[static_cast<size_t>(index)], &out.transform)) {
    warnings->push_back("joint \"" + joints[static_cast<size_t>(index)].name +
                        "\" has a rest matrix that will not decompose; using identity");
    out.transform = ozz::math::Transform::identity();
  }

  const std::vector<int>& kids = children[static_cast<size_t>(index)];
  out.children.resize(kids.size());
  for (size_t k = 0; k < kids.size(); ++k) {
    FillJoint(out.children[k], kids[k], joints, rest, children, warnings);
  }
}

template <typename T>
bool WriteOzz(const T& object, const std::filesystem::path& path) {
  ozz::io::File file(path.string().c_str(), "wb");
  if (!file.opened()) return false;
  ozz::io::OArchive archive(&file);
  archive << object;
  return true;
}

// glm::value_ptr order — column-major, which is what AnimationSet reads back.
nlohmann::ordered_json Mat4Json(const glm::mat4& m) {
  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  for (int i = 0; i < 16; ++i) out.push_back(m[i / 4][i % 4]);
  return out;
}

}  // namespace

std::vector<PackReport> PackFamilies(
    const Recipe& recipe, const std::filesystem::path& intermediate_root,
    const std::vector<std::string>& families_override) {
  std::vector<PackReport> reports;

  std::string error;
  std::optional<std::vector<FamilyEntry>> manifest =
      LoadManifest(intermediate_root, &error);
  if (!manifest) {
    PackReport failed;
    failed.error = error;
    reports.push_back(failed);
    return reports;
  }

  const std::vector<std::string>& wanted =
      families_override.empty() ? recipe.families : families_override;

  for (const std::string& family : wanted) {
    // The output directory is the intermediate's OWN slug, not something
    // derived from the name -- that is what keeps `Main` and `main` in separate
    // directories, as the exporter already arranged.
    const auto entry = std::find_if(
        manifest->begin(), manifest->end(),
        [&](const FamilyEntry& e) { return e.family == family; });

    PackReport report;
    if (entry == manifest->end()) {
      report.family = family;
      report.error = "no family named \"" + family + "\" in the manifest";
      reports.push_back(std::move(report));
      continue;
    }

    Recipe one = recipe;
    one.families.clear();
    one.family = family;
    one.out_dir = recipe.out_dir / entry->slug;
    reports.push_back(Pack(one, intermediate_root));
  }
  return reports;
}

PackReport Pack(const Recipe& recipe,
                const std::filesystem::path& intermediate_root) {
  PackReport report;
  report.family = recipe.family;
  report.out_dir = recipe.out_dir;

  std::string error;
  std::optional<Intermediate> intermediate =
      LoadIntermediate(intermediate_root, recipe.family, &error);
  if (!intermediate) {
    report.error = error;
    return report;
  }
  report.joints = static_cast<int>(intermediate->joints.size());

  // What to pack, as (logical name -> intermediate clip key). A "*" recipe takes
  // the whole family with each clip keeping its own name; otherwise the recipe
  // says, in its own order, which becomes the manifest's clip order.
  std::vector<std::pair<std::string, std::string>> selection = recipe.clips;
  if (recipe.all_clips) {
    selection.clear();
    selection.reserve(intermediate->clips.size());
    for (const IntermediateClip& clip : intermediate->clips) {
      selection.emplace_back(clip.name, clip.name);
    }
  }

  // A logical name becomes both a FILENAME and a manifest key, and with "*" it
  // comes from another repo's data. Three ways that goes wrong, all silent:
  std::unordered_set<std::string> lowered_names;
  for (const auto& [logical, source_key] : selection) {
    // ...it escapes the output directory,
    if (logical.find('/') != std::string::npos ||
        logical.find('\\') != std::string::npos || logical.find("..") != std::string::npos) {
      report.error = "clip name \"" + logical +
                     "\" contains a path separator and cannot become a filename";
      return report;
    }
    // ...it is read back as one of the manifest's `_`-prefixed COMMENT keys, so
    // the clip packs, counts, and is then invisible to AnimationSet::Load,
    if (!logical.empty() && logical.front() == '_') {
      report.error = "clip name \"" + logical +
                     "\" starts with '_', which a manifest reads as a comment key";
      return report;
    }
    // ...or two names differ only in case, which is two manifest entries and
    // ONE file on a case-insensitive filesystem (the macOS default), so both
    // clips would resolve to whichever was written last.
    std::string lowered = logical;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!lowered_names.insert(lowered).second) {
      report.error = "clip names \"" + logical +
                     "\" and another differ only in case, which collides on a "
                     "case-insensitive filesystem";
      return report;
    }
  }

  // ---------------------------------------------------------------------------
  // Skeleton.
  // ---------------------------------------------------------------------------
  std::vector<std::vector<int>> children(intermediate->joints.size());
  std::vector<int> roots;
  for (size_t i = 0; i < intermediate->joints.size(); ++i) {
    const int parent = intermediate->joints[i].parent;
    if (parent < 0) {
      roots.push_back(static_cast<int>(i));
    } else {
      children[static_cast<size_t>(parent)].push_back(static_cast<int>(i));
    }
  }
  if (roots.empty()) {
    report.error = "the intermediate declares no root joint";
    return report;
  }

  // The intermediate carries NO bind pose -- every clip is written at the
  // family's full joint width, so the exporter never needs one. Frame 0 of the
  // first readable clip stands in, which is what keeps a freshly constructed
  // Pose looking like the rig instead of collapsing every joint onto the origin.
  std::vector<glm::mat4> rest(intermediate->joints.size(), glm::mat4(1.0f));
  for (const auto& [logical, source_key] : selection) {
    const IntermediateClip* clip = intermediate->FindClip(source_key);
    if (clip == nullptr) continue;
    std::optional<std::vector<std::vector<glm::mat4>>> frames =
        LoadClipMatrices(*intermediate, *clip, &error);
    if (!frames || frames->empty()) continue;
    rest = (*frames)[0];
    report.rest_pose_from = clip->name;
    break;
  }
  if (report.rest_pose_from.empty()) {
    report.warnings.push_back(
        "no clip could be read for a rest pose; every joint falls back to identity");
  }

  ozz::animation::offline::RawSkeleton raw_skeleton;
  raw_skeleton.roots.resize(roots.size());
  for (size_t r = 0; r < roots.size(); ++r) {
    FillJoint(raw_skeleton.roots[r], roots[r], intermediate->joints, rest, children,
              &report.warnings);
  }
  if (!raw_skeleton.Validate()) {
    report.error = "the joint hierarchy is not a valid skeleton";
    return report;
  }

  ozz::animation::offline::SkeletonBuilder skeleton_builder;
  ozz::unique_ptr<ozz::animation::Skeleton> skeleton = skeleton_builder(raw_skeleton);
  if (!skeleton) {
    report.error = "SkeletonBuilder rejected the joint hierarchy";
    return report;
  }

  // THE REINDEXING TRAP. SkeletonBuilder does NOT preserve the intermediate's
  // joint order, so an index carried across this boundary points at the wrong
  // joint -- and the result is a rig that animates, plausibly, incorrectly.
  // Everything downstream resolves by NAME through this map.
  std::unordered_map<std::string, int> ozz_index;
  const ozz::span<const char* const> ozz_names = skeleton->joint_names();
  for (size_t i = 0; i < ozz_names.size(); ++i) {
    ozz_index.emplace(ozz_names[i], static_cast<int>(i));
  }

  // Clear any previous clips/ before writing. Without this a clip dropped from a
  // recipe stays on disk forever -- and for the shipped rigs, in git LFS -- with
  // nothing referencing it. Only reached once the intermediate has parsed and
  // the skeleton has built, so a malformed input never destroys a good pack.
  std::error_code ec;
  const std::filesystem::path clips_dir = recipe.out_dir / "clips";
  std::filesystem::remove_all(clips_dir, ec);
  std::filesystem::create_directories(clips_dir, ec);
  if (ec) {
    report.error = "cannot create " + clips_dir.string() + ": " + ec.message();
    return report;
  }

  // ---------------------------------------------------------------------------
  // Clips.
  // ---------------------------------------------------------------------------
  nlohmann::ordered_json clips_json = nlohmann::ordered_json::object();

  for (const auto& [logical, source_key] : selection) {
    int matches = 0;
    const IntermediateClip* clip = intermediate->FindClip(source_key, &matches);
    if (clip == nullptr) {
      // One missing clip costs one animation, not the rig -- the same policy
      // AnimationSet::Load applies to an unreadable clip file.
      report.warnings.push_back("recipe clip \"" + logical +
                                "\" names \"" + source_key +
                                "\", which the intermediate does not carry; skipped");
      continue;
    }
    // A logical name is a VARIANT CLASS, not a clip: `idle` names 240 clips on
    // Biped, and 0 A.D. picks among them at random by weight. Taking the first
    // is defensible for a prototype and indefensible if nobody is told, so say
    // which one and how many were passed over.
    if (matches > 1) {
      report.warnings.push_back(
          "recipe clip \"" + logical + "\" matched " + std::to_string(matches) +
          " clips by logical name \"" + source_key + "\"; packed \"" + clip->name +
          "\". Name a clip directly to choose deliberately.");
    }

    std::optional<MatrixOrder> detected;
    std::optional<std::vector<std::vector<glm::mat4>>> frames =
        LoadClipMatrices(*intermediate, *clip, &error, &detected);
    if (!frames) {
      report.warnings.push_back("clip \"" + logical + "\": " + error + "; skipped");
      continue;
    }
    // The intermediate is column-major by contract -- "the writer transposes,
    // the reader must not". If the bytes ever stop looking that way, every
    // matrix in the rig is transposed and the result loads, animates, and is
    // wrong everywhere. Say so here rather than let it surface as a mangled
    // character much later.
    if (detected.has_value() && *detected != MatrixOrder::kColumnMajor) {
      report.warnings.push_back(
          "clip \"" + logical +
          "\": the data looks row-major, but the intermediate is column-major by "
          "contract; every matrix is likely transposed");
    }

    ozz::animation::offline::RawAnimation raw;
    raw.name = logical.c_str();
    // FRAME TIMES, not a rate. 135 clips are unevenly sampled -- wolf_idle_01
    // holds a pose for 5.1 s between frames 0.067 s apart -- so a constant-rate
    // assumption would silently redistribute their motion. ozz takes explicit
    // key times, so the source data suits it directly.
    //
    // A duration must be positive and cover the last key, or ozz rejects the
    // clip; a single-frame clip has neither by itself.
    // The MAX time, not the last one: with out-of-order frames the last is not
    // the largest, and a duration short of a key would see that key clamped away
    // rather than kept.
    const float last_time =
        *std::max_element(clip->frame_times.begin(), clip->frame_times.end());
    raw.duration = std::max({clip->duration, last_time, 1.0f / 60.0f});
    raw.tracks.resize(static_cast<size_t>(skeleton->num_joints()));

    // ozz needs key times INSIDE [0, duration], and the corpus does not always
    // oblige: quadraped__rabbit_walk starts at -0.0417 s. Clamp rather than
    // reject -- a frame a hair before zero is a sampler artefact, not content.
    std::vector<float> times;
    times.reserve(static_cast<size_t>(clip->frames));
    int clamped = 0;
    for (const float t : clip->frame_times) {
      const float in_range = std::clamp(t, 0.0f, raw.duration);
      if (in_range != t) ++clamped;
      times.push_back(in_range);
    }
    if (clamped > 0) {
      report.warnings.push_back(
          "clip \"" + logical + "\": " + std::to_string(clamped) +
          " frame times fall outside [0, " + std::to_string(raw.duration) +
          "] and were clamped into range");
    }

    // Then STRICTLY increasing, on the clamped times: biped__new__boat_fisherman_idle
    // authors three pairs of frames at the same instant, and clamping can create
    // such a pair too.
    //
    // Walked BACKWARDS, keeping a frame only when its time is strictly below the
    // last one kept. That does two things at once: it keeps the LAST frame of an
    // equal-time run (the pose that holds from that instant onward), and it
    // GUARANTEES the result is sorted. Comparing each frame only against its
    // neighbour does not -- times [0.5, 0.6, 0.1, 0.2] would survive that test
    // as [0.5, 0.1, 0.2], still unsorted, and ozz would then reject the clip
    // with a message about validation rather than about time order.
    std::vector<int> keys;
    keys.reserve(static_cast<size_t>(clip->frames));
    for (int f = clip->frames - 1; f >= 0; --f) {
      if (keys.empty() ||
          times[static_cast<size_t>(f)] < times[static_cast<size_t>(keys.back())]) {
        keys.push_back(f);
      }
    }
    std::reverse(keys.begin(), keys.end());
    if (keys.size() != static_cast<size_t>(clip->frames)) {
      report.warnings.push_back(
          "clip \"" + logical + "\": " +
          std::to_string(static_cast<size_t>(clip->frames) - keys.size()) +
          " frames dropped to make key times strictly increasing (equal or "
          "out-of-order timestamps); ozz requires it");
    }

    int decompose_failures = 0;
    for (size_t j = 0; j < intermediate->joints.size(); ++j) {
      const auto track_it = ozz_index.find(intermediate->joints[j].name);
      if (track_it == ozz_index.end()) continue;  // unreachable for a built skeleton
      auto& track = raw.tracks[static_cast<size_t>(track_it->second)];

      for (const int f : keys) {
        ozz::math::Transform transform = ozz::math::Transform::identity();
        if (!Decompose((*frames)[static_cast<size_t>(f)][j], &transform)) {
          ++decompose_failures;
          transform = ozz::math::Transform::identity();
        }
        const float time = times[static_cast<size_t>(f)];
        track.translations.push_back({time, transform.translation});
        track.rotations.push_back({time, transform.rotation});
        track.scales.push_back({time, transform.scale});
      }
    }
    if (decompose_failures > 0) {
      report.warnings.push_back("clip \"" + logical + "\": " +
                                std::to_string(decompose_failures) +
                                " matrices would not decompose (sheared or "
                                "degenerate); those frames use identity");
    }

    if (!raw.Validate()) {
      report.warnings.push_back("clip \"" + logical +
                                "\" did not validate as a raw animation; skipped");
      continue;
    }

    ozz::animation::offline::AnimationBuilder animation_builder;
    ozz::unique_ptr<ozz::animation::Animation> animation = animation_builder(raw);
    if (!animation) {
      report.warnings.push_back("AnimationBuilder rejected clip \"" + logical +
                                "\"; skipped");
      continue;
    }

    const std::filesystem::path clip_path =
        recipe.out_dir / "clips" / (logical + ".ozz");
    if (!WriteOzz(*animation, clip_path)) {
      report.warnings.push_back("cannot write " + clip_path.string() + "; skipped");
      continue;
    }

    // Root motion check: how far the first root joint's origin travels IN THE
    // PAYLOAD. The exporter strips this, so anything but ~0 means it did not.
    ClipReport clip_report;
    clip_report.logical = logical;
    clip_report.source_key = source_key;
    clip_report.frames = clip->frames;
    clip_report.duration_seconds = raw.duration;
    clip_report.uniform = clip->uniform;
    clip_report.root_motion_declared = clip->root_motion;
    const size_t root = static_cast<size_t>(roots[0]);
    const glm::vec3 origin((*frames)[0][root][3]);
    for (const std::vector<glm::mat4>& frame : *frames) {
      clip_report.root_translation =
          std::max(clip_report.root_translation,
                   glm::length(glm::vec3(frame[root][3]) - origin));
    }
    report.clips.push_back(clip_report);

    nlohmann::ordered_json clip_json = nlohmann::ordered_json::object();
    clip_json["file"] = "clips/" + logical + ".ozz";
    // Provenance survives the conversion: the art is CC-BY-SA 3.0, and this is
    // what ties a packed clip back to the file it came from.
    if (!clip->source.empty()) clip_json["source"] = clip->source;
    if (!clip->markers.empty()) {
      nlohmann::ordered_json markers = nlohmann::ordered_json::object();
      for (const auto& [name, ratio] : clip->markers) markers[name] = ratio;
      clip_json["markers"] = markers;
    }
    clips_json[logical] = clip_json;
  }

  if (report.clips.empty()) {
    report.error = "no clips packed";
    return report;
  }

  // The skeleton is written only once at least one clip has, so a failed run
  // does not leave a directory holding a skeleton.ozz and no rig.json -- which
  // in an --all run would sit among 30 good ones and only fail at load.
  if (!WriteOzz(*skeleton, recipe.out_dir / "skeleton.ozz")) {
    report.error = "cannot write " + (recipe.out_dir / "skeleton.ozz").string();
    return report;
  }

  // ---------------------------------------------------------------------------
  // Sockets: the collision collapse happens HERE, so the shipped manifest is
  // already collision-free and the runtime never has to arbitrate.
  // ---------------------------------------------------------------------------
  std::unordered_set<std::string> taken;
  for (const IntermediateJoint& joint : intermediate->joints) taken.insert(joint.name);

  nlohmann::ordered_json sockets_json = nlohmann::ordered_json::array();
  for (const IntermediateSocket& socket : intermediate->sockets) {
    if (!taken.insert(socket.name).second) {
      // A joint of the same name, or a second socket claiming it. Either way the
      // first one wins and this is a duplicate.
      report.dropped_collisions.push_back(socket.name);
      continue;
    }
    // A socket is frozen to a single offset, so a surviving one had better be
    // rigid. The exporter promotes anything that measurably moves into a real
    // joint, which is why every survivor should read ~0 here -- this is the
    // check on that, not a restatement of it.
    report.max_socket_drift = std::max(report.max_socket_drift, socket.drift);

    nlohmann::ordered_json socket_json = nlohmann::ordered_json::object();
    socket_json["name"] = socket.name;
    // The manifest stores the parent's NAME, not its index, precisely because
    // the index the intermediate used is not the one the skeleton ended up with.
    socket_json["parent"] = intermediate->joints[static_cast<size_t>(socket.parent)].name;
    socket_json["offset"] = Mat4Json(socket.offset);
    sockets_json.push_back(socket_json);
    ++report.sockets_kept;
  }

  // ---------------------------------------------------------------------------
  // Manifest.
  // ---------------------------------------------------------------------------
  nlohmann::ordered_json manifest = nlohmann::ordered_json::object();
  manifest["_comment"] = nlohmann::ordered_json::array(
      {"Generated by tools/rigpack -- do not hand-edit; edit pack.json and repack.",
       "Source art is CC-BY-SA 3.0 (0 A.D., art/LICENSE.txt). Each clip's "
       "\"source\" records the file it came from, which is what carries that "
       "attribution forward."});
  manifest["family"] = recipe.family;
  manifest["skeleton"] = "skeleton.ozz";
  manifest["yaw_offset_degrees"] = recipe.yaw_offset_degrees;
  if (!sockets_json.empty()) manifest["sockets"] = sockets_json;
  manifest["clips"] = clips_json;

  const std::filesystem::path manifest_path = recipe.out_dir / "rig.json";
  std::ofstream out(manifest_path);
  if (!out) {
    report.error = "cannot write " + manifest_path.string();
    return report;
  }
  out << manifest.dump(2) << "\n";
  out.close();

  report.ok = true;
  return report;
}

}  // namespace badlands::rigpack
