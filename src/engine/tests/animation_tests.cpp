// Catch2 suite for the skeletal-animation runtime (src/engine/animation).
//
// Fixtures are BUILT IN-TEST from ozz's offline builders and written to temp
// .ozz files, then loaded back through the real Skeleton::Load /
// AnimationClip::Load path. Nothing is committed and nothing asserts against
// shipped data files, so retuning assets/characters/ can never break this.

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/soa_transform.h>

#include "engine/tests/animation_fixture.hpp"

#include "engine/animation/animation_clip.hpp"
#include "engine/animation/animation_set.hpp"
#include "engine/animation/attachment_lines.hpp"
#include "engine/animation/pose.hpp"
#include "engine/animation/sampler.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/skeleton_lines.hpp"
#include "engine/rendering/debug_line_buffer.hpp"

using namespace badlands;
using namespace badlands::test;

namespace {

// Reads joint `index`'s translation X out of a pose's SoA locals. SoA packs
// four joints per entry, so joint i lives at entry i/4, lane i%4.
float LocalTranslationX(const Pose& pose, int index) {
  const ozz::math::SoaTransform& soa = pose.locals()[static_cast<size_t>(index / 4)];
  float lanes[4];
  ozz::math::StorePtrU(soa.translation.x, lanes);
  return lanes[index % 4];
}

// Model-space origin of joint `index` (column 3 of its matrix).
glm::vec3 ModelOrigin(const Pose& pose, int index) {
  return glm::vec3(ToMat4(pose.models()[static_cast<size_t>(index)])[3]);
}

struct Fixture {
  Skeleton skeleton;
  AnimationClip clip;
};

Fixture LoadFixture(float duration = 1.0f) {
  const std::string skeleton_path =
      WriteOzz(*BuildRawSkeleton(), "fixture_skeleton.ozz");
  const std::string clip_path =
      WriteOzz(*BuildRawAnimation(duration), "fixture_clip.ozz");

  std::optional<Skeleton> skeleton = Skeleton::Load(skeleton_path);
  REQUIRE(skeleton.has_value());
  std::optional<AnimationClip> clip = AnimationClip::Load(clip_path);
  REQUIRE(clip.has_value());
  return Fixture{std::move(*skeleton), std::move(*clip)};
}

}  // namespace

TEST_CASE("skeleton round-trips through a .ozz archive", "[animation]") {
  Fixture fixture = LoadFixture();

  CHECK(fixture.skeleton.num_joints() == kFixtureJoints);
  CHECK(fixture.skeleton.num_roots() == kFixtureRoots);
  CHECK(fixture.skeleton.num_soa_joints() == 1);  // 3 joints pack into one SoA entry

  // Exactly one joint has no parent, and the children point at it.
  const ozz::span<const int16_t> parents = fixture.skeleton.joint_parents();
  REQUIRE(parents.size() == kFixtureJoints);
  CHECK(parents[0] == ozz::animation::Skeleton::kNoParent);
  CHECK(parents[1] == 0);
  CHECK(parents[2] == 0);
}

TEST_CASE("loading refuses an archive of the wrong type", "[animation]") {
  // A manifest naming a clip where a skeleton belongs is a content mistake we
  // want reported, not deserialized into garbage.
  const std::string clip_path = WriteOzz(*BuildRawAnimation(), "wrong_tag.ozz");
  CHECK_FALSE(Skeleton::Load(clip_path).has_value());
  CHECK_FALSE(AnimationClip::Load("/nonexistent/path/missing.ozz").has_value());
}

