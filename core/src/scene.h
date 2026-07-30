#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <string>
#include <vector>

#include <shapeshifter/ShapeshifterCore.h>  // Shape, Op, kInvalidNode

namespace sq {

// Fixed camera-ray distance an unsnapped spawn (ray miss) lands at.
inline constexpr float kUnsnappedSpawnDistance = 6.0f;

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
    // Direct add for tests; spawn_snapped/spawn_unsnapped below are the real
    // spawn entry points (id/name allocation, snap bookkeeping).
    Node& add(Node node);

    // hit/unit_normal: world-space raycast_scene hit; the new node rests on
    // the surface (position = hit + unit_normal * 0.5). parent_id: the node
    // that was hit, stored as snap_parent.
    int32_t spawn_snapped(Shape shape, Op op, simd_float3 hit, simd_float3 unit_normal, int32_t parent_id);
    // position: world-space placement as given (e.g. a fixed distance along
    // the spawn ray on a raycast miss). Not snapped; snap fields at Node's
    // defaults except snap_parent, explicitly kInvalidNode.
    int32_t spawn_unsnapped(Shape shape, Op op, simd_float3 position);

private:
    // Allocates id (next_id_++) and an auto name ("<Shape> <per-shape count>",
    // counting spawns of that shape independent of ids/removals); shape, op
    // set; rotation/scale/snap fields left at Node's defaults for the caller
    // to fill in as needed.
    Node make_node(Shape shape, Op op);

    std::vector<Node> nodes_;
    int32_t next_id_ = 1;
    int32_t cube_count_ = 0;
    int32_t sphere_count_ = 0;
};

} // namespace sq
