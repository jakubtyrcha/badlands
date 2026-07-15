#pragma once

// Task T4: pure-CPU scene representation + signed-distance edge oracle for
// the directional-shadow Catch2 suite. Test-only -- deliberately NOT part of
// badlands_engine (see task-4-brief.md and CLAUDE.md's "engine interface is
// stable" rule): this is throwaway verification code, not a reusable engine
// abstraction.
//
// Everything here is independent of Dawn/GPU state (no wgpu:: types) except
// for `badlands::Camera`, which is a plain POD struct (engine/core/camera.hpp)
// used only for its host-side ray-casting math.

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "engine/core/camera.hpp"

namespace badlands::shadowtest {

// A box caster: world-space model matrix (may already include the T4 rigid
// pose -- see ApplyPose) + LOCAL half-extents. The box is centered at its own
// local origin, spanning [-half_extents, +half_extents] before
// `model_matrix` is applied -- exactly GenerateCube's convention
// (primitive_mesh_builders.hpp), so the render scene built from the same
// (model_matrix, half_extents) pair via GenerateCube(half_extents) always
// matches this oracle's geometry by construction.
struct CasterMesh {
  glm::mat4 model_matrix{1.0f};
  glm::vec3 half_extents{0.5f};
};

// Test scene: an analytic ground plane (receiver) + a list of box casters +
// the local "toward-sun" direction. Expressed in LOCAL space until ApplyPose
// transforms it into world space (see task-4-brief.md's ApplyPose contract).
struct Scene {
  glm::vec3 ground_point{0.0f};
  glm::vec3 ground_normal{0.0f, 1.0f, 0.0f};
  std::vector<CasterMesh> casters;
  // Direction TOWARD the sun -- matches SceneContext::sun_direction's
  // convention (shadow_map.hpp's UpdateLightMatrices doc comment).
  glm::vec3 sun_toward{0.0f, 1.0f, 0.0f};
};

// A minimal camera descriptor (position + look-at target) in the same space
// as Scene. Converted to a real badlands::Camera (fov/aspect/near/far added)
// by the caller once both have been posed via ApplyPose.
struct TestCamera {
  glm::vec3 position{0.0f};
  glm::vec3 target{0.0f};
};

// Half-size (world units) of the analytic receiver plane / the finite floor
// quad the harness renders. Shared by RenderShadowFrame's floor-mesh builder
// and ClassifyPixel's ground-hit bounds check so both always agree on what
// counts as "the receiver": a CPU ground-plane hit outside this bound must be
// treated as "no hit" (matching the GPU's finite floor mesh, which shows
// background there), or Test 1 would assert against pixels that never render
// the debug shadow term at all.
inline constexpr float kFloorHalfSize = 500.0f;

// The Task T4 macro scene: ground (local Y=0) + a single 1x1x2m box
// (footprint 1x1m in X/Z, 2m tall along Y) with its base on the ground.
Scene MakeMacroScene();
TestCamera MakeMacroCamera();

// Task T3-fix's Test 5 (RPDB slope-acne) scene: a single planar receiver
// through the local origin, tilted 45 degrees off horizontal (about local
// X) -- meaningfully oblique to MakeMacroScene's `sun_toward` (NdotL ~0.62,
// vs. the flat macro floor's ~0.70) so shadow-map texels stretch
// non-trivially across the surface, the case that needs a correct
// receiver-plane depth bias. NO casters: this isolates self-acne on a LIT
// slope from cast-shadow edge behavior (Test 1's concern). Reuses
// `sun_toward` from MakeMacroScene for consistency.
Scene MakeSlopeScene();

// A fixed, non-axis-aligned rigid pose: M = translate(t) * rotate(R), t far
// from the origin, R = Euler(35, -50, 20) degrees composed about X, Y, Z --
// chosen so no resulting camera axis is parallel to a world axis.
glm::mat4 MakeOffAxisPose();

// Applies `pose` to `scene`/`cam` in place: POSITIONS (ground_point, caster
// model matrices, camera position/target) transform by the full `pose`;
// DIRECTIONS (ground_normal, sun_toward) transform by `pose`'s rotation part
// only (glm::mat3(pose) -- exactly R, since translation never leaks into a
// 4x4 affine matrix's 3x3 block). Caster model matrices are pre-multiplied
// (pose * model_matrix) so their own local shape is preserved.
void ApplyPose(Scene& scene, TestCamera& cam, const glm::mat4& pose);

// Picks an arbitrary orthonormal tangent basis (u, v) for the plane with
// unit `normal`, such that (u, v, normal) is a right-handed orthonormal
// frame. Used both to build the harness's floor mesh transform and to
// express ground-plane hit points in 2D for the hull/signed-distance oracle
// below -- an arbitrary (but consistent) choice, since the plane's own
// texture/rotation about its normal is irrelevant to both the render and the
// oracle.
void PlaneBasis(const glm::vec3& normal, glm::vec3& out_u, glm::vec3& out_v);

// World-space ray direction for a fullscreen pixel's screen_uv (x,y in
// [0,1], y=0 at the TOP row -- matching WGSL's screenUV convention emitted
// by getFullscreenTriangleVertex/used by getRayDirectionInWorldSpace in
// shaders/common/frame.wesl). Mirrors that WGSL function exactly (same
// glm::lookAt-derived rotation, same tan(fov/2)*ndc construction) so the CPU
// oracle casts the SAME ray the GPU's deferred pass reconstructs for a given
// pixel -- load-bearing for the whole oracle's pixel correspondence.
glm::vec3 CameraRayDirectionWorld(const Camera& camera, glm::vec2 screen_uv);

// Convex hull (CCW) of `pts` via Andrew's monotone chain. Duplicate/interior
// points are simply excluded from the result -- callers do not need to
// dedupe input points first (e.g. GenerateCube's 24-unique-corner-but-8-
// unique-position vertex layout would work unchanged, though this oracle
// instead projects the box's 8 sign-combination corners directly).
std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> pts);