TEST_CASE("sampling is linear across the unit interval", "[animation]") {
  Fixture fixture = LoadFixture();
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);

  REQUIRE(sampler.Sample(fixture.clip, 0.0f, pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(0.0f).margin(1e-4));

  REQUIRE(sampler.Sample(fixture.clip, 0.5f, pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(5.0f).margin(1e-3));

  REQUIRE(sampler.Sample(fixture.clip, 1.0f, pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(10.0f).margin(1e-3));
}

TEST_CASE("out-of-range and non-finite ratios are clamped, never passed on",
          "[animation]") {
  Fixture fixture = LoadFixture();
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);

  // Below 0 holds the first frame.
  REQUIRE(sampler.Sample(fixture.clip, -1.0f, pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(0.0f).margin(1e-4));

  // Above 1 holds the last frame.
  REQUIRE(sampler.Sample(fixture.clip, 2.0f, pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(10.0f).margin(1e-3));

  // A NaN ratio must behave as 0 rather than propagating: std::clamp would let
  // it through, since every comparison against NaN is false.
  REQUIRE(sampler.Sample(fixture.clip, std::nanf(""), pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(0.0f).margin(1e-4));
  CHECK(std::isfinite(LocalTranslationX(pose, 0)));
}

TEST_CASE("an unsized sampler refuses to sample", "[animation]") {
  Fixture fixture = LoadFixture();
  Pose pose(fixture.skeleton);
  ClipSampler sampler;  // deliberately not Reset()
  CHECK_FALSE(sampler.Sample(fixture.clip, 0.5f, pose));
}

TEST_CASE("RatioAt guards a zero-length clip instead of dividing by it",
          "[animation]") {
  Fixture unit = LoadFixture(1.0f);
  CHECK(RatioAt(unit.clip, 0.0f) == Catch::Approx(0.0f));
  CHECK(RatioAt(unit.clip, 0.25f) == Catch::Approx(0.25f));
  // Past the end clamps rather than running off the clip.
  CHECK(RatioAt(unit.clip, 5.0f) == Catch::Approx(1.0f));

  Fixture two_second = LoadFixture(2.0f);
  CHECK(RatioAt(two_second.clip, 1.0f) == Catch::Approx(0.5f));
}

TEST_CASE("a looping clip keeps advancing past its duration", "[animation]") {
  // The viewer's playback path: advance seconds, convert, sample. Past the
  // clip's duration a LOOP must keep cycling. Composing the clamping
  // seconds->ratio helper with WrapRatio does NOT do this -- the clamp pins the
  // ratio at 1.0 and the wrap turns that into 0.0 forever -- which is why the
  // loop path gets its own conversion rather than being assembled at call sites.
  Fixture fixture = LoadFixture(1.0f);  // 1-second clip

  // Pinned so the composition cannot quietly come back: RatioAt clamps, so
  // wrapping its result yields 0 for EVERY time past the duration. That is the
  // freeze, expressed as an assertion rather than a comment.
  CHECK(WrapRatio(RatioAt(fixture.clip, 1.25f)) == Catch::Approx(0.0f));
  CHECK(WrapRatio(RatioAt(fixture.clip, 99.0f)) == Catch::Approx(0.0f));

  CHECK(LoopRatioAt(fixture.clip, 0.25f) == Catch::Approx(0.25f));
  // Second cycle: 1.25s into a 1s clip is a quarter through, not the start.
  CHECK(LoopRatioAt(fixture.clip, 1.25f) == Catch::Approx(0.25f));
  // Many cycles later it is still cycling, not frozen.
  CHECK(LoopRatioAt(fixture.clip, 7.5f) == Catch::Approx(0.5f));
  // Exactly on a boundary wraps to the start rather than sticking at the end.
  CHECK(LoopRatioAt(fixture.clip, 2.0f) == Catch::Approx(0.0f));

  // A zero-length clip still must not divide.
  Fixture two_second = LoadFixture(2.0f);
  CHECK(LoopRatioAt(two_second.clip, 3.0f) == Catch::Approx(0.5f));

  // And the pose it produces genuinely differs across cycles -- the regression
  // this guards is a rig that visibly freezes after one loop.
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);
  REQUIRE(sampler.Sample(fixture.clip, LoopRatioAt(fixture.clip, 1.5f), pose));
  CHECK(LocalTranslationX(pose, 0) == Catch::Approx(5.0f).margin(1e-3));

  // The viewer's actual playback loop: accumulate presentation seconds frame by
  // frame and sample. Run it well past the clip's duration and require the pose
  // to keep MOVING. Testing the helpers in isolation is what let the frozen
  // composition ship, so this models the call site rather than the parts.
  float anim_seconds = 0.0f;
  constexpr float kFrameDt = 1.0f / 60.0f;
  int distinct_after_first_cycle = 0;
  float previous = -1.0f;
  for (int frame = 0; frame < 240; ++frame) {  // 4 seconds = 4 cycles
    anim_seconds += kFrameDt;
    REQUIRE(sampler.Sample(fixture.clip, LoopRatioAt(fixture.clip, anim_seconds),
                           pose));
    if (anim_seconds > 1.0f) {  // past the first cycle, where the freeze hit
      const float x = LocalTranslationX(pose, 0);
      if (std::abs(x - previous) > 1e-3f) ++distinct_after_first_cycle;
      previous = x;
    }
  }
  // A frozen rig would sit at a single value; a looping one sweeps 0..10 thrice.
  CHECK(distinct_after_first_cycle > 150);
}

TEST_CASE("WrapRatio loops instead of clamping", "[animation]") {
  CHECK(WrapRatio(0.25f) == Catch::Approx(0.25f));
  CHECK(WrapRatio(1.25f) == Catch::Approx(0.25f));
  CHECK(WrapRatio(3.5f) == Catch::Approx(0.5f));
  // Negatives wrap forward, so a rewound loop stays inside the clip.
  CHECK(WrapRatio(-0.25f) == Catch::Approx(0.75f));
  CHECK(WrapRatio(std::nanf("")) == Catch::Approx(0.0f));
}

TEST_CASE("LocalToModel composes a child onto its parent", "[animation]") {
  Fixture fixture = LoadFixture();
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);

  // Root slid halfway: x = 5. Children keep their rest offsets in LOCAL space,
  // so in MODEL space they must have inherited the root's x.
  REQUIRE(sampler.Sample(fixture.clip, 0.5f, pose));
  REQUIRE(LocalToModel(fixture.skeleton, pose));

  const glm::vec3 root = ModelOrigin(pose, 0);
  const glm::vec3 child_a = ModelOrigin(pose, 1);
  const glm::vec3 child_b = ModelOrigin(pose, 2);

  CHECK(root.x == Catch::Approx(5.0f).margin(1e-3));
  CHECK(child_a.x == Catch::Approx(5.0f).margin(1e-3));
  CHECK(child_a.y == Catch::Approx(kFixtureChildOffsetY).margin(1e-3));
  CHECK(child_b.x == Catch::Approx(5.0f).margin(1e-3));
  CHECK(child_b.y == Catch::Approx(-kFixtureChildOffsetY).margin(1e-3));
}

TEST_CASE("blending two poses weights their locals", "[animation]") {
  Fixture fixture = LoadFixture();
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);

  Pose start(fixture.skeleton);
  Pose end(fixture.skeleton);
  REQUIRE(sampler.Sample(fixture.clip, 0.0f, start));
  REQUIRE(sampler.Sample(fixture.clip, 1.0f, end));

  Pose blended(fixture.skeleton);
  const std::array<BlendLayer, 2> layers{
      BlendLayer{&start, 0.5f},
      BlendLayer{&end, 0.5f},
  };
  REQUIRE(BlendPoses(fixture.skeleton, layers, blended));

  // Half of x=0 and half of x=10.
  CHECK(LocalTranslationX(blended, 0) == Catch::Approx(5.0f).margin(1e-3));

  // A null layer is refused rather than dereferenced.
  const std::array<BlendLayer, 1> bad{BlendLayer{nullptr, 1.0f}};
  CHECK_FALSE(BlendPoses(fixture.skeleton, bad, blended));
}

TEST_CASE("blend weight decides which pose dominates", "[animation]") {
  Fixture fixture = LoadFixture();
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);

  Pose start(fixture.skeleton);
  Pose end(fixture.skeleton);
  REQUIRE(sampler.Sample(fixture.clip, 0.0f, start));
  REQUIRE(sampler.Sample(fixture.clip, 1.0f, end));

  Pose blended(fixture.skeleton);
  const std::array<BlendLayer, 2> layers{
      BlendLayer{&start, 0.25f},
      BlendLayer{&end, 0.75f},
  };
  REQUIRE(BlendPoses(fixture.skeleton, layers, blended));
  CHECK(LocalTranslationX(blended, 0) == Catch::Approx(7.5f).margin(1e-3));
}

