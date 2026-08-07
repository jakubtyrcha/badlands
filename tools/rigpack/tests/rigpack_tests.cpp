// Catch2 suite for the offline rig packer (tools/rigpack).
//
// Every fixture is a SYNTHETIC intermediate written to a temp directory, packed,
// and then loaded back through the real AnimationSet::Load. Nothing here reads
// the 0 A.D. corpus or assets/characters/, so the suite runs on a fresh clone
// with no importer output present -- which is also what makes the packer
// testable before the importer exists.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/animation/animation_set.hpp"
#include "engine/animation/pose.hpp"
#include "engine/animation/sampler.hpp"
#include "engine/animation/skeleton.hpp"
#include "rigpack/intermediate.hpp"
#include "rigpack/pack.hpp"
#include "rigpack/recipe.hpp"

using namespace badlands;
using namespace badlands::rigpack;

namespace {

// A joint as the synthetic intermediate declares it. There is no bind pose in
// the real format, so the packer takes the rest from frame 0.
struct Joint {
  std::string name;
  int parent;
};

// The 16 floats of a translation matrix. The intermediate is column-major by
// contract; the row-major form exists only to feed the transposition guard.
std::vector<float> TranslationFloats(const glm::vec3& t, MatrixOrder order) {
  if (order == MatrixOrder::kRowMajor) {
    return {1, 0, 0, t.x, 0, 1, 0, t.y, 0, 0, 1, t.z, 0, 0, 0, 1};
  }
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t.x, t.y, t.z, 1};
}

std::vector<float> TranslationFloats(const glm::vec3& t) {
  return TranslationFloats(t, MatrixOrder::kColumnMajor);
}

std::filesystem::path TempRoot(const std::string& name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "badlands_rigpack_tests" / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "fixture" / "clips");
  return dir;
}

// Writes a one-family intermediate in the real `animexport` shape: a
// manifest.json indexing the family (whose DIRECTORY SLUG deliberately differs
// from its authored name), a rig.json, and one clip .bin.
void WriteIntermediate(const std::filesystem::path& root,
                       const std::vector<Joint>& joints,
                       const std::vector<std::vector<glm::vec3>>& frames,
                       const nlohmann::json& sockets,
                       MatrixOrder order = MatrixOrder::kColumnMajor,
                       const nlohmann::json& markers = nlohmann::json::array(),
                       const std::vector<float>& frame_times = {}) {
  nlohmann::json manifest;
  manifest["format"] = "badlands-anim-intermediate";
  manifest["version"] = 1;
  manifest["families"] = nlohmann::json::array();
  manifest["families"].push_back({{"family", "Fixture"},
                                  {"rig", "fixture/rig.json"},
                                  {"clips", 1},
                                  {"joints", static_cast<int>(joints.size())},
                                  {"sockets", static_cast<int>(sockets.size())}});
  manifest["skipped"] = nlohmann::json::array();
  std::ofstream manifest_out(root / "manifest.json");
  REQUIRE(manifest_out.good());
  manifest_out << manifest.dump(2);
  manifest_out.close();

  std::vector<float> times = frame_times;
  if (times.empty()) {
    for (size_t i = 0; i < frames.size(); ++i) {
      times.push_back(static_cast<float>(i) / 30.0f);
    }
  }
  REQUIRE(times.size() == frames.size());

  nlohmann::json doc;
  doc["format"] = "badlands-anim-intermediate";
  doc["version"] = 1;
  doc["family"] = "Fixture";
  doc["coordinate_space"] = "engine";
  for (const Joint& joint : joints) {
    doc["joints"].push_back({{"name", joint.name}, {"parent", joint.parent}});
  }
  doc["sockets"] = sockets;
  doc["clips"].push_back({{"name", "src_clip"},
                          {"source", "fixture/src_clip.dae"},
                          {"data", "clips/src_clip.bin"},
                          {"frames", static_cast<int>(frames.size())},
                          {"frame_times", times},
                          {"duration", times.back()},
                          {"uniform", true},
                          {"markers", markers},
                          {"logical_names", {"Fixture_Idle"}},
                          {"root_motion", 0.0f}});

  std::ofstream json_out(root / "fixture" / "rig.json");
  REQUIRE(json_out.good());
  json_out << doc.dump(2);
  json_out.close();

  std::ofstream bin(root / "fixture" / "clips" / "src_clip.bin", std::ios::binary);
  REQUIRE(bin.good());
  for (const std::vector<glm::vec3>& frame : frames) {
    REQUIRE(frame.size() == joints.size());
    for (const glm::vec3& translation : frame) {
      const std::vector<float> floats = TranslationFloats(translation, order);
      bin.write(reinterpret_cast<const char*>(floats.data()),
                static_cast<std::streamsize>(floats.size() * sizeof(float)));
    }
  }
}

