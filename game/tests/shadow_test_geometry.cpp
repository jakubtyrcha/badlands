#include "shadow_test_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace badlands::shadowtest {

Scene MakeMacroScene() {
  Scene scene;
  scene.ground_point = glm::vec3(0.0f);
  scene.ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);

  // 1x1x2m box: footprint half-extents 0.5m in X/Z, 2m tall along Y (half
  // 1.0m) -- base on the ground (y=0), center at y=1.
  CasterMesh box;
  box.half_extents = glm::vec3(0.5f, 1.0f, 0.5f);
  box.model_matrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  scene.casters.push_back(box);

  scene.sun_toward = glm::normalize(glm::vec3(1.0f, 1.0f, 0.25f));
  return scene;
}

TestCamera MakeMacroCamera() {
  TestCamera cam;
  // Steep, elevated diagonal view framing both the box (footprint
  // [-0.5,0.5] in X/Z, height [0,2]) and its ground shadow (falls toward
  // -X/-Z, since the sun is roughly toward +X+Y+Z -- see MakeMacroScene's
  // sun_toward). Positioned on the SAME side as the shadow (-X/-Z, i.e. NOT
  // the sun's side) and steeply elevated so the 2m-tall box doesn't occlude
  // its own nearby shadow from the camera's viewpoint (a shallower, sun-side
  // framing put the shadow almost directly behind the box from the camera).
  cam.position = glm::vec3(-4.0f, 7.0f, -4.0f);
  cam.target = glm::vec3(-0.8f, 0.3f, -0.2f);
  return cam;
}

glm::mat4 MakeOffAxisPose() {
  // Far from the origin, but not so far float precision at kFloorHalfSize
  // scale (~500m) starts to matter (ulp at ~2000m magnitude is ~2.4e-4m,
  // negligible next to the smallest E_leak in Test 1's matrix, ~0.07m).
  const glm::vec3 t(1500.0f, 800.0f, -1200.0f);

  glm::mat4 rotation(1.0f);
  rotation = glm::rotate(rotation, glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
  rotation = glm::rotate(rotation, glm::radians(-50.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  rotation = glm::rotate(rotation, glm::radians(35.0f), glm::vec3(1.0f, 0.0f, 0.0f));

  return glm::translate(glm::mat4(1.0f), t) * rotation;
}

void ApplyPose(Scene& scene, TestCamera& cam, const glm::mat4& pose) {
  // The rotation part of an affine 4x4 (translate * rotate) is exactly its
  // upper-left 3x3 block -- translation never leaks into it.
  const glm::mat3 rotation(pose);

  scene.ground_point = glm::vec3(pose * glm::vec4(scene.ground_point, 1.0f));
  scene.ground_normal = glm::normalize(rotation * scene.ground_normal);
  for (CasterMesh& caster : scene.casters) {
    caster.model_matrix = pose * caster.model_matrix;
  }
  scene.sun_toward = glm::normalize(rotation * scene.sun_toward);

  cam.position = glm::vec3(pose * glm::vec4(cam.position, 1.0f));
  cam.target = glm::vec3(pose * glm::vec4(cam.target, 1.0f));
}

void PlaneBasis(const glm::vec3& normal, glm::vec3& out_u, glm::vec3& out_v) {
  glm::vec3 arbitrary(0.0f, 1.0f, 0.0f);
  if (std::abs(glm::dot(normal, arbitrary)) > 0.99f) {
    arbitrary = glm::vec3(0.0f, 0.0f, 1.0f);
  }
  out_u = glm::normalize(glm::cross(arbitrary, normal));
  out_v = glm::normalize(glm::cross(normal, out_u));
}

glm::vec3 CameraRayDirectionWorld(const Camera& camera, glm::vec2 screen_uv) {
  // Mirrors shaders/common/frame.wesl's getRayDirectionInWorldSpace exactly:
  // same view rotation (glm::lookAt(0, direction, up)), same NDC/tan(fov/2)
  // construction, same transpose-of-rotation view->world direction map.
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), camera.direction, camera.up);
  const glm::mat3 rotation(view);

  const glm::vec2 ndc(screen_uv.x * 2.0f - 1.0f, (1.0f - screen_uv.y) * 2.0f - 1.0f);
  const float tan_half_fov = std::tan(glm::radians(camera.fov) * 0.5f);
  const glm::vec3 view_space_dir(ndc.x * camera.aspect * tan_half_fov,
                                 ndc.y * tan_half_fov, -1.0f);
  return glm::normalize(glm::transpose(rotation) * view_space_dir);
}

namespace {
float Cross2(const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
  return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}
}  // namespace

std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> pts) {
  std::sort(pts.begin(), pts.end(), [](const glm::vec2& a, const glm::vec2& b) {
    return a.x != b.x ? a.x < b.x : a.y < b.y;
  });
  pts.erase(std::unique(pts.begin(), pts.end(),
                        [](const glm::vec2& a, const glm::vec2& b) {
                          return a.x == b.x && a.y == b.y;
                        }),
           pts.end());

  const int n = static_cast<int>(pts.size());
  if (n < 3) return pts;

  std::vector<glm::vec2> hull(2 * n);
  int k = 0;
  for (int i = 0; i < n; ++i) {
    while (k >= 2 && Cross2(hull[k - 2], hull[k - 1], pts[i]) <= 0.0f) --k;
    hull[k++] = pts[i];
  }
  for (int i = n - 2, lower = k + 1; i >= 0; --i) {
    while (k >= lower && Cross2(hull[k - 2], hull[k - 1], pts[i]) <= 0.0f) --k;
    hull[k++] = pts[i];
  }
  hull.resize(static_cast<size_t>(k - 1));
  return hull;
}