TEST_CASE("the skeleton emitter draws one segment per non-root joint",
          "[animation]") {
  Fixture fixture = LoadFixture();
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);
  REQUIRE(sampler.Sample(fixture.clip, 0.0f, pose));
  REQUIRE(LocalToModel(fixture.skeleton, pose));

  DebugLineBuffer lines;
  EmitSkeletonLines(fixture.skeleton, pose, glm::mat4(1.0f), lines);

  CHECK(lines.lines.size() ==
        static_cast<size_t>(kFixtureJoints - kFixtureRoots));

  // Both bones start at the root's origin and reach its children.
  for (const DebugLine& line : lines.lines) {
    CHECK(line.start.x == Catch::Approx(0.0f).margin(1e-3));
    CHECK(line.start.y == Catch::Approx(0.0f).margin(1e-3));
    CHECK(std::abs(line.end.y) == Catch::Approx(kFixtureChildOffsetY).margin(1e-3));
  }

  // Appending is additive: a second character's bones join the same buffer,
  // which is what lets one frame carry several overlays.
  EmitSkeletonLines(fixture.skeleton, pose, glm::mat4(1.0f), lines);
  CHECK(lines.lines.size() ==
        static_cast<size_t>(2 * (kFixtureJoints - kFixtureRoots)));
}

