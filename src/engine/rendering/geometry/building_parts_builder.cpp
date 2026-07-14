#include "engine/rendering/geometry/building_parts_builder.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/geometry/mesh_builder_utils.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"

namespace badlands {

namespace {

// Scale factors matching the reference per-building assembly (renderer.rs @
// 8ee93cc): the gable roof footprint is 5% larger than the walls and a fixed
// 0.9 world units tall; corner towers are a fixed 0.6-unit-diameter cylinder
// scaled to 1.3x the wall height, capped by a cone 1.15x wider and 1.3x
// taller (relative to the tower radius) than the cylinder.
constexpr float kGableRoofFootprintScale = 1.05f;
constexpr float kGableRoofHeight = 0.9f;
constexpr float kTowerHeightScale = 1.3f;  // of building height
constexpr float kTowerDiameter = 0.6f;     // "tr" in the reference
constexpr float kTowerCapRadiusScale = 1.15f;
constexpr float kTowerCapHeightScale = 1.3f;
constexpr int kTowerSegments = 12;
constexpr int kConeSegments = 12;

// Returns a copy of `src` with `transform` baked into its vertices (used to
// place a locally-generated primitive within the building's local frame).
TexturedMeshResult Placed(const TexturedMeshResult& src, const glm::mat4& transform) {
  StaticTexturedMeshComponent out_mesh;
  out_mesh.geometry_type = src.mesh.geometry_type;
  AppendTransformedMesh(out_mesh, src.mesh, transform);
  out_mesh.dirty = true;
  return {.mesh = std::move(out_mesh), .local_bounds = src.local_bounds.TransformedBy(transform)};
}

// Merges `src` (transformed by `transform`) into `dst` in place.
void Merge(TexturedMeshResult& dst, const TexturedMeshResult& src, const glm::mat4& transform) {
  AppendTransformedMesh(dst.mesh, src.mesh, transform);
  dst.local_bounds = dst.local_bounds.Union(src.local_bounds.TransformedBy(transform));
}

}  // namespace

std::vector<BuildingPart> BuildBuildingParts(float width, float depth, float height,
                                             RoofShape roof) {
  std::vector<BuildingPart> parts;

  // Walls: box centered in XZ, base at y=0 (GenerateCube is centered at the
  // origin, so translate up by half the height).
  TexturedMeshResult wall_box = GenerateCube(glm::vec3(width * 0.5f, height * 0.5f, depth * 0.5f));
  TexturedMeshResult wall =
      Placed(wall_box, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, height * 0.5f, 0.0f)));
  parts.push_back({.mesh = std::move(wall), .kind = BuildingPartKind::Wall});

  switch (roof) {
    case RoofShape::Gable: {
      glm::vec3 roof_size(width * kGableRoofFootprintScale, kGableRoofHeight,
                          depth * kGableRoofFootprintScale);
      TexturedMeshResult roof_local = GenerateGableRoof(roof_size);
      TexturedMeshResult roof_placed =
          Placed(roof_local, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, height, 0.0f)));
      parts.push_back({.mesh = std::move(roof_placed), .kind = BuildingPartKind::Roof});
      break;
    }
    case RoofShape::CornerTowers: {
      const float hx = width * 0.5f;
      const float hz = depth * 0.5f;
      const float tower_h = height * kTowerHeightScale;
      const float cyl_r = 0.5f * kTowerDiameter;
      const float cone_r = cyl_r * kTowerCapRadiusScale;
      const float cone_h = kTowerDiameter * kTowerCapHeightScale;

      TexturedMeshResult cylinder = GenerateCylinder(cyl_r, tower_h, kTowerSegments);
      TexturedMeshResult cone = GenerateCone(cone_r, cone_h, kConeSegments);

      const glm::vec2 corners[4] = {{-hx, -hz}, {hx, -hz}, {hx, hz}, {-hx, hz}};
      for (const glm::vec2& corner : corners) {
        TexturedMeshResult tower =
            Placed(cylinder, glm::translate(glm::mat4(1.0f), glm::vec3(corner.x, 0.0f, corner.y)));
        Merge(tower, cone,
             glm::translate(glm::mat4(1.0f), glm::vec3(corner.x, tower_h, corner.y)));
        parts.push_back({.mesh = std::move(tower), .kind = BuildingPartKind::Tower});
      }
      break;
    }
    case RoofShape::None:
      break;
  }

  return parts;
}

}  // namespace badlands