float SignedDistanceToPolygon(const std::vector<glm::vec2>& poly, glm::vec2 p) {
  // Inigo Quilez's sdPolygon: winding-number sign + min distance to any
  // edge segment. Works for any polygon winding (no CCW assumption needed).
  const size_t n = poly.size();
  if (n < 3) return std::numeric_limits<float>::infinity();

  float d = glm::dot(p - poly[0], p - poly[0]);
  float s = 1.0f;
  for (size_t i = 0, j = n - 1; i < n; j = i, ++i) {
    const glm::vec2 e = poly[j] - poly[i];
    const glm::vec2 w = p - poly[i];
    const float t = glm::clamp(glm::dot(w, e) / glm::dot(e, e), 0.0f, 1.0f);
    const glm::vec2 b = w - e * t;
    d = std::min(d, glm::dot(b, b));

    const bool c1 = p.y >= poly[i].y;
    const bool c2 = p.y < poly[j].y;
    const bool c3 = e.x * w.y > e.y * w.x;
    if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
  }
  return s * std::sqrt(d);
}

std::vector<glm::vec2> ComputeShadowPolygon(const Scene& scene,
                                            const glm::vec3& basis_u,
                                            const glm::vec3& basis_v) {
  const glm::vec3 light_ray_dir = -glm::normalize(scene.sun_toward);
  std::vector<glm::vec2> points;
  points.reserve(scene.casters.size() * 8);

  for (const CasterMesh& caster : scene.casters) {
    for (int i = 0; i < 8; ++i) {
      const glm::vec3 local((i & 1) ? caster.half_extents.x : -caster.half_extents.x,
                            (i & 2) ? caster.half_extents.y : -caster.half_extents.y,
                            (i & 4) ? caster.half_extents.z : -caster.half_extents.z);
      const glm::vec3 world = glm::vec3(caster.model_matrix * glm::vec4(local, 1.0f));

      const float denom = glm::dot(light_ray_dir, scene.ground_normal);
      if (std::abs(denom) < 1e-8f) continue;  // ray parallel to the plane
      const float t = glm::dot(scene.ground_point - world, scene.ground_normal) / denom;
      const glm::vec3 proj = world + t * light_ray_dir;
      const glm::vec3 rel = proj - scene.ground_point;
      points.emplace_back(glm::dot(rel, basis_u), glm::dot(rel, basis_v));
    }
  }
  return ConvexHull(points);
}