// Writes a recipe naming the single fixture clip as "only", in `out_dir`.
std::filesystem::path WriteRecipe(const std::filesystem::path& out_dir) {
  std::filesystem::create_directories(out_dir);
  const std::filesystem::path path = out_dir / "pack.json";
  std::ofstream out(path);
  REQUIRE(out.good());
  out << R"({ "family": "Fixture", "yaw_offset_degrees": 0,
              "clips": { "only": "src_clip" } })";
  out.close();
  return path;
}

// Packs and loads, REQUIREing success at every step.
struct Packed {
  PackReport report;
  std::optional<AnimationSet> set;
};

Packed PackAndLoad(const std::filesystem::path& root) {
  const std::filesystem::path out_dir = root / "packed";
  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(WriteRecipe(out_dir), &error);
  REQUIRE(recipe.has_value());

  Packed packed;
  packed.report = Pack(*recipe, root);
  INFO(packed.report.error);
  REQUIRE(packed.report.ok);

  packed.set = AnimationSet::Load((out_dir / "rig.json").string());
  REQUIRE(packed.set.has_value());
  return packed;
}

// The loaded rig posed at the end of its only clip.
Pose PoseAtEnd(const AnimationSet& set) {
  Pose pose(set.skeleton());
  ClipSampler sampler;
  sampler.Reset(set.skeleton());
  REQUIRE(sampler.Sample(set.clip(0), 1.0f, pose));
  REQUIRE(LocalToModel(set.skeleton(), pose));
  return pose;
}

glm::vec3 AttachmentOrigin(const AnimationSet& set, const std::string& name,
                           const Pose& pose) {
  const int id = set.FindAttachment(name);
  REQUIRE(id >= 0);
  return glm::vec3(set.AttachmentTransform(id, pose)[3]);
}

}  // namespace

TEST_CASE("a packed rig round-trips into an AnimationSet", "[rigpack]") {
  const std::filesystem::path root = TempRoot("round_trip");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  nlohmann::json sockets = nlohmann::json::array();
  sockets.push_back({{"name", "backplate"},
                     {"parent", 1},
                     {"offset", TranslationFloats({0.0f, 0.0f, -0.5f})}});
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}}, sockets,
                    MatrixOrder::kColumnMajor,
                    {{{"name", "event"}, {"ratio", 0.5f}}});

  const Packed packed = PackAndLoad(root);

  CHECK(packed.report.joints == 2);
  CHECK(packed.report.sockets_kept == 1);
  CHECK(packed.report.dropped_collisions.empty());
  CHECK(packed.report.warnings.empty());
  REQUIRE(packed.report.clips.size() == 1);
  CHECK(packed.report.clips[0].logical == "only");
  CHECK(packed.report.clips[0].frames == 1);
  // Nothing moves the root, which is what the sim requires.
  CHECK(packed.report.clips[0].root_translation == Catch::Approx(0.0f).margin(1e-5));

  const AnimationSet& set = *packed.set;
  CHECK(set.family() == "Fixture");
  CHECK(set.skeleton().num_joints() == 2);
  CHECK(set.clip_count() == 1);
  CHECK(set.clip_name(0) == "only");
  // Joints and the surviving socket, in one namespace.
  CHECK(set.attachment_count() == 3);
  CHECK(set.FindAttachment("root") == 0);
  CHECK(set.FindAttachment("spine") == 1);
  CHECK(set.FindAttachment("backplate") == 2);
  // Markers survive the conversion.
  CHECK(set.clip_marker(0, "event") == Catch::Approx(0.5f));

  // The socket rides its parent joint, offset by half a unit behind it.
  const Pose pose = PoseAtEnd(set);
  const glm::vec3 socket = AttachmentOrigin(set, "backplate", pose);
  CHECK(socket.y == Catch::Approx(1.0f).margin(1e-4));
  CHECK(socket.z == Catch::Approx(-0.5f).margin(1e-4));
}

