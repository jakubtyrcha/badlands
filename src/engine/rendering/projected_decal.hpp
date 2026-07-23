#pragma once

// Generic, game-agnostic projected-decal primitive.
//
// A ProjectedDecal is a world-space footprint lying on a horizontal plane at
// `center.y`, PROJECTED onto whatever geometry the depth buffer reconstructs
// within +/- `projector_half_height` of that plane. The decal pass
// (rendering/passes/render_projected_decals.hpp) is a fullscreen pass: it
// reconstructs the world position per pixel from the G-buffer depth, maps it
// into each decal's local frame, evaluates an outline SDF, and blends the
// dashed line into the target.
//
// Only the OUTLINE is drawn (an SDF band of `line_width`), optionally dashed
// with two alternating colours -- the default (white/black, scrolling) is the
// classic "marching ants" selection look, but both colours, the dash metrics
// and the line width are per-decal parameters.
//
// No game types appear here: a decal is a shape + a placement + a line style.
// The descriptor is deliberately INJECTION-STAGE INDEPENDENT -- today one pass
// composites decals into the lit HDR colour; a future pass could stamp the same
// descriptors into the G-buffer (albedo/normals) or the light buffer without
// changing this struct.

#include <cstdint>

#include <glm/glm.hpp>

namespace badlands {

// Footprint shape, evaluated as a 2D SDF in the decal's local XZ frame.
enum class DecalShape : uint32_t {
  Ring = 0,        // circle outline; half_extents.x is the radius
  RoundedRect = 1  // rounded-rectangle outline; half_extents = half size
};

// Maximum decals composited in one pass (see the decal UBO in
// render_projected_decals.cpp). Excess decals are dropped with a warning.
inline constexpr uint32_t kMaxProjectedDecals = 32;

struct ProjectedDecal {
  // Placement.
  glm::vec3 center{0.0f};  // footprint centre: xz position, y = the decal plane
  float yaw = 0.0f;        // rotation about +Y, radians
  glm::vec2 half_extents{1.0f};  // local half-size (Ring: .x = radius, .y unused)
  // Vertical clip band around center.y. A reconstructed surface further than
  // this from the plane is not decalled -- keeps a ground decal off rooftops
  // and off geometry passing above/below it.
  float projector_half_height = 2.0f;

  // Line style.
  float line_width = 0.15f;     // outline thickness, world units
  float corner_radius = 0.0f;   // RoundedRect corner rounding (ignored by Ring)

  DecalShape shape = DecalShape::Ring;

  // Which receiving surfaces accept this decal, as a fade band on the world
  // normal's Y component: fully rejected at or below `receiver_min_normal_y`,
  // fully accepted at or above `receiver_max_normal_y`, smoothly faded between.
  // The vertical projector band alone cannot do this -- a wall passing through
  // the band would take the decal and smear it vertically.
  //
  // The defaults keep a decal on roughly-horizontal ground (rejecting anything
  // steeper than ~69 degrees). A decal meant for walls or arbitrary geometry
  // sets its own band; `receiver_min_normal_y = -1` accepts every surface.
  float receiver_min_normal_y = 0.35f;
  float receiver_max_normal_y = 0.65f;

  // Dash pattern, measured in world units ALONG the outline. dash_length <= 0
  // (or dash_length + dash_gap <= 0) draws a solid line in color_a. The period
  // is snapped to divide the closed perimeter exactly, so the pattern has no
  // seam where the curve wraps (see decal_math.hpp's FitDashPeriod).
  float dash_length = 0.5f;
  float dash_gap = 0.5f;
  float scroll_speed = 0.5f;  // dash periods per second (marching direction +)

  // The two alternating dash colours, linear RGB + straight alpha. The default
  // white/black pair reads on any background.
  glm::vec4 color_a{1.0f, 1.0f, 1.0f, 1.0f};
  glm::vec4 color_b{0.0f, 0.0f, 0.0f, 1.0f};
};

}  // namespace badlands
