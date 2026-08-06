// Tests for the USD -> engine mesh conversion
// (engine/rendering/geometry/usd_mesh_adapter.hpp).
//
// Every input here is hand-built, not loaded: a two-triangle quad with known
// positions and attributes, so the axis convention, unit scaling and interleave
// stride are each checked against arithmetic rather than against whatever a
// particular asset happens to contain. The real .usdc files are covered
// separately by badlands_usd_tests.

#include <catch_amalgamated.hpp>

#include <cmath>

#include "engine/rendering/geometry/usd_mesh_adapter.hpp"

using badlands::BuildImportedModels;
using badlands::UsdMaterialBinding;
using badlands::UsdMeshData;
using badlands::UsdSceneData;

namespace {

// A unit quad in the XY plane (USD's Z-up ground plane), 4 verts / 2 tris,
// with +Z normals, +X tangents and corner UVs.
UsdMeshData MakeQuad() {
  UsdMeshData mesh;
  mesh.name = "quad";
  mesh.material_name = "quad_mat";
  mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  // xyzw: +X tangent, right-handed (w = +1) on every vertex.
  mesh.tangents = {1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                   1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  mesh.uvs = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

UsdSceneData MakeScene(UsdMeshData mesh, bool z_up, float meters_per_unit) {
  UsdSceneData scene;
  scene.meshes.push_back(std::move(mesh));
  scene.z_up = z_up;
  scene.meters_per_unit = meters_per_unit;
  scene.ok = true;
  return scene;
}

UsdMaterialBinding DefaultBinding() {
  UsdMaterialBinding binding;
  binding.default_pack_dir = "assets/models/quad_1k";
  return binding;
}

constexpr size_t kStride = badlands::kTexturedMeshFloatsPerVertex;

}  // namespace

TEST_CASE("a Y-up scene passes positions through unrotated", "[usd][adapter]") {
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), false, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  REQUIRE(v.size() == 4 * kStride);
  // Vertex 2 was (1, 1, 0).
  CHECK(v[2 * kStride + 0] == Catch::Approx(1.0f));
  CHECK(v[2 * kStride + 1] == Catch::Approx(1.0f));
  CHECK(v[2 * kStride + 2] == Catch::Approx(0.0f));
}

TEST_CASE("a Z-up scene maps (x,y,z) to (x,z,-y)", "[usd][adapter]") {
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), true, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  // Vertex 2 was (1, 1, 0) in USD space -> (1, 0, -1) in engine space.
  CHECK(v[2 * kStride + 0] == Catch::Approx(1.0f));
  CHECK(v[2 * kStride + 1] == Catch::Approx(0.0f));
  CHECK(v[2 * kStride + 2] == Catch::Approx(-1.0f));

  // The quad lay in USD's XY ground plane, so its +Z normal must come out as
  // the engine's +Y (up). This is the check that catches a wrong-sign rotation:
  // a mesh can look plausible while being lit from underneath.
  CHECK(v[2 * kStride + 5] == Catch::Approx(0.0f));
  CHECK(v[2 * kStride + 6] == Catch::Approx(1.0f));
  CHECK(v[2 * kStride + 7] == Catch::Approx(0.0f));
}

TEST_CASE("the axis change preserves winding, so indices are untouched",
          "[usd][adapter]") {
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), true, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);
  // A mirrored basis would need the winding flipped to stay front-facing; a
  // rotation does not. Pinning this stops a future "fix" from reversing them.
  CHECK(models[0].mesh.mesh.indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
}

TEST_CASE("metersPerUnit scales positions but not directions",
          "[usd][adapter]") {
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), false, 0.01f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  // Vertex 2 was (1, 1, 0) in centimetres -> (0.01, 0.01, 0) metres.
  CHECK(v[2 * kStride + 0] == Catch::Approx(0.01f));
  CHECK(v[2 * kStride + 1] == Catch::Approx(0.01f));
  // The normal must stay unit length: scaling it would darken the surface in
  // proportion to the file's unit choice.
  CHECK(v[2 * kStride + 5] == Catch::Approx(0.0f));
  CHECK(v[2 * kStride + 6] == Catch::Approx(0.0f));
  CHECK(v[2 * kStride + 7] == Catch::Approx(1.0f));
}