TEST_CASE("clip tracks follow the skeleton's joint order, not the intermediate's",
          "[rigpack]") {
  // SkeletonBuilder emits joints DEPTH-first. Listing the intermediate
  // BREADTH-first therefore makes the two orders genuinely disagree: the
  // intermediate has arm=2, head=3, while the built skeleton has head=2, arm=3.
  // A packer that indexed tracks by the intermediate's own index would animate
  // the ARM with the HEAD's data -- producing a rig that loads, plays, and is
  // wrong. If a future ozz changed its ordering, the REQUIREs below fail loudly
  // rather than letting this quietly stop testing anything.
  const std::filesystem::path root = TempRoot("reindex");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
      {"arm", 0},
      {"head", 1},
  };
  nlohmann::json sockets = nlohmann::json::array();
  sockets.push_back({{"name", "helmet"},
                     {"parent", 3},  // head, in the INTERMEDIATE's numbering
                     {"offset", TranslationFloats({0.0f, 0.25f, 0.0f})}});

  // Two frames, in the intermediate's joint order. Only the HEAD moves, +3 in Z.
  const std::vector<std::vector<glm::vec3>> frames = {
      {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 1, 0}},
      {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 1, 3}},
  };
  WriteIntermediate(root, joints, frames, sockets);

  const Packed packed = PackAndLoad(root);
  const AnimationSet& set = *packed.set;

  // The reorder is real, or this test proves nothing.
  REQUIRE(set.FindAttachment("head") == 2);
  REQUIRE(set.FindAttachment("arm") == 3);

  const Pose pose = PoseAtEnd(set);
  // The head moved and the arm did not, which is only true if every track was
  // placed by NAME.
  CHECK(AttachmentOrigin(set, "head", pose).z == Catch::Approx(3.0f).margin(0.01));
  CHECK(AttachmentOrigin(set, "arm", pose).z == Catch::Approx(0.0f).margin(0.01));
  // And the socket rode the head, not whatever joint index 3 became.
  const glm::vec3 helmet = AttachmentOrigin(set, "helmet", pose);
  CHECK(helmet.z == Catch::Approx(3.0f).margin(0.01));
  CHECK(helmet.y == Catch::Approx(2.25f).margin(0.01));
}

TEST_CASE("a socket colliding with a joint is dropped at pack time", "[rigpack]") {
  // 0 A.D. carries `weapon_R` as a joint AND `prop-weapon_R` as a prop node, a
  // millimetre apart. The joint is the animated one and wins; the collapse
  // happens here so the shipped manifest is already collision-free.
  const std::filesystem::path root = TempRoot("collision");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"weapon_R", 0},
  };
  nlohmann::json sockets = nlohmann::json::array();
  sockets.push_back({{"name", "weapon_R"}, {"parent", 0}});
  sockets.push_back({{"name", "quiver"}, {"parent", 0}});
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}}, sockets);

  const Packed packed = PackAndLoad(root);

  REQUIRE(packed.report.dropped_collisions.size() == 1);
  CHECK(packed.report.dropped_collisions[0] == "weapon_R");
  CHECK(packed.report.sockets_kept == 1);

  const AnimationSet& set = *packed.set;
  // One "weapon_R", and it is the JOINT -- so it sits where the joint does, not
  // at the dropped socket's parent (the root).
  CHECK(set.attachment_count() == 3);
  const int weapon = set.FindAttachment("weapon_R");
  REQUIRE(weapon >= 0);
  CHECK(weapon < set.skeleton().num_joints());

  const Pose pose = PoseAtEnd(set);
  CHECK(AttachmentOrigin(set, "weapon_R", pose).y == Catch::Approx(1.0f).margin(1e-4));

  // The manifest that came out is itself collision-free, so the runtime never
  // had to arbitrate -- no warning was logged on load.
  const std::filesystem::path manifest = root / "packed" / "rig.json";
  std::ifstream in(manifest);
  REQUIRE(in.good());
  nlohmann::json doc;
  in >> doc;
  REQUIRE(doc["sockets"].size() == 1);
  CHECK(doc["sockets"][0]["name"] == "quiver");
}

TEST_CASE("both matrix orders read to the same transform", "[rigpack]") {
  // The intermediate's 16 floats memcpy straight into a glm::mat4 -- the
  // exporter already transposed COLLADA's row-major order. Reading them the
  // other way yields a rig that loads and animates and is transposed
  // everywhere, so this pins the convention rather than leaving it to inspection.
  const std::filesystem::path root = TempRoot("order_col");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {2.0f, 1.0f, -3.0f}}},
                    nlohmann::json::array());

  const Packed packed = PackAndLoad(root);
  CHECK(packed.report.warnings.empty());

  // Margins are 1e-2, not 1e-4: ozz's runtime animation stores translation keys
  // as half floats, so a packed value is only ever good to about a thousandth.
  // Anything tighter would be asserting against the compressor, not the packer.
  const glm::vec3 origin = AttachmentOrigin(*packed.set, "spine", PoseAtEnd(*packed.set));
  CHECK(origin.x == Catch::Approx(2.0f).margin(0.01));
  CHECK(origin.y == Catch::Approx(1.0f).margin(0.01));
  CHECK(origin.z == Catch::Approx(-3.0f).margin(0.01));
}