TEST_CASE("a manifest accepts both clip forms and defaults the pivot",
          "[animation]") {
  // Written in-test rather than asserting against assets/characters/, so
  // retuning shipped data can never break this.
  const std::string skeleton_path =
      WriteOzz(*BuildRawSkeleton(), "manifest_skeleton.ozz");
  const std::string clip_path = WriteOzz(*BuildRawAnimation(), "manifest_clip.ozz");

  const std::filesystem::path manifest_path = TempDir() / "manifest.json";
  {
    std::ofstream out(manifest_path);
    REQUIRE(out.good());
    out << R"({
      "_comment": "ignored",
      "skeleton": ")" << std::filesystem::path(skeleton_path).filename().string()
        << R"(",
      "clips": {
        "plain": ")" << std::filesystem::path(clip_path).filename().string()
        << R"(",
        "with_pivot": { "file": ")"
        << std::filesystem::path(clip_path).filename().string()
        << R"(", "pivot": 0.25 },
        "out_of_range": { "file": ")"
        << std::filesystem::path(clip_path).filename().string()
        << R"(", "pivot": 4.0 }
      }
    })";
  }

  std::optional<AnimationSet> set = AnimationSet::Load(manifest_path.string());
  REQUIRE(set.has_value());
  REQUIRE(set->clip_count() == 3);

  const int plain = set->FindClip("plain");
  const int with_pivot = set->FindClip("with_pivot");
  const int out_of_range = set->FindClip("out_of_range");
  REQUIRE(plain >= 0);
  REQUIRE(with_pivot >= 0);
  REQUIRE(out_of_range >= 0);

  // A bare filename stays valid, so adding a pivot to one clip does not force
  // rewriting the rest; absent, it defaults to "the whole clip is phase one".
  CHECK(set->clip_pivot(plain) == Catch::Approx(1.0f));
  CHECK(set->clip_pivot(with_pivot) == Catch::Approx(0.25f));
  // A nonsense authored value is clamped, not trusted into the sampler.
  CHECK(set->clip_pivot(out_of_range) == Catch::Approx(1.0f));

  // Manifest order is preserved, so clip indices mean what the file says.
  CHECK(set->clip_name(0) == "plain");
  CHECK(set->clip_name(1) == "with_pivot");
}

