#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <optional>
#include "camera.h"     // sq::Ray

namespace sq {

class SceneDocument;

struct RayHit {
    float t;                 // parametric distance along the (normalized) query ray
    simd_float3 point;
    simd_float3 normal;      // unit length
};

// Local-space primitive tests. The incoming ray is in the primitive's local
// space and its dir is NOT normalized (it went through an inverse transform);
// t is therefore in the local ray's parameterization — callers re-derive
// world t from the world-space hit point.
std::optional<RayHit> ray_unit_sphere(const Ray& local);   // radius 0.5, origin-centered
std::optional<RayHit> ray_unit_cube(const Ray& local);     // [-0.5,0.5]^3 slab test

struct PickHit { int32_t node_id; RayHit hit; };           // hit is world-space, t in world units

std::optional<PickHit> raycast_scene(const SceneDocument& doc, const Ray& world);

} // namespace sq