TEST_CASE("UVs land in the right slots of the interleave", "[usd][adapter]") {
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), false, 1.0f), DefaultBinding());
  const auto& v = models[0].mesh.mesh.vertices;
  // Vertex 1's UV was (1, 0) in USD, at floats 3 and 4 of its 11. V is flipped
  // on import (see the next test), so it arrives as (1, 1).
  CHECK(v[1 * kStride + 3] == Catch::Approx(1.0f));
  CHECK(v[1 * kStride + 4] == Catch::Approx(1.0f));
}

TEST_CASE("V is flipped from USD's lower-left origin to the sampler's upper-left",
          "[usd][adapter]") {
  // USD's `st` puts (0,0) at the texture's LOWER-left; WebGPU samples with row
  // 0 at the TOP. Passing V through unflipped mirrors every model vertically.
  //
  // That is not a subtle wrongness on atlased models: rock_moss_set_01's
  // diffuse is a 3x3 atlas whose bottom third is pure black and whose six
  // meshes all live in v >= 1/3, so the unflipped read sampled the black band
  // and the rocks rendered as black blobs.
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), false, 1.0f), DefaultBinding());
  const auto& v = models[0].mesh.mesh.vertices;

  // U is untouched; only V mirrors.
  CHECK(v[0 * kStride + 3] == Catch::Approx(0.0f));  // vertex 0 was (0, 0)
  CHECK(v[0 * kStride + 4] == Catch::Approx(1.0f));
  CHECK(v[2 * kStride + 3] == Catch::Approx(1.0f));  // vertex 2 was (1, 1)
  CHECK(v[2 * kStride + 4] == Catch::Approx(0.0f));
}

TEST_CASE("local bounds cover the converted geometry", "[usd][adapter]") {
  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), true, 1.0f), DefaultBinding());
  const auto& bounds = models[0].mesh.local_bounds;
  // Post-rotation the quad spans x [0,1], y 0, z [-1,0].
  CHECK(bounds.min.x == Catch::Approx(0.0f));
  CHECK(bounds.max.x == Catch::Approx(1.0f));
  CHECK(bounds.min.z == Catch::Approx(-1.0f));
  CHECK(bounds.max.z == Catch::Approx(0.0f));
}

TEST_CASE("the pack directory comes from the binding", "[usd][adapter]") {
  UsdMaterialBinding binding = DefaultBinding();
  binding.by_material["quad_mat"] = "assets/materials/override_pack";

  const auto models =
      BuildImportedModels(MakeScene(MakeQuad(), false, 1.0f), binding);
  CHECK(models[0].pack_dir == "assets/materials/override_pack");
}

TEST_CASE("a mesh missing an attribute is skipped, not zero-filled",
          "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  mesh.tangents.clear();

  const auto models =
      BuildImportedModels(MakeScene(std::move(mesh), false, 1.0f),
                          DefaultBinding());
  // Emitting it with zeroed tangents would render as a black surface with
  // nothing logged anywhere -- far harder to chase than an absent model.
  CHECK(models.empty());
}

TEST_CASE("the prim's world transform places the mesh", "[usd][adapter]") {
  // Mesh points are in each prim's OWN local space; placement lives in the
  // node hierarchy. Dropping it collapses every part of a model onto the
  // origin -- which is what put a treasure chest's lid inside its body.
  UsdMeshData mesh = MakeQuad();
  // Column-major (glm): translation occupies elements 12..14.
  mesh.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5.0f, 6.0f, 7.0f, 1};

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  // Vertex 2 was (1, 1, 0) -> translated to (6, 7, 7).
  CHECK(v[2 * kStride + 0] == Catch::Approx(6.0f));
  CHECK(v[2 * kStride + 1] == Catch::Approx(7.0f));
  CHECK(v[2 * kStride + 2] == Catch::Approx(7.0f));
  // A pure translation must leave the normal alone.
  CHECK(v[2 * kStride + 5] == Catch::Approx(0.0f));
  CHECK(v[2 * kStride + 6] == Catch::Approx(0.0f));
  CHECK(v[2 * kStride + 7] == Catch::Approx(1.0f));
}

TEST_CASE("the transform rotates normals, and scale does not shrink them",
          "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  // Uniform scale of 2 with no rotation. Positions must scale; normals must
  // stay unit length, or the surface darkens in proportion to the scale.
  mesh.transform = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1};

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  const auto& v = models[0].mesh.mesh.vertices;

  CHECK(v[2 * kStride + 0] == Catch::Approx(2.0f));
  CHECK(v[2 * kStride + 1] == Catch::Approx(2.0f));

  const float nx = v[2 * kStride + 5], ny = v[2 * kStride + 6],
              nz = v[2 * kStride + 7];
  CHECK(std::sqrt(nx * nx + ny * ny + nz * nz) == Catch::Approx(1.0f));
}

