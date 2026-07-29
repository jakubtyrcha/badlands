#include "scene.h"

#include "math.h"

namespace sq {

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

} // namespace sq
