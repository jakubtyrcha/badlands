// Tests for the USD -> engine mesh conversion
// (engine/rendering/geometry/usd_mesh_adapter.hpp).
//
// Every input here is hand-built, not loaded: a two-triangle quad with known
// positions and attributes, so the axis convention, unit scaling and interleave
// stride are each checked against arithmetic rather than against whatever a
// particular asset happens to contain. The real .usdc files are covered
// separately by badlands_usd_tests.

#include <catch_amalgamated.hpp>

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
  mesh.tangents = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                   1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
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
  // Vertex 1's UV was (1, 0), at floats 3 and 4 of its 11.
  CHECK(v[1 * kStride + 3] == Catch::Approx(1.0f));
  CHECK(v[1 * kStride + 4] == Catch::Approx(0.0f));
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

TEST_CASE("a failed load converts to nothing", "[usd][adapter]") {
  UsdSceneData scene = MakeScene(MakeQuad(), false, 1.0f);
  scene.ok = false;
  CHECK(BuildImportedModels(scene, DefaultBinding()).empty());
}
