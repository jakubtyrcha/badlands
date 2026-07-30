#include "lines.h"

#include <array>
#include <cmath>

#include "scene.h"

namespace sq {

namespace {

LineVertex make_vertex(simd_float3 local, const simd_float4x4& world_from_local, simd_float4 color) {
    const simd_float4 world = simd_mul(world_from_local, (simd_float4){local.x, local.y, local.z, 1.0f});
    LineVertex v;
    v.pos = (simd_float4){world.x, world.y, world.z, 1.0f};
    v.color = color;
    return v;
}

constexpr int kCircleSegments = 32;

} // namespace

void append_cube_edges(std::vector<LineVertex>& out, const simd_float4x4& world_from_local, simd_float4 color) {
    static constexpr std::array<simd_float3, 8> kCorners = {{
        {-0.5f, -0.5f, -0.5f}, // 0
        { 0.5f, -0.5f, -0.5f}, // 1
        { 0.5f,  0.5f, -0.5f}, // 2
        {-0.5f,  0.5f, -0.5f}, // 3
        {-0.5f, -0.5f,  0.5f}, // 4
        { 0.5f, -0.5f,  0.5f}, // 5
        { 0.5f,  0.5f,  0.5f}, // 6
        {-0.5f,  0.5f,  0.5f}, // 7
    }};
    static constexpr std::array<std::array<int, 2>, 12> kEdges = {{
        {0, 1}, {1, 5}, {5, 4}, {4, 0}, // bottom
        {3, 2}, {2, 6}, {6, 7}, {7, 3}, // top
        {0, 3}, {1, 2}, {5, 6}, {4, 7}, // verticals
    }};

    for (const auto& edge : kEdges) {
        out.push_back(make_vertex(kCorners[edge[0]], world_from_local, color));
        out.push_back(make_vertex(kCorners[edge[1]], world_from_local, color));
    }
}

void append_sphere_circles(std::vector<LineVertex>& out, const simd_float4x4& world_from_local, simd_float4 color) {
    constexpr float r = 0.5f;
    // axis 0 = X, 1 = Y, 2 = Z.
    for (int axis = 0; axis < 3; ++axis) {
        auto point = [&](int i) -> simd_float3 {
            const float t = static_cast<float>(i) / static_cast<float>(kCircleSegments) * 2.0f * float(M_PI);
            const float a = std::cos(t) * r;
            const float b = std::sin(t) * r;
            switch (axis) {
                case 0: return simd_float3{0.0f, a, b};  // around X
                case 1: return simd_float3{a, 0.0f, b};  // around Y
                default: return simd_float3{a, b, 0.0f}; // around Z
            }
        };
        for (int i = 0; i < kCircleSegments; ++i) {
            out.push_back(make_vertex(point(i), world_from_local, color));
            out.push_back(make_vertex(point(i + 1), world_from_local, color));
        }
    }
}

void append_tangent_frame(std::vector<LineVertex>& out, simd_float3 origin, simd_float3 normal,
                          float half_extent, int divisions) {
    const simd_float3 ref = (std::fabs(normal.y) < 0.99f) ? simd_float3{0.0f, 1.0f, 0.0f}
                                                            : simd_float3{1.0f, 0.0f, 0.0f};
    const simd_float3 u = simd_normalize(simd_cross(normal, ref));
    const simd_float3 v = simd_cross(normal, u);

    const float he = half_extent;
    const float step = 2.0f * he / static_cast<float>(divisions);
    const int center = divisions / 2;

    auto push = [&](simd_float3 p, simd_float4 color) {
        LineVertex vertex;
        vertex.pos = (simd_float4){p.x, p.y, p.z, 1.0f};
        vertex.color = color;
        out.push_back(vertex);
    };

    // Grid lines: for each sample i in [0, divisions] (skipping the center,
    // which would duplicate the axis lines below), emit one line running
    // along u (offset along v) and one running along v (offset along u).
    for (int i = 0; i <= divisions; ++i) {
        if (i == center) {
            continue;
        }
        const float offset = -he + static_cast<float>(i) * step;
        push(origin + offset * v - he * u, kColorGridLine);
        push(origin + offset * v + he * u, kColorGridLine);
        push(origin + offset * u - he * v, kColorGridLine);
        push(origin + offset * u + he * v, kColorGridLine);
    }

    // Axis lines through the origin.
    push(origin - he * u, kColorGridAxis);
    push(origin + he * u, kColorGridAxis);
    push(origin - he * v, kColorGridAxis);
    push(origin + he * v, kColorGridAxis);

    // Normal stub.
    push(origin, kColorGridAxis);
    push(origin + normal * (0.5f * he), kColorGridAxis);
}

std::vector<LineVertex> build_scene_lines(const SceneDocument& doc, int32_t selected_id) {
    std::vector<LineVertex> out;
    for (const Node& node : doc.nodes()) {
        simd_float4 color = (node.op == Op::Add) ? kColorAdd : kColorSubtract;
        if (node.id == selected_id) {
            color = kColorSelected;
        }
        const simd_float4x4 world_from_local = node.world_from_local();
        if (node.shape == Shape::Cube) {
            append_cube_edges(out, world_from_local, color);
        } else {
            append_sphere_circles(out, world_from_local, color);
        }
    }
    return out;
}

} // namespace sq