bool RayAabbLocal(const glm::vec3& o_local, const glm::vec3& d_local,
                  const glm::vec3& half_extents, float& t_near) {
  float tmin = -std::numeric_limits<float>::infinity();
  float tmax = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    const float o = o_local[axis];
    const float d = d_local[axis];
    const float bmin = -half_extents[axis];
    const float bmax = half_extents[axis];
    if (std::abs(d) < 1e-9f) {
      if (o < bmin || o > bmax) return false;
      continue;
    }
    float t1 = (bmin - o) / d;
    float t2 = (bmax - o) / d;
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;
  }
  if (tmax < 0.0f) return false;
  t_near = tmin >= 0.0f ? tmin : tmax;
  return t_near > 0.0f;
}

PixelHit ClassifyPixel(const Scene& scene, const Camera& camera,
                       const glm::vec3& basis_u, const glm::vec3& basis_v,
                       uint32_t px, uint32_t py, uint32_t width,
                       uint32_t height) {
  const glm::vec2 screen_uv((static_cast<float>(px) + 0.5f) / static_cast<float>(width),
                            (static_cast<float>(py) + 0.5f) / static_cast<float>(height));
  const glm::vec3 ray_dir = CameraRayDirectionWorld(camera, screen_uv);
  const glm::vec3 ray_origin = camera.GetPosition();

  bool has_ground = false;
  float t_ground = 0.0f;
  glm::vec2 ground_uv(0.0f);
  {
    const float denom = glm::dot(ray_dir, scene.ground_normal);
    if (std::abs(denom) > 1e-8f) {
      const float t = glm::dot(scene.ground_point - ray_origin, scene.ground_normal) / denom;
      if (t > 0.0f) {
        const glm::vec3 hit = ray_origin + t * ray_dir;
        const glm::vec3 rel = hit - scene.ground_point;
        const glm::vec2 uv(glm::dot(rel, basis_u), glm::dot(rel, basis_v));
        if (std::abs(uv.x) <= kFloorHalfSize && std::abs(uv.y) <= kFloorHalfSize) {
          has_ground = true;
          t_ground = t;
          ground_uv = uv;
        }
      }
    }
  }

  bool has_box = false;
  float t_box = 0.0f;
  for (const CasterMesh& caster : scene.casters) {
    const glm::mat4 inv_model = glm::inverse(caster.model_matrix);
    const glm::vec3 o_local = glm::vec3(inv_model * glm::vec4(ray_origin, 1.0f));
    const glm::vec3 d_local = glm::mat3(inv_model) * ray_dir;
    float t = 0.0f;
    if (RayAabbLocal(o_local, d_local, caster.half_extents, t)) {
      if (!has_box || t < t_box) {
        has_box = true;
        t_box = t;
      }
    }
  }

  PixelHit result;
  if (has_ground && (!has_box || t_ground < t_box)) {
    result.is_receiver = true;
    result.ground_uv = ground_uv;
  }
  return result;
}

float ReconstructLinearZ(float depth, float near_plane, float far_plane) {
  return near_plane * far_plane / (depth * (far_plane - near_plane) + near_plane);
}

bool ProjectToScreenUV(const Camera& camera, const glm::vec3& world_point,
                       glm::vec2& out_uv) {
  const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), camera.direction, camera.up);
  const glm::mat4 proj = camera.GetProj();
  const glm::vec3 offseted = world_point - camera.GetPosition();
  const glm::vec4 clip = proj * view * glm::vec4(offseted, 1.0f);
  if (clip.w <= 0.0f) return false;
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  out_uv = glm::vec2((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f);
  return true;
}

}  // namespace badlands::shadowtest
