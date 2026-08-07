#include "rigpack/intermediate.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

namespace badlands::rigpack {
namespace {

constexpr const char* kFormat = "badlands-anim-intermediate";
constexpr int kVersion = 1;

// The intermediate's own convention: 16 floats that memcpy straight into a
// glm::mat4. The writer already transposed COLLADA's row-major order.
glm::mat4 FromColumnMajor(const float* f) {
  glm::mat4 m(1.0f);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) m[c][r] = f[4 * c + r];
  }
  return m;
}

bool IsAffineTail(const float* f) {
  return std::abs(f[12]) < 1e-4f && std::abs(f[13]) < 1e-4f &&
         std::abs(f[14]) < 1e-4f && std::abs(f[15] - 1.0f) < 1e-4f;
}

bool IsAffineStride(const float* f) {
  return std::abs(f[3]) < 1e-4f && std::abs(f[7]) < 1e-4f &&
         std::abs(f[11]) < 1e-4f && std::abs(f[15] - 1.0f) < 1e-4f;
}

std::optional<glm::mat4> ParseMat4(const nlohmann::json& value) {
  if (!value.is_array() || value.size() != 16) return std::nullopt;
  float f[16];
  for (size_t i = 0; i < 16; ++i) {
    if (!value[i].is_number()) return std::nullopt;
    f[i] = value[i].get<float>();
  }
  return FromColumnMajor(f);
}

std::string Lowered(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ReadJson(const std::filesystem::path& path, nlohmann::json* doc,
              std::string* error) {
  std::ifstream stream(path);
  if (!stream) {
    *error = "cannot open " + path.string();
    return false;
  }
  try {
    stream >> *doc;
  } catch (const nlohmann::json::exception& e) {
    *error = path.string() + " is not valid JSON: " + e.what();
    return false;
  }
  return true;
}

bool CheckFormat(const nlohmann::json& doc, const std::filesystem::path& path,
                 std::string* error) {
  if (doc.value("format", std::string()) != kFormat) {
    *error = path.string() + " is not a " + kFormat + " file";
    return false;
  }
  if (doc.value("version", 0) != kVersion) {
    *error = path.string() + " is version " +
             std::to_string(doc.value("version", 0)) + ", expected " +
             std::to_string(kVersion);
    return false;
  }
  return true;
}

}  // namespace

std::optional<std::vector<FamilyEntry>> LoadManifest(
    const std::filesystem::path& root, std::string* error) {
  const std::filesystem::path path = root / "manifest.json";
  nlohmann::json doc;
  if (!ReadJson(path, &doc, error)) return std::nullopt;

  // Guarded as a whole: nlohmann's value() THROWS when a key exists with the
  // wrong type, and this is data from another repo. The header promises a
  // malformed intermediate is reported, not fatal.
  try {
  if (!CheckFormat(doc, path, error)) return std::nullopt;

  const auto families = doc.find("families");
  if (families == doc.end() || !families->is_array()) {
    *error = path.string() + " has no \"families\" array";
    return std::nullopt;
  }

  std::vector<FamilyEntry> out;
  for (const auto& value : *families) {
    FamilyEntry entry;
    entry.family = value.value("family", std::string());
    const std::string rig = value.value("rig", std::string());
    if (entry.family.empty() || rig.empty()) {
      *error = path.string() + " has a family without a name or a rig path";
      return std::nullopt;
    }
    entry.rig = root / rig;
    // The directory holding rig.json IS the slug -- deriving it from the name
    // would reinvent the disambiguation the exporter already did (`Main` and
    // `main` slug to `main` and `main_2`).
    entry.slug = entry.rig.parent_path().filename().string();
    out.push_back(std::move(entry));
  }
  return out;
  } catch (const nlohmann::json::exception& e) {
    *error = path.string() + " has a field of the wrong type: " + e.what();
    return std::nullopt;
  }
}

namespace {

// Resolves a family name to its rig.json through the manifest. Keying off the
// manifest rather than deriving a slug matters: 0 A.D. ships two DISTINCT
// families named `Main` and `main`, which slug to `main` and `main_2`.
std::optional<std::filesystem::path> ResolveRigPath(
    const std::filesystem::path& root, const std::string& family,
    std::string* error) {
  if (!std::filesystem::exists(root / "manifest.json")) {
    // A single-family `--family NAME` extract need not leave a manifest.
    const std::filesystem::path direct = root / family / "rig.json";
    if (std::filesystem::exists(direct)) return direct;
    const std::filesystem::path slug = root / Lowered(family) / "rig.json";
    if (std::filesystem::exists(slug)) return slug;
    *error = "no manifest.json in " + root.string() + " and no " + direct.string();
    return std::nullopt;
  }

  std::optional<std::vector<FamilyEntry>> manifest = LoadManifest(root, error);
  if (!manifest) return std::nullopt;

  // Exact match first: `Main` and `main` are different families, so a
  // case-insensitive match must never win over an exact one.
  for (const FamilyEntry& entry : *manifest) {
    if (entry.family == family) return entry.rig;
  }
  for (const FamilyEntry& entry : *manifest) {
    if (Lowered(entry.family) == Lowered(family)) return entry.rig;
  }

  *error = (root / "manifest.json").string() + " lists no family named \"" +
           family + "\"";
  return std::nullopt;
}

}  // namespace