TEST_CASE("the emitter applies its world transform", "[animation]") {
  Fixture fixture = LoadFixture();
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);
  REQUIRE(sampler.Sample(fixture.clip, 0.0f, pose));
  REQUIRE(LocalToModel(fixture.skeleton, pose));

  const glm::mat4 world =
      glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, -50.0f));
  DebugLineBuffer lines;
  EmitSkeletonLines(fixture.skeleton, pose, world, lines);

  REQUIRE(lines.lines.size() == static_cast<size_t>(kFixtureJoints - kFixtureRoots));
  for (const DebugLine& line : lines.lines) {
    CHECK(line.start.x == Catch::Approx(100.0f).margin(1e-3));
    CHECK(line.start.z == Catch::Approx(-50.0f).margin(1e-3));
  }
}

// ---------------------------------------------------------------------------
// Markers and attachments.
//
// The manifests below are written in-test, so these assert against the loader's
// contract and never against assets/characters/.
// ---------------------------------------------------------------------------

namespace {

// Writes a manifest naming the fixture skeleton and one clip, with `extra`
// spliced in as additional top-level entries (a "sockets" array, a "family") and
// `clip_body` as the clip's own object body.
std::string WriteManifest(const std::string& filename, const std::string& extra,
                          const std::string& clip_body) {
  const std::string skeleton_file =
      std::filesystem::path(WriteOzz(*BuildRawSkeleton(), filename + "_skeleton.ozz"))
          .filename()
          .string();
  const std::string clip_file =
      std::filesystem::path(WriteOzz(*BuildRawAnimation(), filename + "_clip.ozz"))
          .filename()
          .string();

  const std::filesystem::path path = TempDir() / (filename + ".json");
  std::ofstream out(path);
  REQUIRE(out.good());
  out << "{\n  \"skeleton\": \"" << skeleton_file << "\",\n"
      << extra << "  \"clips\": { \"only\": { \"file\": \"" << clip_file << "\""
      << clip_body << " } }\n}\n";
  out.close();
  return path.string();
}

// A one-joint skeleton, so a Pose can be sized against something SMALLER than
// the fixture -- which is the only way to exercise the emitters' bounds guard.
Skeleton LoadTinySkeleton() {
  ozz::animation::offline::RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "lonely";
  raw.roots[0].transform = ozz::math::Transform::identity();
  REQUIRE(raw.Validate());
  ozz::animation::offline::SkeletonBuilder builder;
  std::optional<Skeleton> skeleton =
      Skeleton::Load(WriteOzz(*builder(raw), "tiny_skeleton.ozz"));
  REQUIRE(skeleton.has_value());
  return std::move(*skeleton);
}

// The fixture posed at `ratio`, with models() filled.
Pose PoseAt(const Fixture& fixture, float ratio) {
  Pose pose(fixture.skeleton);
  ClipSampler sampler;
  sampler.Reset(fixture.skeleton);
  REQUIRE(sampler.Sample(fixture.clip, ratio, pose));
  REQUIRE(LocalToModel(fixture.skeleton, pose));
  return pose;
}

}  // namespace

TEST_CASE("a clip carries free-form named markers", "[animation]") {
  const std::string manifest = WriteManifest(
      "markers", "", R"(, "markers": { "event": 0.5, "sound": 2.0 })");
  std::optional<AnimationSet> set = AnimationSet::Load(manifest);
  REQUIRE(set.has_value());

  CHECK(set->clip_marker(0, "event") == Catch::Approx(0.5f));
  // An authored value outside [0,1] is clamped, not trusted into a sampler.
  CHECK(set->clip_marker(0, "sound") == Catch::Approx(1.0f));
  // An undeclared marker is absent, which is distinguishable from one at 0.
  CHECK_FALSE(set->clip_marker(0, "load").has_value());
  // Authored order, which is what the debug panel lists.
  REQUIRE(set->clip_marker_count(0) == 2);
  CHECK(set->clip_marker_name(0, 0) == "event");
  CHECK(set->clip_marker_value(0, 1) == Catch::Approx(1.0f));
}