TEST_CASE("a transposed payload is reported, not absorbed", "[rigpack]") {
  // The bytes are row-major, so the exporter's convention changed under us.
  // Nothing about the pack FAILS -- that is exactly the danger, since the rig
  // still loads and plays -- so the report has to say so.
  const std::filesystem::path root = TempRoot("order_mismatch");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {2.0f, 1.0f, -3.0f}}},
                    nlohmann::json::array(), MatrixOrder::kRowMajor);

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(WriteRecipe(root / "packed"), &error);
  REQUIRE(recipe.has_value());
  const PackReport report = Pack(*recipe, root);
  REQUIRE(report.ok);

  REQUIRE(report.warnings.size() == 1);
  CHECK_THAT(report.warnings[0],
             Catch::Matchers::ContainsSubstring("likely transposed"));
}

TEST_CASE("a clip is reachable by its logical name", "[rigpack]") {
  // One clip serves several states and 0 A.D. is inconsistent about case, so a
  // recipe may name a clip by `name` or by any of its logical names.
  const std::filesystem::path root = TempRoot("logical_name");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    // The fixture declares "Fixture_Idle"; the recipe asks in another case.
    out << R"({ "family": "Fixture", "clips": { "idle": "fixture_IDLE" } })";
  }

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  const PackReport report = Pack(*recipe, root);
  INFO(report.error);
  REQUIRE(report.ok);
  CHECK(report.warnings.empty());
  REQUIRE(report.clips.size() == 1);
  CHECK(report.clips[0].logical == "idle");
}

TEST_CASE("an ambiguous logical name is reported, not silently resolved",
          "[rigpack]") {
  // A logical name is a VARIANT CLASS, not a clip: `idle` names 240 clips on
  // Biped and 0 A.D. picks among them at random by weight. Taking the first is
  // fine; taking it silently is not, because the recipe then reads as a
  // deliberate choice that nobody made.
  const std::filesystem::path root = TempRoot("ambiguous");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  // A second clip sharing the first's logical name, spliced into the rig.json
  // the fixture just wrote.
  const std::filesystem::path rig = root / "fixture" / "rig.json";
  nlohmann::json doc;
  {
    std::ifstream in(rig);
    REQUIRE(in.good());
    in >> doc;
  }
  nlohmann::json second = doc["clips"][0];
  second["name"] = "src_clip_variant";
  doc["clips"].push_back(second);  // same data file, same logical name
  {
    std::ofstream out(rig);
    REQUIRE(out.good());
    out << doc.dump(2);
  }

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": { "idle": "Fixture_Idle" } })";
  }

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  const PackReport report = Pack(*recipe, root);
  INFO(report.error);
  REQUIRE(report.ok);

  REQUIRE(report.warnings.size() == 1);
  CHECK_THAT(report.warnings[0], Catch::Matchers::ContainsSubstring("matched 2 clips"));
  // The first in file order wins, and the report names it.
  CHECK_THAT(report.warnings[0], Catch::Matchers::ContainsSubstring("src_clip"));

  // Naming the clip directly is unambiguous and says nothing.
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": { "idle": "src_clip_variant" } })";
  }
  std::optional<Recipe> exact = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(exact.has_value());
  const PackReport quiet = Pack(*exact, root);
  REQUIRE(quiet.ok);
  CHECK(quiet.warnings.empty());
}