TEST_CASE("leftHanded orientation flips winding", "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  mesh.right_handed = false;

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  // Without the flip the mesh keeps reversed winding and vanishes under
  // backface culling, with nothing logged.
  CHECK(models[0].mesh.mesh.indices == std::vector<uint32_t>{0, 2, 1, 0, 3, 2});
}

TEST_CASE("a mirroring transform and leftHanded cancel out", "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  mesh.right_handed = false;
  // Negative determinant (mirror on X) -- reverses winding a second time.
  mesh.transform = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  CHECK(models[0].mesh.mesh.indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
}

TEST_CASE("a degenerate tangent yields a finite vector, never NaN",
          "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  // Tydra's solver emits a zero tangent for any triangle with zero UV area.
  // Vertex 1's xyz, leaving its w intact.
  mesh.tangents[4] = 0.0f;
  mesh.tangents[5] = 0.0f;
  mesh.tangents[6] = 0.0f;

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  // A NaN here poisons the Gram-Schmidt frame in gbuffer_encode.wesl and
  // renders as garbage with no diagnostic.
  for (size_t c = 8; c < 11; ++c) {
    CHECK(std::isfinite(v[1 * kStride + c]));
  }
}

TEST_CASE("a degenerate tangent survives the shader's Gram-Schmidt",
          "[usd][adapter]") {
  // Finite is NOT sufficient. gbuffer_encode.wesl re-orthogonalises with
  // normalize(T - N * dot(T, N)), so a fallback tangent PARALLEL to the normal
  // is finite on the CPU and NaN on the GPU. This reproduces that: a quad whose
  // normal points along +X, which a constant (1,0,0) fallback would match
  // exactly.
  UsdMeshData mesh = MakeQuad();
  for (size_t i = 0; i < 4; ++i) {
    mesh.normals[i * 3 + 0] = 1.0f;  // N = +X
    mesh.normals[i * 3 + 1] = 0.0f;
    mesh.normals[i * 3 + 2] = 0.0f;
    mesh.tangents[i * 4 + 0] = 0.0f;  // degenerate tangent
    mesh.tangents[i * 4 + 1] = 0.0f;
    mesh.tangents[i * 4 + 2] = 0.0f;
  }

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  for (size_t i = 0; i < 4; ++i) {
    const glm::vec3 n{v[i * kStride + 5], v[i * kStride + 6], v[i * kStride + 7]};
    const glm::vec3 t{v[i * kStride + 8], v[i * kStride + 9], v[i * kStride + 10]};
    CAPTURE(i, t.x, t.y, t.z);
    // The shader's own expression, evaluated on the CPU.
    const glm::vec3 orthogonalized = t - n * glm::dot(t, n);
    REQUIRE(glm::length(orthogonalized) > 1e-3f);
  }
}

TEST_CASE("tangent handedness reaches the vertex buffer", "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  mesh.tangents[3] = -1.0f;  // vertex 0's w: a mirrored UV island

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  REQUIRE(models.size() == 1);

  const auto& v = models[0].mesh.mesh.vertices;
  // Float 11 is the handedness the shader multiplies into cross(N, T). This is
  // the whole reason the vertex is 12 floats rather than 11 -- dropping it
  // inverted the normal map's V response on mirrored islands.
  CHECK(v[0 * kStride + 11] == Catch::Approx(-1.0f));
  CHECK(v[1 * kStride + 11] == Catch::Approx(1.0f));
}

TEST_CASE("a mirroring transform flips tangent handedness", "[usd][adapter]") {
  UsdMeshData mesh = MakeQuad();
  mesh.transform = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  const auto models = BuildImportedModels(
      MakeScene(std::move(mesh), false, 1.0f), DefaultBinding());
  const auto& v = models[0].mesh.mesh.vertices;
  // A mirror reverses the frame's chirality, so a right-handed source basis
  // becomes left-handed once placed.
  CHECK(v[0 * kStride + 11] == Catch::Approx(-1.0f));
}

TEST_CASE("a failed load converts to nothing", "[usd][adapter]") {
  UsdSceneData scene = MakeScene(MakeQuad(), false, 1.0f);
  scene.ok = false;
  CHECK(BuildImportedModels(scene, DefaultBinding()).empty());
}
