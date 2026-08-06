#include "scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "math.h"

namespace sq {

namespace {

// Indexed by Shape's own value, so a new shape is one row here and one enum
// case -- never a chain of ternaries to extend.
constexpr const char* kShapeNames[kShapeCount] = {
    "Cube", "Sphere", "Cone", "Capsule", "Octahedron", "Pyramid", "Prism", "Vesica",
};

// The dial table. Four shapes have a profile degree of freedom the box cannot
// state; the other four are fully described by their half-extents and carry no
// dial. The vesica is the interesting omission -- its pointiness IS its box
// aspect (length from scale.y, width from scale.x), so a dial would give the
// Shape gizmo and the dial one meaning between them.
//
// Defaults are chosen so a freshly spawned shape looks like its own name: a
// cone is a cone (sharp), a capsule is a capsule (fully round).
constexpr ShapeParamSpec kShapeParams[kShapeCount] = {
    /* Cube       */ {false, 0.0f, 0.0f,  0.0f,  0.0f, false},
    /* Sphere     */ {false, 0.0f, 0.0f,  0.0f,  0.0f, false},
    /* Cone       */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // tip ratio: point -> cylinder
    /* Capsule    */ {true,  0.0f, 1.0f,  0.05f, 1.0f, false},  // cap roundness: flat -> capsule
    /* Octahedron */ {false, 0.0f, 0.0f,  0.0f,  0.0f, false},
    /* Pyramid    */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // tip ratio: point -> box
    /* Prism      */ {true,  3.0f, 12.0f, 1.0f,  6.0f, true},   // side count
    /* Vesica     */ {false, 0.0f, 0.0f,  0.0f,  0.0f, false},
};

// Shape -> table index, guarding against a value from outside the enum (an id
// crossing the interop boundary is just an int32).
int shape_index(Shape shape) {
    const int32_t raw = static_cast<int32_t>(shape);
    return (raw >= 0 && raw < kShapeCount) ? static_cast<int>(raw) : 0;
}

const char* shape_name(Shape shape) {
    return kShapeNames[shape_index(shape)];
}

} // namespace

ShapeParamSpec shape_param_spec(Shape shape) {
    return kShapeParams[shape_index(shape)];
}

float snap_shape_param(const ShapeParamSpec& spec, float value) {
    if (!spec.has_param || !(spec.step > 0.0f)) {
        return 0.0f;
    }
    // Clamp BEFORE snapping: snapping first could round a just-out-of-range
    // value onto a valid multiple and hide that it was out of range at all.
    // NaN is clamped to min_value rather than propagating -- std::clamp on a
    // NaN is undefined, so this is a comparison chain instead.
    float v = value;
    if (!(v >= spec.min_value)) {
        v = spec.min_value;
    } else if (v > spec.max_value) {
        v = spec.max_value;
    }
    const float snapped = std::round(v / spec.step) * spec.step;
    // Re-clamp: the round can step outside the range when an end is not itself
    // a multiple of the step.
    return std::clamp(snapped, spec.min_value, spec.max_value);
}

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
    node.shape_param = shape_param_spec(shape).default_value;
    int32_t& count = shape_counts_[shape_index(shape)];
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
