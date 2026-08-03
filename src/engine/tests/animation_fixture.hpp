#pragma once

// Test-only fixture builders for the animation suites: a tiny skeleton and clip
// BUILT IN-TEST from ozz's offline builders and written to temp .ozz files, so
// nothing is committed and no suite asserts against shipped data files.
//
// Shared by src/engine/tests/animation_tests.cpp (the runtime) and
// src/game/tests/skeleton_debug_overlay_tests.cpp (the overlay), which need the
// same fixture for different reasons.

#include <filesystem>
#include <string>

#include <catch_amalgamated.hpp>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

namespace badlands::test {

// The fixture hierarchy: one root with two children, so there are 3 joints,
// 1 root, and therefore exactly 2 bone segments to draw.
inline constexpr int kFixtureJoints = 3;
inline constexpr int kFixtureRoots = 1;
inline constexpr float kFixtureChildOffsetY = 2.0f;

inline std::filesystem::path TempDir() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "badlands_animation_tests";
  std::filesystem::create_directories(dir);
  return dir;
}

// root at origin; child_a offset +Y; child_b offset -Y.
inline ozz::unique_ptr<ozz::animation::Skeleton> BuildRawSkeleton() {
  ozz::animation::offline::RawSkeleton raw;
  raw.roots.resize(1);

  ozz::animation::offline::RawSkeleton::Joint& root = raw.roots[0];
  root.name = "root";
  root.transform = ozz::math::Transform::identity();
  root.children.resize(2);

  root.children[0].name = "child_a";
  root.children[0].transform = ozz::math::Transform::identity();
  root.children[0].transform.translation =
      ozz::math::Float3(0.0f, kFixtureChildOffsetY, 0.0f);

  root.children[1].name = "child_b";
  root.children[1].transform = ozz::math::Transform::identity();
  root.children[1].transform.translation =
      ozz::math::Float3(0.0f, -kFixtureChildOffsetY, 0.0f);

  REQUIRE(raw.Validate());
  ozz::animation::offline::SkeletonBuilder builder;
  return builder(raw);
}

// A clip that slides the ROOT from x=0 to x=10 over `duration`, leaving the
// children at their rest offsets. Linear, so the value at a ratio is exactly
// 10*ratio -- which is what makes the sampling assertions unambiguous.
inline ozz::unique_ptr<ozz::animation::Animation> BuildRawAnimation(
    float duration = 1.0f) {
  ozz::animation::offline::RawAnimation raw;
  raw.duration = duration;
  raw.tracks.resize(kFixtureJoints);

  raw.tracks[0].translations.push_back({0.0f, ozz::math::Float3(0.0f, 0.0f, 0.0f)});
  raw.tracks[0].translations.push_back({duration, ozz::math::Float3(10.0f, 0.0f, 0.0f)});

  raw.tracks[1].translations.push_back(
      {0.0f, ozz::math::Float3(0.0f, kFixtureChildOffsetY, 0.0f)});
  raw.tracks[2].translations.push_back(
      {0.0f, ozz::math::Float3(0.0f, -kFixtureChildOffsetY, 0.0f)});

  REQUIRE(raw.Validate());
  ozz::animation::offline::AnimationBuilder builder;
  return builder(raw);
}

template <typename T>
std::string WriteOzz(const T& object, const std::string& filename) {
  const std::string path = (TempDir() / filename).string();
  {
    ozz::io::File file(path.c_str(), "wb");
    REQUIRE(file.opened());
    ozz::io::OArchive archive(&file);
    archive << object;
  }
  return path;
}

}  // namespace badlands::test