const IntermediateClip* Intermediate::FindClip(const std::string& name,
                                               int* matches) const {
  if (matches != nullptr) *matches = 0;
  for (const IntermediateClip& clip : clips) {
    if (clip.name == name) {
      if (matches != nullptr) *matches = 1;  // unique by construction
      return &clip;
    }
  }

  // Then logical names, case-insensitively: one clip serves several states and
  // 0 A.D. writes both "attack_melee" and "Attack_melee" for the same thing.
  //
  // These are FAR from unique -- `idle` names 240 Biped clips -- so count them
  // all rather than stopping at the first. A recipe that leans on this is
  // choosing arbitrarily, and the caller has to be able to say so.
  const std::string wanted = Lowered(name);
  const IntermediateClip* found = nullptr;
  for (const IntermediateClip& clip : clips) {
    for (const std::string& logical : clip.logical_names) {
      if (Lowered(logical) != wanted) continue;
      if (found == nullptr) found = &clip;
      if (matches == nullptr) return found;  // caller does not care how many
      ++*matches;
      break;  // one clip counts once, however many aliases it spells
    }
  }
  return found;
}

std::optional<MatrixOrder> DetectMatrixOrder(const float* floats) {
  const bool tail = IsAffineTail(floats);
  const bool stride = IsAffineStride(floats);
  // Both hold whenever the translation is zero -- an identity, or a joint at its
  // parent's origin. That is AMBIGUOUS, not an answer: the affine structure sits
  // in both places and only the basis differs.
  if (tail == stride) return std::nullopt;
  return tail ? MatrixOrder::kRowMajor : MatrixOrder::kColumnMajor;
}

std::optional<Intermediate> LoadIntermediate(const std::filesystem::path& root,
                                             const std::string& family,
                                             std::string* error) {
  std::optional<std::filesystem::path> path = ResolveRigPath(root, family, error);
  if (!path) return std::nullopt;

  nlohmann::json doc;
  if (!ReadJson(*path, &doc, error)) return std::nullopt;

  // Guarded as a whole -- see LoadManifest. Every value() below reads a field
  // out of another repo's output and throws on a type mismatch.
  try {
  if (!CheckFormat(doc, *path, error)) return std::nullopt;

  Intermediate out;
  out.dir = path->parent_path();
  out.family = doc.value("family", family);

  // The conversion to engine space happens in the exporter, at joint 0 only.
  // A file that says otherwise would need a rotation applied here, so refuse
  // rather than pack a rig lying on its side.
  if (const std::string space = doc.value("coordinate_space", std::string("engine"));
      space != "engine") {
    *error = path->string() + " is in coordinate space \"" + space +
             "\", expected \"engine\"";
    return std::nullopt;
  }

  const auto joints_it = doc.find("joints");
  if (joints_it == doc.end() || !joints_it->is_array() || joints_it->empty()) {
    *error = path->string() + " has no non-empty \"joints\" array";
    return std::nullopt;
  }
  for (const auto& value : *joints_it) {
    IntermediateJoint joint;
    joint.name = value.value("name", std::string());
    joint.parent = value.value("parent", -1);
    if (joint.name.empty()) {
      *error = "a joint has no \"name\"";
      return std::nullopt;
    }
    out.joints.push_back(std::move(joint));
  }
  // Joint names must be UNIQUE, because everything downstream of
  // SkeletonBuilder resolves by name. Two joints sharing one would collapse
  // onto a single ozz track, appending both their keys to it -- producing
  // repeated timestamps that fail validation on EVERY clip, and reporting it as
  // "did not validate as a raw animation" with no hint of the real cause.
  {
    std::unordered_set<std::string> seen;
    for (const IntermediateJoint& joint : out.joints) {
      if (!seen.insert(joint.name).second) {
        *error = "two joints are both named \"" + joint.name +
                 "\"; joint names must be unique";
        return std::nullopt;
      }
    }
  }

  for (size_t i = 0; i < out.joints.size(); ++i) {
    const int parent = out.joints[i].parent;
    if (parent < -1 || parent >= static_cast<int>(out.joints.size())) {
      *error = "joint \"" + out.joints[i].name + "\" has out-of-range parent " +
               std::to_string(parent);
      return std::nullopt;
    }
    // Parents-before-children is what lets the tree be built in one pass.
    if (parent >= static_cast<int>(i)) {
      *error = "joint \"" + out.joints[i].name +
               "\" is not listed after its parent (parents must come first)";
      return std::nullopt;
    }
  }

  if (const auto sockets_it = doc.find("sockets");
      sockets_it != doc.end() && sockets_it->is_array()) {
    for (const auto& value : *sockets_it) {
      IntermediateSocket socket;
      socket.name = value.value("name", std::string());
      socket.parent = value.value("parent", -1);
      socket.drift = value.value("drift", 0.0f);
      if (socket.name.empty() || socket.parent < 0 ||
          socket.parent >= static_cast<int>(out.joints.size())) {
        *error = "socket \"" + socket.name +
                 "\" has no name or an out-of-range parent";
        return std::nullopt;
      }
      if (const auto offset_it = value.find("offset"); offset_it != value.end()) {
        std::optional<glm::mat4> offset = ParseMat4(*offset_it);
        if (!offset) {
          *error = "socket \"" + socket.name + "\" has a malformed offset";
          return std::nullopt;
        }
        socket.offset = *offset;
      }
      out.sockets.push_back(std::move(socket));
    }
  }

  const auto clips_it = doc.find("clips");
  if (clips_it == doc.end() || !clips_it->is_array()) {
    *error = path->string() + " has no \"clips\" array";
    return std::nullopt;
  }
  for (const auto& value : *clips_it) {
    IntermediateClip clip;
    clip.name = value.value("name", std::string());
    clip.source = value.value("source", std::string());
    clip.data = value.value("data", std::string());
    clip.frames = value.value("frames", 0);
    clip.duration = value.value("duration", 0.0f);
    clip.uniform = value.value("uniform", true);
    clip.root_motion = value.value("root_motion", 0.0f);

    if (const auto times_it = value.find("frame_times");
        times_it != value.end() && times_it->is_array()) {
      for (const auto& time : *times_it) {
        if (time.is_number()) clip.frame_times.push_back(time.get<float>());
      }
    }
    if (const auto markers_it = value.find("markers");
        markers_it != value.end() && markers_it->is_array()) {
      for (const auto& marker : *markers_it) {
        const std::string name = marker.value("name", std::string());
        if (!name.empty() && marker.contains("ratio")) {
          clip.markers.emplace_back(name, marker.value("ratio", 0.0f));
        }
      }
    }
    if (const auto logical_it = value.find("logical_names");
        logical_it != value.end() && logical_it->is_array()) {
      for (const auto& logical : *logical_it) {
        if (logical.is_string()) clip.logical_names.push_back(logical.get<std::string>());
      }
    }

    if (clip.name.empty() || clip.data.empty() || clip.frames <= 0) {
      *error = "clip \"" + clip.name + "\" is missing a name, data path or frames";
      return std::nullopt;
    }
    // Frame times are what the packer keys on, so a table that does not cover
    // every frame is a hard error rather than something to interpolate around.
    if (clip.frame_times.size() != static_cast<size_t>(clip.frames)) {
      *error = "clip \"" + clip.name + "\" has " +
               std::to_string(clip.frame_times.size()) + " frame_times for " +
               std::to_string(clip.frames) + " frames";
      return std::nullopt;
    }
    out.clips.push_back(std::move(clip));
  }

  return out;
  } catch (const nlohmann::json::exception& e) {
    *error = path->string() + " has a field of the wrong type: " + e.what();
    return std::nullopt;
  }
}

