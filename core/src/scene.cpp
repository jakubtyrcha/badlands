#include "scene.h"

#include <string>
#include <vector>

#include "math.h"

namespace sq {

namespace {
const char* shape_name(Shape shape) {
    return (shape == Shape::Cube) ? "Cube" : "Sphere";
}
} // namespace

simd_float4x4 Node::world_from_local() const {
    return trs_matrix(position, rotation, scale);
}

Node* SceneDocument::find(int32_t id) {
    for (Node& node : nodes_) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const Node* SceneDocument::find(int32_t id) const {
    for (const Node& node : nodes_) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

Node& SceneDocument::add(Node node) {
    nodes_.push_back(std::move(node));
    return nodes_.back();
}

Node SceneDocument::make_node(Shape shape, Op op) {
    Node node;
    node.id = next_id_++;
    node.shape = shape;
    node.op = op;
    int32_t& count = (shape == Shape::Cube) ? cube_count_ : sphere_count_;
    node.name = std::string(shape_name(shape)) + " " + std::to_string(++count);
    return node;
}

int32_t SceneDocument::spawn_snapped(Shape shape, Op op, simd_float3 hit, simd_float3 unit_normal,
                                      int32_t parent_id) {
    Node node = make_node(shape, op);
    // Centred ON the surface, not resting on it. A detail added to a face is
    // meant to be half-embedded -- that is what an Add node unions into, and a
    // Subtract node carves from. It also puts the node's centre exactly on its
    // snap point, so the placement and shape gizmos coincide until the detail
    // is deliberately lifted off.
    node.position = hit;
    node.snapped = true;
    node.snap_point = hit;
    node.snap_normal = unit_normal;
    node.snap_parent = parent_id;
    return add(std::move(node)).id;
}

int32_t SceneDocument::spawn_unsnapped(Shape shape, Op op, simd_float3 position) {
    Node node = make_node(shape, op);
    node.position = position;
    node.snapped = false;
    node.snap_parent = kInvalidNode;
    return add(std::move(node)).id;
}

void SceneDocument::remove_node(int32_t id) {
    std::erase_if(nodes_, [id](const Node& node) { return node.id == id; });

    // Fix up survivors that were snapped onto the just-removed node: leaving
    // snap_parent pointing at a dead id would be a dangling reference. The
    // gizmo frame keys off `snapped`, so clearing it here drops the orphan back
    // to its own local axes, exactly like a node that was never snapped.
    for (Node& node : nodes_) {
        if (node.snap_parent == id) {
            node.snapped = false;
            node.snap_parent = kInvalidNode;
        }
    }
}

} // namespace sq
