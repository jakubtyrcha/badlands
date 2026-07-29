#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <string>
#include <vector>

namespace sq {

enum class Shape : int32_t { Cube = 0, Sphere = 1 };
enum class Op    : int32_t { Add = 0, Subtract = 1 };

inline constexpr int32_t kInvalidNode = -1;

struct Node {
    int32_t id = kInvalidNode;
    std::string name;
    Shape shape = Shape::Cube;
    Op op = Op::Add;
    simd_float3 position = {0, 0, 0};
    simd_quatf rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);  // identity; stays identity in MVP
    simd_float3 scale = {1, 1, 1};
    bool snapped = false;
    simd_float3 snap_point = {0, 0, 0};
    simd_float3 snap_normal = {0, 1, 0};
    int32_t snap_parent = kInvalidNode;

    simd_float4x4 world_from_local() const;   // trs_matrix(position, rotation, scale)
};

class SceneDocument {
public:
    Node* find(int32_t id);
    const Node* find(int32_t id) const;
    const std::vector<Node>& nodes() const { return nodes_; }
    // Direct add for tests/hardcoded scenes; spawn APIs arrive in a later milestone.
    Node& add(Node node);

private:
    std::vector<Node> nodes_;
};

} // namespace sq
