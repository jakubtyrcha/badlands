// Task S2.C: procedural geometry (primitives, rock ring-extrusion, building
// parts) + the building visual catalog + ploppable footprint rings.
//
// Pure CPU generators — no GPU/Dawn init needed, even though
// StaticTexturedMeshComponent (the mesh type these all return, wrapped in
// TexturedMeshResult) carries wgpu::TextureView/Sampler members; they're
// simply never touched here.

#include "badlands_game.h"
#include "core/roof_shape.hpp"
#include "engine/rendering/geometry/building_parts_builder.hpp"
#include "engine/rendering/geometry/extrusion_mesh_builder.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "game/building_catalog.h"
#include "game/geometry/ploppable_rings.h"
#include "game/material_id.hpp"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

using namespace badlands;

namespace {

// ---- generic per-generator sanity -----------------------------------------

// vertex_count*11 == vertices.size(), non-empty, and every tangent is
// ~unit-length and ~perpendicular to its normal (dot < 0.1).
void check_well_formed(const TexturedMeshResult& result, const char* label) {
  INFO(label);
  const auto& verts = result.mesh.vertices;
  REQUIRE(result.mesh.vertex_count > 0);
  REQUIRE(verts.size() == static_cast<size_t>(result.mesh.vertex_count) *
                             kTexturedMeshFloatsPerVertex);

  for (size_t i = 0; i < verts.size(); i += kTexturedMeshFloatsPerVertex) {
    glm::vec3 normal(verts[i + 5], verts[i + 6], verts[i + 7]);
    glm::vec3 tangent(verts[i + 8], verts[i + 9], verts[i + 10]);
    CAPTURE(i / kTexturedMeshFloatsPerVertex);
    CHECK(glm::length(normal) == Catch::Approx(1.0f).margin(0.01));
    CHECK(glm::length(tangent) == Catch::Approx(1.0f).margin(0.01));
    CHECK(std::abs(glm::dot(normal, tangent)) < 0.1f);
  }

  // Triangle WINDING must agree with the shading normals. Under the deferred
  // material's default CullMode::Back / FrontFace::CCW, a triangle is drawn
  // only if its geometric normal cross(v1-v0, v2-v0) faces the camera; if that
  // winding normal opposes the (correct) outward shading normal, the outer
  // surface is back-face-culled and the mesh renders inside-out even though the
  // shading normal looks right. (Reversed cone/cylinder/extrusion winding
  // shipped past two reviews precisely because only shading normals were
  // checked — this guards every generator against that class of bug.)
  REQUIRE(result.mesh.vertex_count % 3 == 0);
  for (size_t t = 0; t + 3 * kTexturedMeshFloatsPerVertex <= verts.size();
       t += 3 * kTexturedMeshFloatsPerVertex) {
    auto at = [&](int k) {
      size_t o = t + static_cast<size_t>(k) * kTexturedMeshFloatsPerVertex;
      return std::pair<glm::vec3, glm::vec3>(
          glm::vec3(verts[o], verts[o + 1], verts[o + 2]),
          glm::vec3(verts[o + 5], verts[o + 6], verts[o + 7]));
    };
    auto [p0, n0] = at(0);
    auto [p1, n1] = at(1);
    auto [p2, n2] = at(2);
    // Skip degenerate/collapsed triangles (e.g. a capsule/sphere's pole
    // triangles, where one edge shrinks to ~0): their geometric normal is
    // ill-defined and their ~zero area makes them irrelevant to culling. A
    // genuine winding reversal is a strongly negative dot, not float noise.
    float min_edge = std::min({glm::length(p1 - p0), glm::length(p2 - p0),
                               glm::length(p2 - p1)});
    if (min_edge < 1e-4f) continue;
    glm::vec3 geo = glm::cross(p1 - p0, p2 - p0);
    CAPTURE(t / (3 * kTexturedMeshFloatsPerVertex));
    CHECK(glm::dot(geo, n0 + n1 + n2) > 0.0f);
  }
}

// True if any vertex's normal is within `margin` of `dir` (unit vector) —
// used to confirm a cap (e.g. +Y / -Y) is present in a mesh.
bool has_normal_near(const TexturedMeshResult& result, glm::vec3 dir, float margin = 0.05f) {
  const auto& verts = result.mesh.vertices;
  for (size_t i = 0; i < verts.size(); i += kTexturedMeshFloatsPerVertex) {
    glm::vec3 normal(verts[i + 5], verts[i + 6], verts[i + 7]);
    if (glm::length(normal - dir) < margin) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---- primitives -------------------------------------------------------

TEST_CASE("GenerateCube: well-formed, 24 unique face-corners, sane bounds") {
  auto result = GenerateCube(glm::vec3(1.0f, 2.0f, 3.0f));
  check_well_formed(result, "cube");

  // 2 triangles/face x 6 faces = 12 triangles = 36 non-indexed vertices, but
  // only 4 unique (position, normal) corner combinations per face (24 total)
  // — the 6th/2nd corner of each face's two triangles repeats.
  CHECK(result.mesh.vertex_count == 36);
  std::set<std::tuple<int, int, int, int, int, int>> unique_corners;
  const auto& verts = result.mesh.vertices;
  for (size_t i = 0; i < verts.size(); i += kTexturedMeshFloatsPerVertex) {
    auto q = [](float f) { return static_cast<int>(std::lround(f * 1000.0f)); };
    unique_corners.insert({q(verts[i]), q(verts[i + 1]), q(verts[i + 2]), q(verts[i + 5]),
                          q(verts[i + 6]), q(verts[i + 7])});
  }
  CHECK(unique_corners.size() == 24);

  CHECK(result.local_bounds.min == glm::vec3(-1.0f, -2.0f, -3.0f));
  CHECK(result.local_bounds.max == glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("GenerateCylinder: well-formed, has top+bottom caps, sane bounds") {
  auto result = GenerateCylinder(1.5f, 2.0f, 16);
  check_well_formed(result, "cylinder");
  CHECK(has_normal_near(result, glm::vec3(0, 1, 0)));
  CHECK(has_normal_near(result, glm::vec3(0, -1, 0)));
  CHECK(result.local_bounds.min == glm::vec3(-1.5f, 0.0f, -1.5f));
  CHECK(result.local_bounds.max == glm::vec3(1.5f, 2.0f, 1.5f));
}

TEST_CASE("GenerateCone: well-formed, has base cap, sane bounds") {
  auto result = GenerateCone(1.0f, 2.5f, 16);
  check_well_formed(result, "cone");
  CHECK(has_normal_near(result, glm::vec3(0, -1, 0)));
  CHECK(result.local_bounds.min == glm::vec3(-1.0f, 0.0f, -1.0f));
  CHECK(result.local_bounds.max == glm::vec3(1.0f, 2.5f, 1.0f));

  // Regression: side normals must point outward (away from the cone axis),
  // not inward. Sample every side vertex (skip the base-cap triangles, whose
  // normal is (0,-1,0)) and check the normal has a positive outward radial
  // component and a positive Y component.
  const auto& verts = result.mesh.vertices;
  int side_vertices_checked = 0;
  for (size_t i = 0; i < verts.size(); i += kTexturedMeshFloatsPerVertex) {
    glm::vec3 pos(verts[i], verts[i + 1], verts[i + 2]);
    glm::vec3 normal(verts[i + 5], verts[i + 6], verts[i + 7]);
    if (normal.y < -0.5f) continue;  // base-cap vertex, skip.

    glm::vec3 radial = glm::vec3(pos.x, 0.0f, pos.z);
    if (glm::length(radial) < 1e-4f) continue;  // apex vertex, no radial dir.
    glm::vec3 outward = glm::normalize(radial);

    CAPTURE(i / kTexturedMeshFloatsPerVertex);
    CHECK(glm::dot(normal, outward) > 0.0f);
    CHECK(normal.y > 0.0f);
    ++side_vertices_checked;
  }
  CHECK(side_vertices_checked > 0);
}

TEST_CASE("GenerateGableRoof: well-formed, ridge + base bounds") {
  auto result = GenerateGableRoof(glm::vec3(4.0f, 1.5f, 3.0f));
  check_well_formed(result, "gable roof");
  CHECK(result.local_bounds.min == glm::vec3(-2.0f, 0.0f, -1.5f));
  CHECK(result.local_bounds.max == glm::vec3(2.0f, 1.5f, 1.5f));
}

TEST_CASE("GenerateCapsule: well-formed and spans the expected height") {
  const float radius = 0.5f;
  const float cylinder_height = 1.0f;
  auto result = GenerateCapsule(radius, cylinder_height, 20);
  check_well_formed(result, "capsule");

  float expected_height = 2.0f * radius + cylinder_height;
  CHECK(result.local_bounds.min.y == Catch::Approx(0.0f).margin(1e-4));
  CHECK(result.local_bounds.max.y == Catch::Approx(expected_height).margin(1e-4));
  CHECK(result.local_bounds.min.x == Catch::Approx(-radius).margin(1e-4));
  CHECK(result.local_bounds.max.x == Catch::Approx(radius).margin(1e-4));
}

// ---- rock ring-extrusion -----------------------------------------------

namespace {
std::vector<glm::vec2> square(float r) {
  return {{-r, -r}, {r, -r}, {r, r}, {-r, r}};
}
}  // namespace

TEST_CASE("BuildExtrusionMesh: mesa over a square ring") {
  auto ring = square(2.0f);
  auto result = BuildExtrusionMesh(ring, 0.0f, 2.0f, 0.85f);
  check_well_formed(result, "extrusion mesa");

  // 4 top-fan triangles (12 verts) + 4 wall quads (24 verts) = 36 verts,
  // matching the reference build_extrusion test (minus its `layer` float).
  CHECK(result.mesh.vertex_count == 36);

  // Top-face normals point up regardless of mesa/basin.
  CHECK(has_normal_near(result, glm::vec3(0, 1, 0)));

  // Mesa walls point outward: sample the first wall vertex's normal (index
  // 12, right after the 12 top-fan vertices) and confirm it points away from
  // the ring centroid (origin, for a centered square).
  const auto& verts = result.mesh.vertices;
  size_t wall_start = 12 * kTexturedMeshFloatsPerVertex;
  glm::vec3 wall_normal(verts[wall_start + 5], verts[wall_start + 6], verts[wall_start + 7]);
  glm::vec3 wall_pos(verts[wall_start], verts[wall_start + 1], verts[wall_start + 2]);
  CHECK(glm::dot(glm::vec2(wall_normal.x, wall_normal.z), glm::vec2(wall_pos.x, wall_pos.z)) > 0.0f);
}

TEST_CASE("BuildExtrusionMesh: basin (delta_y < 0) also has upward top-face normals") {
  auto ring = square(3.0f);
  auto result = BuildExtrusionMesh(ring, 0.0f, -1.5f, 0.9f);
  check_well_formed(result, "extrusion basin");
  CHECK(has_normal_near(result, glm::vec3(0, 1, 0)));
  CHECK(result.local_bounds.min.y == Catch::Approx(-1.5f).margin(1e-4));
}

TEST_CASE("BuildExtrusionMesh: degenerate ring (<3 points) is empty") {
  auto result = BuildExtrusionMesh({{0.0f, 0.0f}, {1.0f, 0.0f}}, 0.0f, 1.0f, 0.85f);
  CHECK(result.mesh.vertex_count == 0);
  CHECK(result.mesh.vertices.empty());
}

// ---- building parts -----------------------------------------------------

TEST_CASE("BuildBuildingParts: None roof is walls only") {
  auto parts = BuildBuildingParts(3.0f, 2.0f, 4.0f, RoofShape::None);
  REQUIRE(parts.size() == 1);
  CHECK(parts[0].kind == BuildingPartKind::Wall);
  check_well_formed(parts[0].mesh, "wall (None roof)");
  CHECK(parts[0].mesh.local_bounds.min == glm::vec3(-1.5f, 0.0f, -1.0f));
  CHECK(parts[0].mesh.local_bounds.max == glm::vec3(1.5f, 4.0f, 1.0f));
}

TEST_CASE("BuildBuildingParts: Gable roof adds one Roof part atop the walls") {
  auto parts = BuildBuildingParts(3.0f, 2.0f, 4.0f, RoofShape::Gable);
  REQUIRE(parts.size() == 2);
  CHECK(parts[0].kind == BuildingPartKind::Wall);
  CHECK(parts[1].kind == BuildingPartKind::Roof);
  check_well_formed(parts[1].mesh, "roof");
  CHECK(parts[1].mesh.local_bounds.min.y == Catch::Approx(4.0f).margin(1e-4));
  CHECK(parts[1].mesh.local_bounds.max.y > 4.0f);
}

TEST_CASE("BuildBuildingParts: CornerTowers adds four Tower parts near the footprint corners") {
  float width = 4.0f, depth = 4.0f, height = 5.0f;
  auto parts = BuildBuildingParts(width, depth, height, RoofShape::CornerTowers);
  REQUIRE(parts.size() == 5);
  CHECK(parts[0].kind == BuildingPartKind::Wall);

  float hx = width * 0.5f, hz = depth * 0.5f;
  glm::vec2 expected_corners[4] = {{-hx, -hz}, {hx, -hz}, {hx, hz}, {-hx, hz}};
  for (int i = 0; i < 4; ++i) {
    const BuildingPart& tower = parts[1 + i];
    CHECK(tower.kind == BuildingPartKind::Tower);
    check_well_formed(tower.mesh, "tower");
    glm::vec3 center = tower.mesh.local_bounds.Center();
    CHECK(center.x == Catch::Approx(expected_corners[i].x).margin(0.1));
    CHECK(center.z == Catch::Approx(expected_corners[i].y).margin(0.1));
    CHECK(tower.mesh.local_bounds.min.y == Catch::Approx(0.0f).margin(1e-3));
  }
}

// ---- building visual catalog ---------------------------------------------

TEST_CASE("building_visual: expected material/roof per kind") {
  {
    BuildingVisual v = building_visual(GAME_BUILDING_CASTLE);
    CHECK(v.height == 5.0f);
    CHECK(v.roof == RoofShape::CornerTowers);
    CHECK(v.wall_material == MaterialId::RockWall);
    CHECK(v.roof_material == MaterialId::RockWall);
  }
  {
    BuildingVisual v = building_visual(GAME_BUILDING_TAVERN);
    CHECK(v.height == 2.2f);
    CHECK(v.roof == RoofShape::Gable);
    CHECK(v.wall_material == MaterialId::Planks);
    CHECK(v.roof_material == MaterialId::RoofSlates);
  }
  {
    BuildingVisual v = building_visual(GAME_BUILDING_SCRIPTORIUM);
    CHECK(v.roof == RoofShape::Gable);
    CHECK(v.wall_material == MaterialId::Plaster);
  }
  {
    BuildingVisual v = building_visual(GAME_BUILDING_SEWER);
    CHECK(v.height == 0.4f);
    CHECK(v.roof == RoofShape::None);
    CHECK(v.wall_material == MaterialId::RockWall);
  }
}

// ---- ploppable footprint rings -------------------------------------------

TEST_CASE("ploppable_local_ring: footprints and rotation") {
  CHECK(ploppable_local_ring(GamePloppableKind::Tree, 0).empty());

  auto rock_a = ploppable_local_ring(GamePloppableKind::RockA, 0);
  REQUIRE(rock_a.size() == 8);
  for (auto& p : rock_a) {
    CHECK(glm::length(p) == Catch::Approx(2.0f).margin(1e-3));
  }

  // A rectangle footprint rotated 90 degrees (rot=2, 2*45deg) swaps its
  // bounding half-extents.
  auto rock_c = ploppable_local_ring(GamePloppableKind::RockC, 0);
  REQUIRE(rock_c.size() == 4);
  float minx = rock_c[0].x, maxx = rock_c[0].x, minz = rock_c[0].y, maxz = rock_c[0].y;
  for (auto& p : rock_c) {
    minx = std::min(minx, p.x);
    maxx = std::max(maxx, p.x);
    minz = std::min(minz, p.y);
    maxz = std::max(maxz, p.y);
  }
  CHECK((maxx - minx) == Catch::Approx(7.0f).margin(1e-3));   // 2 * 3.5
  CHECK((maxz - minz) == Catch::Approx(3.6f).margin(1e-3));   // 2 * 1.8

  auto rock_c_rot90 = ploppable_local_ring(GamePloppableKind::RockC, 2);
  float rminx = rock_c_rot90[0].x, rmaxx = rock_c_rot90[0].x, rminz = rock_c_rot90[0].y,
       rmaxz = rock_c_rot90[0].y;
  for (auto& p : rock_c_rot90) {
    rminx = std::min(rminx, p.x);
    rmaxx = std::max(rmaxx, p.x);
    rminz = std::min(rminz, p.y);
    rmaxz = std::max(rmaxz, p.y);
  }
  CHECK((rmaxx - rminx) == Catch::Approx(3.6f).margin(1e-2));  // swapped
  CHECK((rmaxz - rminz) == Catch::Approx(7.0f).margin(1e-2));
}