// Signed distance from `p` to the boundary of polygon `poly` (any winding,
// convex or not): negative inside, positive outside. Inigo Quilez's
// winding-number `sdPolygon` construction -- doesn't require `poly` to be
// CCW, so it works directly on ConvexHull's output.
float SignedDistanceToPolygon(const std::vector<glm::vec2>& poly, glm::vec2 p);

// Projects every caster's 8 local corners onto the ground plane along
// `light_ray_dir` (the direction light TRAVELS, i.e. -sun_toward), expresses
// each projected point in the (basis_u, basis_v) 2D frame relative to
// ground_point, and returns the convex hull of the combined point set --
// the "shadow polygon" Test 1's oracle measures distance against.
std::vector<glm::vec2> ComputeShadowPolygon(const Scene& scene,
                                            const glm::vec3& basis_u,
                                            const glm::vec3& basis_v);

// Ray/axis-aligned-box intersection (slab method) in the box's OWN local
// space: `o_local`/`d_local` must already be the ray transformed into local
// space (see ClassifyPixel), `d_local` unit length. On a hit with t>0,
// returns true and sets `t_near` to the entry distance (== world-space
// distance along the original unit ray, since local space here differs from
// world space by a rigid transform only -- no scale).
bool RayAabbLocal(const glm::vec3& o_local, const glm::vec3& d_local,
                  const glm::vec3& half_extents, float& t_near);

// Per-pixel classification result for the Test 1 oracle.
struct PixelHit {
  bool is_receiver = false;
  // Ground-hit point in the (basis_u, basis_v) frame relative to
  // ground_point -- only valid when is_receiver is true.
  glm::vec2 ground_uv{0.0f};
};

// Casts the camera ray for pixel (px, py) of a `width`x`height` frame,
// intersects both the ground plane (bounded to kFloorHalfSize, matching the
// harness's finite floor mesh) and every caster's box; the pixel is a
// "receiver" iff the ground is the NEAREST hit (nearer than every caster,
// and a valid hit at all) -- mirrors the GPU's depth-test-driven nearest-
// surface-wins semantics.
PixelHit ClassifyPixel(const Scene& scene, const Camera& camera,
                       const glm::vec3& basis_u, const glm::vec3& basis_v,
                       uint32_t px, uint32_t py, uint32_t width,
                       uint32_t height);

// CPU mirror of shaders/common/frame.wesl's reconstructLinearZ: near*far /
// (depth*(far-near)+near). Reversed-Z: depth=1 -> near, depth=0 -> far.
float ReconstructLinearZ(float depth, float near_plane, float far_plane);

// Projects `world_point` through the SAME view/proj construction
// SceneRenderer::Render uses (world-camera-offset scheme: subtract
// camera.GetPosition(), then glm::lookAt(0, camera.direction, camera.up) *
// camera.GetProj()) to a screen UV (x,y in [0,1], y=0 at the top -- see
// CameraRayDirectionWorld). Returns false (out_uv left unset) if the point
// is behind the camera (clip.w <= 0); does NOT clamp/clip to [0,1] -- the
// caller decides what to do with an out-of-frame result.
bool ProjectToScreenUV(const Camera& camera, const glm::vec3& world_point,
                       glm::vec2& out_uv);

}  // namespace badlands::shadowtest