TEST_CASE("the legacy pivot field is the marker named pivot", "[animation]") {
  // The shipped Quaternius manifest authors this form, so it must keep working
  // with no edit -- that is what makes the marker table additive.
  const std::string manifest =
      WriteManifest("legacy_pivot", "", R"(, "pivot": 0.25)");
  std::optional<AnimationSet> set = AnimationSet::Load(manifest);
  REQUIRE(set.has_value());

  CHECK(set->clip_pivot(0) == Catch::Approx(0.25f));
  CHECK(set->clip_marker(0, "pivot") == Catch::Approx(0.25f));

  // Absent, the pivot still degrades to "the whole clip is phase one".
  const std::string bare = WriteManifest("bare_pivot", "", "");
  std::optional<AnimationSet> bare_set = AnimationSet::Load(bare);
  REQUIRE(bare_set.has_value());
  CHECK(bare_set->clip_pivot(0) == Catch::Approx(1.0f));
  CHECK_FALSE(bare_set->clip_marker(0, "pivot").has_value());
}

TEST_CASE("every joint is an attachment, and ids are joint indices",
          "[animation]") {
  // A manifest with no "sockets" is exactly the shipped Quaternius shape, so
  // this is also the graceful-degradation case.
  const std::string manifest = WriteManifest("joints_only", "", "");
  std::optional<AnimationSet> set = AnimationSet::Load(manifest);
  REQUIRE(set.has_value());

  REQUIRE(set->attachment_count() == kFixtureJoints);
  CHECK(set->family().empty());
  for (int i = 0; i < kFixtureJoints; ++i) {
    CHECK(set->attachment_name(i) == set->skeleton().joint_names()[i]);
    CHECK(set->FindAttachment(set->attachment_name(i)) == i);
  }
  CHECK(set->FindAttachment("no_such_thing") == -1);

  // The invariant that makes a joint attachment's transform provable: for a
  // POSED frame, not the rest pose.
  Fixture fixture = LoadFixture();
  const Pose pose = PoseAt(fixture, 0.5f);
  for (int i = 0; i < kFixtureJoints; ++i) {
    const glm::mat4 expected = ToMat4(pose.models()[static_cast<size_t>(i)]);
    const glm::mat4 actual = set->AttachmentTransform(i, pose);
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        CHECK(actual[c][r] == Catch::Approx(expected[c][r]).margin(1e-4));
      }
    }
  }
}

TEST_CASE("a socket composes parent-then-offset", "[animation]") {
  // The offset is a pure ROTATION and the parent a pure TRANSLATION, which is
  // what makes the two orderings distinguishable: parent*offset keeps the
  // parent's origin, offset*parent rotates it. With an identity rotation both
  // orderings agree and the reversal -- "the sword is attached but points the
  // wrong way" -- would sail through.
  const glm::mat4 offset =
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
  std::string sockets = "  \"family\": \"Fixture\",\n  \"sockets\": [ { \"name\": "
                        "\"hand_item\", \"parent\": \"child_a\", \"offset\": [";
  for (int i = 0; i < 16; ++i) {
    sockets += (i > 0 ? ", " : "") + std::to_string(offset[i / 4][i % 4]);
  }
  sockets += "] } ],\n";

  const std::string manifest = WriteManifest("socket_offset", sockets, "");
  std::optional<AnimationSet> set = AnimationSet::Load(manifest);
  REQUIRE(set.has_value());

  CHECK(set->family() == "Fixture");
  REQUIRE(set->attachment_count() == kFixtureJoints + 1);
  const int socket = set->FindAttachment("hand_item");
  REQUIRE(socket == kFixtureJoints);  // joints first, then sockets

  Fixture fixture = LoadFixture();
  const Pose pose = PoseAt(fixture, 0.5f);
  const int parent = set->FindAttachment("child_a");
  REQUIRE(parent >= 0);

  const glm::mat4 expected = ToMat4(pose.models()[static_cast<size_t>(parent)]) * offset;
  const glm::mat4 actual = set->AttachmentTransform(socket, pose);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      CHECK(actual[c][r] == Catch::Approx(expected[c][r]).margin(1e-4));
    }
  }

  // Spelled out, so a regression names the failure rather than showing a matrix
  // diff: the socket sits AT its parent and is turned by the offset.
  const glm::vec3 parent_origin(ToMat4(pose.models()[static_cast<size_t>(parent)])[3]);
  CHECK(glm::vec3(actual[3]).x == Catch::Approx(parent_origin.x).margin(1e-4));
  CHECK(glm::vec3(actual[3]).y == Catch::Approx(parent_origin.y).margin(1e-4));
  CHECK(glm::vec3(actual[0]).y == Catch::Approx(1.0f).margin(1e-4));  // X -> +Y

  // And it FOLLOWS the animation: the parent chain moves with the root, so the
  // same id at another ratio is somewhere else.
  const Pose start = PoseAt(fixture, 0.0f);
  CHECK(set->AttachmentTransform(socket, start)[3].x !=
        Catch::Approx(actual[3].x).margin(1e-3));
}