TEST_CASE("family resolution is case-SENSITIVE first", "[rigpack]") {
  // 0 A.D. ships two DISTINCT families named `Main` and `main`, slugged `main`
  // and `main_2`. They previously shared a directory on a case-insensitive
  // filesystem and one silently overwrote the other, costing 11 clips. A
  // case-insensitive family lookup would reintroduce exactly that confusion.
  const std::filesystem::path root = TempRoot("case_collision");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  // Rename the fixture family to `main`, and add a DIFFERENT family `Main`
  // listed first, so a case-blind match would find the wrong one.
  const std::filesystem::path rig = root / "fixture" / "rig.json";
  nlohmann::json doc;
  {
    std::ifstream in(rig);
    REQUIRE(in.good());
    in >> doc;
  }
  doc["family"] = "main";
  {
    std::ofstream out(rig);
    REQUIRE(out.good());
    out << doc.dump(2);
  }

  std::filesystem::create_directories(root / "main_upper");
  nlohmann::json other = doc;
  other["family"] = "Main";
  other["clips"] = nlohmann::json::array();  // no clips: packing it must fail
  {
    std::ofstream out(root / "main_upper" / "rig.json");
    REQUIRE(out.good());
    out << other.dump(2);
  }

  nlohmann::json manifest;
  {
    std::ifstream in(root / "manifest.json");
    REQUIRE(in.good());
    in >> manifest;
  }
  manifest["families"] = nlohmann::json::array();
  manifest["families"].push_back({{"family", "Main"}, {"rig", "main_upper/rig.json"}});
  manifest["families"].push_back({{"family", "main"}, {"rig", "fixture/rig.json"}});
  {
    std::ofstream out(root / "manifest.json");
    REQUIRE(out.good());
    out << manifest.dump(2);
  }

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "main", "clips": { "only": "src_clip" } })";
  }

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  const PackReport report = Pack(*recipe, root);

  // The exact match wins even though `Main` is listed first.
  INFO(report.error);
  REQUIRE(report.ok);
  CHECK(report.family == "main");
  CHECK(report.clips.size() == 1);
}

TEST_CASE("a \"*\" recipe packs the whole family", "[rigpack]") {
  // The browse case: the viewer should be able to show everything a skeleton can
  // do, not a curated handful. Each clip keeps its own name, so no mapping is
  // invented for clips nobody selected.
  const std::filesystem::path root = TempRoot("wildcard");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path rig = root / "fixture" / "rig.json";
  nlohmann::json doc;
  {
    std::ifstream in(rig);
    REQUIRE(in.good());
    in >> doc;
  }
  nlohmann::json second = doc["clips"][0];
  second["name"] = "another_clip";
  doc["clips"].push_back(second);
  {
    std::ofstream out(rig);
    REQUIRE(out.good());
    out << doc.dump(2);
  }

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": "*" })";
  }

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  CHECK(recipe->all_clips);
  CHECK(recipe->clips.empty());

  const PackReport report = Pack(*recipe, root);
  INFO(report.error);
  REQUIRE(report.ok);
  CHECK(report.warnings.empty());
  REQUIRE(report.clips.size() == 2);

  std::optional<AnimationSet> set =
      AnimationSet::Load((out_dir / "rig.json").string());
  REQUIRE(set.has_value());
  CHECK(set->clip_count() == 2);
  // Named by the intermediate, in its order -- no logical vocabulary invented.
  CHECK(set->FindClip("src_clip") == 0);
  CHECK(set->FindClip("another_clip") == 1);
}

TEST_CASE("a \"families\" recipe packs one rig per slug", "[rigpack]") {
  // 31 creature families ship, and 31 near-identical recipes would be
  // boilerplate -- but the list still has to be checked in, so one recipe names
  // them all and each lands in its own directory.
  const std::filesystem::path root = TempRoot("families");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "families": ["Fixture", "NoSuchFamily"], "clips": "*" })";
  }

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  REQUIRE(recipe->families.size() == 2);
  CHECK(recipe->family.empty());

  const std::vector<PackReport> reports = PackFamilies(*recipe, root);
  REQUIRE(reports.size() == 2);

  // The output directory is the exporter's own SLUG ("fixture"), not the
  // authored family name -- that is what keeps `Main` and `main` apart.
  CHECK(reports[0].ok);
  CHECK(reports[0].out_dir.filename() == "fixture");
  std::optional<AnimationSet> set =
      AnimationSet::Load((out_dir / "fixture" / "rig.json").string());
  CHECK(set.has_value());

  // One bad family does not stop the rest: packing 31 rigs and losing all of
  // them because the 9th is malformed would be the wrong trade.
  CHECK_FALSE(reports[1].ok);
  CHECK(reports[1].family == "NoSuchFamily");
  CHECK_THAT(reports[1].error, Catch::Matchers::ContainsSubstring("no family named"));
}