std::optional<std::vector<std::vector<glm::mat4>>> LoadClipMatrices(
    const Intermediate& intermediate, const IntermediateClip& clip,
    std::string* error, std::optional<MatrixOrder>* detected) {
  const std::filesystem::path path = intermediate.dir / clip.data;
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    *error = "cannot open " + path.string();
    return std::nullopt;
  }

  // Every clip is written at the family's FULL joint width, so this is an
  // equality and not a lower bound. The README says to assert it.
  const std::streamsize size = stream.tellg();
  const size_t joints = intermediate.joints.size();
  const size_t expected = static_cast<size_t>(clip.frames) * joints * 16 * sizeof(float);
  if (static_cast<size_t>(size) != expected) {
    *error = path.string() + " is " + std::to_string(size) + " bytes, expected " +
             std::to_string(expected) + " (" + std::to_string(clip.frames) +
             " frames x " + std::to_string(joints) + " joints x 16 floats)";
    return std::nullopt;
  }

  // The payload is little-endian float32, read straight into memory. Every
  // target this repo builds for (darwin/arm64, x86_64) is little-endian, so
  // there is no swap here -- a big-endian host would need one, and would also
  // need a great deal else.
  std::vector<float> floats(static_cast<size_t>(clip.frames) * joints * 16);
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(floats.data()), size);
  if (!stream) {
    *error = "short read on " + path.string();
    return std::nullopt;
  }

  // Most matrices are ambiguous (a zero translation reads the same either way),
  // so scan until one is decisive.
  if (detected != nullptr) {
    *detected = std::nullopt;
    for (size_t i = 0; i + 16 <= floats.size() && !detected->has_value(); i += 16) {
      *detected = DetectMatrixOrder(&floats[i]);
    }
  }

  std::vector<std::vector<glm::mat4>> out(static_cast<size_t>(clip.frames));
  for (size_t frame = 0; frame < out.size(); ++frame) {
    out[frame].resize(joints);
    for (size_t joint = 0; joint < joints; ++joint) {
      out[frame][joint] = FromColumnMajor(&floats[(frame * joints + joint) * 16]);
    }
  }
  return out;
}

}  // namespace badlands::rigpack
