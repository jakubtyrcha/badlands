#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <vector>
#include <shared_types.h>

namespace sq {
class SceneDocument;

inline constexpr simd_float4 kColorAdd      = {0.1f, 1.0f, 0.1f, 1.0f};
inline constexpr simd_float4 kColorSubtract = {1.0f, 0.2f, 0.15f, 1.0f};
inline constexpr simd_float4 kColorSelected = {0.6f, 0.8f, 1.0f, 1.0f};

// 12 edges -> 24 vertices. Unit cube corners at ±0.5.
void append_cube_edges(std::vector<LineVertex>& out, const simd_float4x4& world_from_local, simd_float4 color);
// 3 great circles (around local X, Y, Z), 32 segments each -> 192 vertices, radius 0.5 local.
void append_sphere_circles(std::vector<LineVertex>& out, const simd_float4x4& world_from_local, simd_float4 color);
// Color per node op (Add=kColorAdd, Subtract=kColorSubtract); selected node overrides to kColorSelected.
std::vector<LineVertex> build_scene_lines(const SceneDocument& doc, int32_t selected_id);
}