TEST_CASE("a socket colliding with a joint is dropped", "[animation]") {
  // `weapon_R` and `prop-weapon_R` are duplicates in the 0 A.D. corpus, a
  // millimetre apart. The joint is the animated one, so the joint wins -- and a
  // packer that failed to collapse them must not produce two "child_a"s here.
  const std::string sockets =
      "  \"sockets\": [ { \"name\": \"child_a\", \"parent\": \"root\" },\n"
      "                 { \"name\": \"orphan\", \"parent\": \"no_such_joint\" } ],\n";
  const std::string manifest = WriteManifest("socket_collision", sockets, "");
  std::optional<AnimationSet> set = AnimationSet::Load(manifest);
  REQUIRE(set.has_value());

  // Both sockets are rejected: one collides, one names a parent that is not a
  // joint. Neither failure costs the rest of the manifest.
  CHECK(set->attachment_count() == kFixtureJoints);
  CHECK(set->FindAttachment("orphan") == -1);

  // "child_a" still resolves -- to the JOINT, whose offset is identity.
  const int child_a = set->FindAttachment("child_a");
  REQUIRE(child_a >= 0);
  CHECK(child_a < kFixtureJoints);

  Fixture fixture = LoadFixture();
  const Pose pose = PoseAt(fixture, 0.5f);
  const glm::mat4 expected = ToMat4(pose.models()[static_cast<size_t>(child_a)]);
  CHECK(set->AttachmentTransform(child_a, pose)[3].y ==
        Catch::Approx(expected[3].y).margin(1e-4));
}

TEST_CASE("the attachment emitter appends three lines per attachment",
          "[animation]") {
  const std::string manifest = WriteManifest("attachment_axes", "", "");
  std::optional<AnimationSet> set = AnimationSet::Load(manifest);
  REQUIRE(set.has_value());

  Fixture fixture = LoadFixture();
  const Pose pose = PoseAt(fixture, 0.5f);

  const glm::mat4 world =
      glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, -50.0f));
  DebugLineBuffer lines;
  EmitAttachmentAxes(*set, pose, world, lines);

  const size_t expected = static_cast<size_t>(3 * set->attachment_count());
  REQUIRE(lines.lines.size() == expected);
  // The world transform reaches the triads, not only the bones.
  for (const DebugLine& line : lines.lines) {
    CHECK(line.start.z == Catch::Approx(-50.0f).margin(1e-3));
  }

  // Appending is additive and clears nothing, matching EmitSkeletonLines -- one
  // frame's debug lines come from several emitters sharing one buffer.
  EmitAttachmentAxes(*set, pose, world, lines);
  CHECK(lines.lines.size() == 2 * expected);

  // A pose sized against another skeleton appends nothing rather than reading
  // past the joint matrices.
  const Skeleton tiny = LoadTinySkeleton();
  Pose undersized(tiny);
  DebugLineBuffer none;
  EmitAttachmentAxes(*set, undersized, world, none);
  CHECK(none.lines.empty());
}
