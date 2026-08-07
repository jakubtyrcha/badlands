#include "scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "math_util.h"

namespace sq {

namespace {

// Indexed by Shape's own value, so a new shape is one row here and one enum
// case -- never a chain of ternaries to extend.
constexpr const char* kShapeNames[kShapeCount] = {
    "Cube", "Sphere", "Cone", "Capsule", "Octahedron", "Pyramid", "Prism", "Vesica",
};

// The dial table. Every shape but the sphere has one profile degree of freedom
// its bounding box cannot state, and carries a dial for it. The sphere is the
// sole exception because an ellipsoid is already the roundest thing its box
// allows -- there is nothing left to vary.
//
// ROUNDNESS is the same idea on four different shapes (see sdf_eval_node), and
// is why the vesica and the octahedron are not the parameterless oddities an
// earlier draft made them: their POINTINESS is indeed their box aspect, and a
// dial for that would have duplicated the Shape gizmo -- but how sharp their
// tips are is free, and the box says nothing about it.
//
// Defaults are chosen so a freshly spawned shape looks like its own name: a
// cone is a cone (sharp), a cube has square corners, a capsule is a capsule.
constexpr ShapeParamSpec kShapeParams[kShapeCount] = {
    /* Cube       */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // roundness: box -> ball
    /* Sphere     */ {false, 0.0f, 0.0f,  0.0f,  0.0f, false},
    /* Cone       */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // tip ratio: point -> cylinder
    /* Capsule    */ {true,  0.0f, 1.0f,  0.05f, 1.0f, false},  // cap roundness: flat -> capsule
    /* Octahedron */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // roundness: faceted -> ball
    /* Pyramid    */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // tip ratio: point -> box
    /* Prism      */ {true,  3.0f, 12.0f, 1.0f,  6.0f, true},   // side count
    /* Vesica     */ {true,  0.0f, 1.0f,  0.05f, 0.0f, false},  // tip roundness: cusps -> capsule
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

NodePlacement SceneDocument::placement(int32_t id) const {
    NodePlacement out;
    const Node* node = find(id);
    if (node == nullptr) {
        return out; // unknown id: identity frame, no box, no contact
    }

    out.frame.position = node->position;
    out.frame.rotation = node->rotation;
    out.frame.uniform_scale = 1.0f;
    // simd_abs, matching append_node_wireframe and sdf_safe_half_extents: the
    // evaluator measures against abs(half_extents), so a negative component
    // mirrors the solid rather than inverting the box.
    out.half_extents = 0.5f * simd_abs(node->scale);
    if (node->snapped) {
        out.contact = WorldContact{node->snap_point, node->snap_normal};
    }
    return out;
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