TEST_CASE("a wrong-typed field is reported, not thrown", "[rigpack]") {
  // nlohmann's value() THROWS when a key exists with the wrong type, and both
  // files rigpack reads are hand-authored or come from another repo. pack.hpp
  // promises every failure comes back in a report; an uncaught type_error would
  // abort the process instead (verified: exit 134 before this was guarded).
  const std::filesystem::path root = TempRoot("wrong_types");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  SECTION("in the recipe") {
    const std::filesystem::path path = root / "bad_recipe.json";
    {
      std::ofstream out(path);
      REQUIRE(out.good());
      out << R"({ "family": "Fixture", "yaw_offset_degrees": "0", "clips": "*" })";
    }
    std::string error;
    CHECK_FALSE(LoadRecipe(path, &error).has_value());
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("wrong type"));
  }

  SECTION("in the intermediate") {
    const std::filesystem::path rig = root / "fixture" / "rig.json";
    nlohmann::json doc;
    {
      std::ifstream in(rig);
      REQUIRE(in.good());
      in >> doc;
    }
    doc["clips"][0]["frames"] = "24";  // a string where a number belongs
    {
      std::ofstream out(rig);
      REQUIRE(out.good());
      out << doc.dump(2);
    }
    std::string error;
    CHECK_FALSE(LoadIntermediate(root, "Fixture", &error).has_value());
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("wrong type"));
  }
}

TEST_CASE("duplicate joint names are refused by name", "[rigpack]") {
  // Everything downstream of SkeletonBuilder resolves by name, so two joints
  // sharing one would collapse onto a single ozz track and append both their
  // keys to it -- repeated timestamps that fail validation on EVERY clip,
  // surfacing as "did not validate as a raw animation" with no hint of why.
  const std::filesystem::path root = TempRoot("duplicate_joints");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}},
                    nlohmann::json::array());

  std::string error;
  CHECK_FALSE(LoadIntermediate(root, "Fixture", &error).has_value());
  CHECK_THAT(error, Catch::Matchers::ContainsSubstring("both named \"spine\""));
}

TEST_CASE("clip names that break the manifest are refused", "[rigpack]") {
  const std::filesystem::path root = TempRoot("bad_clip_names");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  const auto pack_with = [&](const char* clips) {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": )" << clips << " }";
    out.close();
    std::string error;
    std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
    REQUIRE(recipe.has_value());
    return Pack(*recipe, root);
  };

  // Two names differing only in case are two manifest entries and ONE file on a
  // case-insensitive filesystem, which macOS is by default.
  CHECK_THAT(pack_with(R"({ "Idle": "src_clip", "idle": "src_clip" })").error,
             Catch::Matchers::ContainsSubstring("differ only in case"));

  // A separator would write outside clips/.
  CHECK_THAT(pack_with(R"({ "../escape": "src_clip" })").error,
             Catch::Matchers::ContainsSubstring("path separator"));
}

TEST_CASE("a '_'-prefixed clip name from the intermediate is refused",
          "[rigpack]") {
  // A recipe cannot express this -- LoadRecipe already treats a leading '_' as
  // its own comment key. It reaches the packer only through "*", where names
  // come from another repo's data. Left alone the clip would pack, be counted,
  // and then be invisible: AnimationSet::Load skips '_' keys as comments.
  const std::filesystem::path root = TempRoot("underscore_clip");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path rig = root / "fixture" / "rig.json";
  nlohmann::json doc;
  {
    std::ifstream in(rig);
    REQUIRE(in.good());
    in >> doc;
  }
  doc["clips"][0]["name"] = "_hidden";
  {
    std::ofstream out(rig);
    REQUIRE(out.good());
    out << doc.dump(2);
  }

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": "*" })";
  }
  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  CHECK_THAT(Pack(*recipe, root).error,
             Catch::Matchers::ContainsSubstring("comment key"));
}

TEST_CASE("out-of-order frame times still yield a sorted clip", "[rigpack]") {
  // Comparing each frame only against its neighbour does NOT guarantee a sorted
  // result: [0.5, 0.6, 0.1, 0.2] survives that test as [0.5, 0.1, 0.2], and ozz
  // then rejects the clip with a message about validation rather than time order.
  const std::filesystem::path root = TempRoot("unsorted_times");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  const std::vector<std::vector<glm::vec3>> frames = {
      {{0, 0, 0}, {0.0f, 1.0f, 0.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 1.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 2.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 3.0f}},
  };
  WriteIntermediate(root, joints, frames, nlohmann::json::array(),
                    MatrixOrder::kColumnMajor, nlohmann::json::array(),
                    {0.5f, 0.6f, 0.1f, 0.2f});

  // The clip survives -- which it only can if the surviving keys are sorted.
  const Packed packed = PackAndLoad(root);
  REQUIRE(packed.report.clips.size() == 1);
  CHECK_THAT(packed.report.warnings[0],
             Catch::Matchers::ContainsSubstring("strictly increasing"));
}

