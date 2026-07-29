#include "picking.h"

#include <cmath>
#include <limits>

#include "scene.h"

namespace sq {

namespace {

constexpr float kEps = 1e-4f;

float sign_nonzero(float v) {
    return (v >= 0.0f) ? 1.0f : -1.0f;
}

} // namespace

std::optional<RayHit> ray_unit_sphere(const Ray& local) {
    const simd_float3& o = local.origin;
    const simd_float3& d = local.dir;

    // |o + t d|^2 = 0.25  =>  a t^2 + b t + c = 0
    const float a = simd_dot(d, d);
    const float b = 2.0f * simd_dot(o, d);
    const float c = simd_dot(o, o) - 0.25f;

    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) {
        return std::nullopt; // no real roots: ray misses the sphere entirely
    }

    const float sqrt_disc = std::sqrt(disc);
    const float root_near = (-b - sqrt_disc) / (2.0f * a);
    const float root_far  = (-b + sqrt_disc) / (2.0f * a);

    // Smallest root > kEps: an origin inside the sphere makes root_near <=
    // kEps (often negative), so this naturally falls through to the exit
    // root (root_far).
    float t;
    if (root_near > kEps) {
        t = root_near;
    } else if (root_far > kEps) {
        t = root_far;
    } else {
        return std::nullopt;
    }

    const simd_float3 point = o + t * d;
    const simd_float3 normal = simd_normalize(point); // point / 0.5, then normalized == normalize(point)
    return RayHit{t, point, normal};
}

std::optional<RayHit> ray_unit_cube(const Ray& local) {
    const float o[3] = {local.origin.x, local.origin.y, local.origin.z};
    const float d[3] = {local.dir.x, local.dir.y, local.dir.z};

    float tmin = -std::numeric_limits<float>::infinity();
    float tmax = std::numeric_limits<float>::infinity();
    int tmin_axis = -1;
    int tmax_axis = -1;

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-12f) {
            if (o[axis] < -0.5f || o[axis] > 0.5f) {
                return std::nullopt; // parallel to this pair of slab planes and outside them
            }
            continue; // parallel but inside: this axis doesn't constrain tmin/tmax
        }
        const float t1 = (-0.5f - o[axis]) / d[axis];
        const float t2 = (0.5f - o[axis]) / d[axis];
        const float axis_near = std::fmin(t1, t2);
        const float axis_far  = std::fmax(t1, t2);
        if (axis_near > tmin) {
            tmin = axis_near;
            tmin_axis = axis;
        }
        if (axis_far < tmax) {
            tmax = axis_far;
            tmax_axis = axis;
        }
    }

    if (tmax < std::fmax(tmin, kEps)) {
        return std::nullopt;
    }

    const bool entry = tmin > kEps;
    const float t = entry ? tmin : tmax;
    const int axis = entry ? tmin_axis : tmax_axis;

    // Face normal: unit axis vector of the crossing axis, signed toward the
    // side the ray crossed — opposite the ray's direction component on entry
    // (the ray arrives from outside, moving against the outward normal),
    // along it on exit (the ray leaves moving with the outward normal).
    const float comp = entry ? -sign_nonzero(d[axis]) : sign_nonzero(d[axis]);
    simd_float3 normal = {0.0f, 0.0f, 0.0f};
    normal[axis] = comp;

    const simd_float3 point = local.origin + t * local.dir;
    return RayHit{t, point, normal};
}

std::optional<PickHit> raycast_scene(const SceneDocument& doc, const Ray& world) {
    std::optional<PickHit> best;

    for (const Node& node : doc.nodes()) {
        const simd_float4x4 M = node.world_from_local();
        const simd_float4x4 Minv = simd_inverse(M);

        const simd_float4 local_origin4 =
            simd_mul(Minv, (simd_float4){world.origin.x, world.origin.y, world.origin.z, 1.0f});
        const simd_float4 local_dir4 =
            simd_mul(Minv, (simd_float4){world.dir.x, world.dir.y, world.dir.z, 0.0f}); // NOT re-normalized
        const Ray local{local_origin4.xyz, local_dir4.xyz};

        const std::optional<RayHit> hit =
            (node.shape == Shape::Cube) ? ray_unit_cube(local) : ray_unit_sphere(local);
        if (!hit) {
            continue;
        }

        const simd_float4 world_point4 =
            simd_mul(M, (simd_float4){hit->point.x, hit->point.y, hit->point.z, 1.0f});
        const simd_float3 world_point = world_point4.xyz;
        const float world_t = simd_dot(world_point - world.origin, world.dir);
        if (world_t <= kEps) {
            continue;
        }

        const simd_float4x4 Minv_t = simd_transpose(Minv);
        const simd_float4 world_normal4 =
            simd_mul(Minv_t, (simd_float4){hit->normal.x, hit->normal.y, hit->normal.z, 0.0f});
        const simd_float3 world_normal = simd_normalize(world_normal4.xyz);

        if (!best || world_t < best->hit.t) {
            best = PickHit{node.id, RayHit{world_t, world_point, world_normal}};
        }
    }

    return best;
}

} // namespace sq