TEST_CASE("a failed pack leaves no half-written rig", "[rigpack]") {
  // In an --all run a directory holding a skeleton.ozz and no rig.json would sit
  // among 30 good ones and only fail later, when something tried to load it.
  const std::filesystem::path root = TempRoot("partial_output");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": { "only": "no_such_clip" } })";
  }
  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());

  const PackReport report = Pack(*recipe, root);
  REQUIRE_FALSE(report.ok);
  CHECK_FALSE(std::filesystem::exists(out_dir / "skeleton.ozz"));
  CHECK_FALSE(std::filesystem::exists(out_dir / "rig.json"));
}

TEST_CASE("a repack removes clips the recipe no longer names", "[rigpack]") {
  // Otherwise a dropped clip stays on disk forever with nothing referencing it
  // -- and for the shipped rigs, in git LFS.
  const std::filesystem::path root = TempRoot("stale_clips");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path out_dir = root / "packed";
  const auto pack_as = [&](const char* logical) {
    std::filesystem::create_directories(out_dir);
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture", "clips": { ")" << logical
        << R"(": "src_clip" } })";
    out.close();
    std::string error;
    std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
    REQUIRE(recipe.has_value());
    const PackReport report = Pack(*recipe, root);
    INFO(report.error);
    REQUIRE(report.ok);
  };

  pack_as("first");
  REQUIRE(std::filesystem::exists(out_dir / "clips" / "first.ozz"));
  pack_as("second");
  CHECK(std::filesystem::exists(out_dir / "clips" / "second.ozz"));
  CHECK_FALSE(std::filesystem::exists(out_dir / "clips" / "first.ozz"));
}

TEST_CASE("a recipe naming both family and families is refused", "[rigpack]") {
  const std::filesystem::path root = TempRoot("both_forms");
  const std::filesystem::path path = root / "pack.json";
  {
    std::ofstream out(path);
    REQUIRE(out.good());
    out << R"({ "family": "A", "families": ["B"], "clips": "*" })";
  }
  std::string error;
  CHECK_FALSE(LoadRecipe(path, &error).has_value());
  CHECK_THAT(error, Catch::Matchers::ContainsSubstring("exactly one"));
}

TEST_CASE("frame times outside the clip are clamped, not fatal", "[rigpack]") {
  // quadraped__rabbit_walk starts at -0.0417 s, and ozz requires keys inside
  // [0, duration]. A frame a hair before zero is a sampler artefact, not
  // content, so it is clamped -- which then makes it a duplicate of the real
  // first key, and the equal-time merge absorbs it.
  const std::filesystem::path root = TempRoot("negative_time");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  const std::vector<std::vector<glm::vec3>> frames = {
      {{0, 0, 0}, {0.0f, 1.0f, 9.0f}},  // before zero: clamped, then superseded
      {{0, 0, 0}, {0.0f, 1.0f, 0.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 3.0f}},
  };
  WriteIntermediate(root, joints, frames, nlohmann::json::array(),
                    MatrixOrder::kColumnMajor, nlohmann::json::array(),
                    {-0.05f, 0.0f, 1.0f});

  const Packed packed = PackAndLoad(root);
  REQUIRE(packed.report.clips.size() == 1);
  CHECK_THAT(packed.report.warnings[0],
             Catch::Matchers::ContainsSubstring("clamped into range"));

  // The pre-zero frame lost to the real one at t=0, so the clip starts at 0.0
  // rather than at the 9.0 that sat outside it.
  Pose pose(packed.set->skeleton());
  ClipSampler sampler;
  sampler.Reset(packed.set->skeleton());
  REQUIRE(sampler.Sample(packed.set->clip(0), 0.0f, pose));
  REQUIRE(LocalToModel(packed.set->skeleton(), pose));
  CHECK(AttachmentOrigin(*packed.set, "spine", pose).z ==
        Catch::Approx(0.0f).margin(0.01));
}

TEST_CASE("frames sharing a timestamp are merged, not dropped", "[rigpack]") {
  // ozz needs strictly increasing key times and the corpus does not always
  // oblige -- biped__new__boat_fisherman_idle authors three pairs of frames at
  // the same instant. Rejecting the clip loses real content over a degenerate
  // COLLADA sampler, so the last frame of each equal-time run wins.
  const std::filesystem::path root = TempRoot("duplicate_times");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  const std::vector<std::vector<glm::vec3>> frames = {
      {{0, 0, 0}, {0.0f, 1.0f, 0.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 5.0f}},  // superseded: same time as the next
      {{0, 0, 0}, {0.0f, 1.0f, 2.0f}},
  };
  WriteIntermediate(root, joints, frames, nlohmann::json::array(),
                    MatrixOrder::kColumnMajor, nlohmann::json::array(),
                    {0.0f, 1.0f, 1.0f});

  const Packed packed = PackAndLoad(root);
  // The clip SURVIVES, which is the point.
  REQUIRE(packed.report.clips.size() == 1);
  REQUIRE(packed.report.warnings.size() == 1);
  CHECK_THAT(packed.report.warnings[0],
             Catch::Matchers::ContainsSubstring("strictly increasing"));

  // The last frame of the run is the one that held, so the end pose is 2.0 --
  // not the 5.0 of the frame it superseded.
  const float z = AttachmentOrigin(*packed.set, "spine", PoseAtEnd(*packed.set)).z;
  CHECK(z == Catch::Approx(2.0f).margin(0.01));
}

TEST_CASE("unevenly sampled frames keep their own times", "[rigpack]") {
  // 135 clips in the corpus are not evenly sampled -- wolf_idle_01 holds a pose
  // for 5.1 s between frames 0.067 s apart. Keying on a constant rate would
  // silently redistribute that motion across the clip.
  const std::filesystem::path root = TempRoot("uneven");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  // Three frames at 0.0, 0.1 and 4.0 s: the pose is HELD for the long gap, so at
  // the clip's midpoint the spine must still be near its first value. Assuming a
  // constant step would place the midpoint between frames 1 and 2 instead.
  const std::vector<std::vector<glm::vec3>> frames = {
      {{0, 0, 0}, {0.0f, 1.0f, 0.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 0.0f}},
      {{0, 0, 0}, {0.0f, 1.0f, 8.0f}},
  };
  WriteIntermediate(root, joints, frames, nlohmann::json::array(),
                    MatrixOrder::kColumnMajor, nlohmann::json::array(),
                    {0.0f, 0.1f, 4.0f});

  const Packed packed = PackAndLoad(root);
  CHECK(packed.report.clips[0].duration_seconds == Catch::Approx(4.0f).margin(1e-3));

  Pose pose(packed.set->skeleton());
  ClipSampler sampler;
  sampler.Reset(packed.set->skeleton());
  REQUIRE(sampler.Sample(packed.set->clip(0), 0.5f, pose));  // t = 2.0 s
  REQUIRE(LocalToModel(packed.set->skeleton(), pose));

  // Halfway through the clip is halfway through the HOLD, so the spine has
  // travelled about half the gap's worth -- not the ~8 a uniform reading of
  // three frames would give at its own midpoint.
  const float z = AttachmentOrigin(*packed.set, "spine", pose).z;
  CHECK(z == Catch::Approx(8.0f * (2.0f - 0.1f) / (4.0f - 0.1f)).margin(0.05));
}

TEST_CASE("a recipe naming an absent clip costs one clip, not the rig",
          "[rigpack]") {
  const std::filesystem::path root = TempRoot("missing_clip");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  WriteIntermediate(root, joints, {{{0, 0, 0}, {0, 1, 0}}},
                    nlohmann::json::array());

  const std::filesystem::path out_dir = root / "packed";
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream out(out_dir / "pack.json");
    REQUIRE(out.good());
    out << R"({ "family": "Fixture",
                "clips": { "only": "src_clip", "ghost": "not_in_the_corpus" } })";
  }

  std::string error;
  std::optional<Recipe> recipe = LoadRecipe(out_dir / "pack.json", &error);
  REQUIRE(recipe.has_value());
  const PackReport report = Pack(*recipe, root);

  // The rig still packs, minus the clip that does not exist -- the same policy
  // AnimationSet::Load applies to an unreadable clip file.
  REQUIRE(report.ok);
  CHECK(report.clips.size() == 1);
  REQUIRE(report.warnings.size() == 1);
  CHECK_THAT(report.warnings[0], Catch::Matchers::ContainsSubstring("ghost"));
}

TEST_CASE("baked root motion is measured and reported", "[rigpack]") {
  // The sim owns movement, so a clip that also translates its root moves a
  // character twice. The importer strips this; the packer measures it anyway,
  // because "it was supposed to be stripped" is not a check.
  const std::filesystem::path root = TempRoot("root_motion");
  const std::vector<Joint> joints = {
      {"root", -1},
      {"spine", 0},
  };
  const std::vector<std::vector<glm::vec3>> frames = {
      {{0, 0, 0}, {0, 1, 0}},
      {{0, 0, 4.0f}, {0, 1, 0}},
  };
  WriteIntermediate(root, joints, frames, nlohmann::json::array());

  const Packed packed = PackAndLoad(root);
  REQUIRE(packed.report.clips.size() == 1);
  CHECK(packed.report.clips[0].root_translation == Catch::Approx(4.0f).margin(1e-3));
}
