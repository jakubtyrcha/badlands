#include "scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

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

Frame SceneDocument::local_frame(const Node& node) const {
    Frame f;
    f.position = node.local_position;
    f.rotation = node.local_rotation;
    // THE PROPAGATION RULE, in one ternary. A Group contributes its uniform
    // scale, which reaches every descendant's offset and box. A Shape
    // contributes exactly 1, because its scale is the size of its own box and
    // stretching a skull must not smear the horn attached to it.
    f.uniform_scale = (node.kind == NodeKind::Group) ? node.scale.x : 1.0f;
    return f;
}

Frame SceneDocument::resolve_parent_frame(const Node& node, bool& binding_resolved,
                                          bool& cyclic) const {
    binding_resolved = true;
    cyclic = false;

    // Walk UP collecting the chain, then compose DOWNWARD. Iterative rather
    // than recursive so the cycle guard is a plain loop bound and a corrupt
    // document cannot blow the stack.
    const Node* chain[kMaxParentDepth];
    int depth = 0;
    // Identity unless the walk ends at a resolved external attachment, in which
    // case the rig supplies the frame everything above sits on.
    Frame base;

    for (const Node* cur = &node;;) {
        if (cur->parent.kind == ParentRef::Kind::World) {
            break;
        }
        if (cur->parent.kind == ParentRef::Kind::Attachment) {
            // The chain leaves the document here. Whatever the provider hands
            // back has ALREADY crossed frame_from_matrix, so it is a similarity
            // and cannot carry shear into anything composed on top of it.
            const std::optional<Frame> attached =
                provider_ != nullptr ? provider_->frame_for_attachment(cur->parent.attachment)
                                     : std::nullopt;
            if (attached.has_value()) {
                base = *attached;
            } else {
                // No provider, or a name it does not know. The node stays
                // USABLE -- world-rooted -- and says so, rather than sitting at
                // the origin with no explanation. Reported from wherever in the
                // chain it happened, so a descendant of a broken binding knows
                // its own placement is untrustworthy too.
                binding_resolved = false;
            }
            break;
        }
        const Node* parent = find(cur->parent.node);
        if (parent == nullptr) {
            break; // dangling parent id: fall back to world-rooted
        }
        if (depth >= kMaxParentDepth) {
            // Only a cycle reaches this: attach() rejects them, so arriving here
            // means parent fields were written directly. Reported to the caller
            // rather than papered over, because a frame composed from half a
            // cycle is worse than no frame at all.
            cyclic = true;
            return Frame{};
        }
        chain[depth++] = parent;
        cur = parent;
    }

    Frame f = base;
    for (int i = depth - 1; i >= 0; --i) {
        f = compose(f, local_frame(*chain[i]));
    }
    return f;
}

Frame SceneDocument::parent_frame(int32_t id) const {
    const Node* node = find(id);
    if (node == nullptr) {
        return Frame{};
    }
    bool binding_resolved = true;
    bool cyclic = false;
    return resolve_parent_frame(*node, binding_resolved, cyclic);
}

NodePlacement SceneDocument::placement(int32_t id) const {
    NodePlacement out;
    const Node* node = find(id);
    if (node == nullptr) {
        return out; // unknown id: identity frame, no box, no contact
    }

    bool cyclic = false;
    // The parent's frame, kept in hand rather than folded straight in, because
    // the CONTACT rides it -- not the node's own frame.
    const Frame parent_frame = resolve_parent_frame(*node, out.binding_resolved, cyclic);
    if (cyclic) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::error("shapeshifter: node {} exceeds the parent-depth guard ({}); "
                          "the chain is cyclic and the node will not be placed",
                          id, kMaxParentDepth);
        }
        return NodePlacement{};
    }
    out.frame = compose(parent_frame, local_frame(*node));

    // A Group has no box and contributes no SDF, so it reports no extent.
    // simd_abs, matching append_node_wireframe and sdf_safe_half_extents: the
    // evaluator measures against abs(half_extents), so a negative component
    // mirrors the solid rather than inverting the box.
    if (node->kind == NodeKind::Shape) {
        out.half_extents = 0.5f * simd_abs(node->scale) * out.frame.uniform_scale;
    }

    if (node->contact.valid) {
        // The contact is a fact about the SURFACE, expressed in the parent's
        // frame -- so it rides the parent and is untouched by anything the node
        // itself does. Dragging a detail off the skin it was placed on leaves
        // the attachment behind, which is what the tether reports.
        out.contact = WorldContact{transform_point(parent_frame, node->contact.point),
                                   transform_direction(parent_frame, node->contact.normal)};
    }
    return out;
}

void SceneDocument::set_node_scale(int32_t id, simd_float3 scale) {
    Node* node = find(id);
    if (node == nullptr) {
        return;
    }
    // Forced uniform for a Group rather than merely asked to be: compose has no
    // meaning for a Group with {1,2,1}, and a rule kept only in a comment is a
    // rule that gets broken by the next caller.
    node->scale = (node->kind == NodeKind::Group)
                      ? simd_float3{scale.x, scale.x, scale.x}
                      : scale;
}

Node& SceneDocument::add(Node node) {
    nodes_.push_back(std::move(node));
    return nodes_.back();
}

Counters SceneDocument::counters() const {
    Counters c;
    c.next_id = next_id_;
    c.shape_counts = shape_counts_;
    c.group_count = group_count_;
    return c;
}

void SceneDocument::set_counters(const Counters& counters) {
    next_id_ = counters.next_id;
    shape_counts_ = counters.shape_counts;
    group_count_ = counters.group_count;
}

Node& SceneDocument::insert(Node node, size_t index) {
    const size_t at = std::min(index, nodes_.size());
    return *nodes_.insert(nodes_.begin() + static_cast<ptrdiff_t>(at), std::move(node));
}

void SceneDocument::erase(int32_t id) {
    std::erase_if(nodes_, [id](const Node& node) { return node.id == id; });
}

void SceneDocument::reorder(const std::vector<int32_t>& ids) {
    std::vector<Node> out;
    out.reserve(nodes_.size());
    for (const int32_t id : ids) {
        for (Node& node : nodes_) {
            if (node.id == id) {
                out.push_back(std::move(node));
                node.id = kInvalidNode; // claimed; the sweep below skips it
                break;
            }
        }
    }
    // Anything `ids` did not name keeps its relative order at the end, so a
    // stale sequence degrades into a partial reorder rather than dropping
    // nodes on the floor.
    for (Node& node : nodes_) {
        if (node.id != kInvalidNode) {
            out.push_back(std::move(node));
        }
    }
    nodes_ = std::move(out);
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
    // contact point, so the placement and shape gizmos coincide until the detail
    // is deliberately lifted off.
    //
    // `parent` is left at Kind::World, deliberately. The node RESTS ON the
    // surface it was placed against without being expressed in its frame, so
    // nothing propagates and dragging that surface still leaves this behind --
    // the same behaviour as before the hierarchy existed. Since the node is
    // world-rooted, the contact's parent-frame coordinates are world
    // coordinates, which is exactly what snap_point used to hold.
    node.local_position = hit;
    node.contact.valid = true;
    node.contact.surface = parent_id;
    node.contact.point = hit;
    node.contact.normal = unit_normal;
    return add(std::move(node)).id;
}

int32_t SceneDocument::add_group() {
    Node node;
    node.id = next_id_++;
    node.kind = NodeKind::Group;
    node.name = "Group " + std::to_string(++group_count_);
    return add(std::move(node)).id;
}

int32_t SceneDocument::spawn_unsnapped(Shape shape, Op op, simd_float3 position) {
    Node node = make_node(shape, op);
    node.local_position = position;
    return add(std::move(node)).id; // world-rooted, resting on nothing
}

bool SceneDocument::attach(int32_t id, ParentRef parent, bool preserve_world_transform) {
    Node* node = find(id);
    if (node == nullptr) {
        return false;
    }
    if (parent.kind == ParentRef::Kind::Node) {
        if (parent.node == id) {
            return false; // a node cannot be its own parent
        }
        if (find(parent.node) == nullptr) {
            return false; // parent must exist
        }
        // Walk up from the PROPOSED parent. If the chain reaches `id`, adopting
        // it would close a loop. Bounded by kMaxParentDepth so an already-cyclic
        // document cannot hang this check either.
        const Node* cur = find(parent.node);
        for (int depth = 0; cur != nullptr && depth <= kMaxParentDepth; ++depth) {
            if (cur->id == id) {
                return false; // would create a cycle
            }
            if (cur->parent.kind != ParentRef::Kind::Node) {
                break;
            }
            cur = find(cur->parent.node);
        }
    }

    // Captured BEFORE the parent changes, since this is what is being kept.
    const NodePlacement before = placement(id);
    node->parent = std::move(parent);
    if (!preserve_world_transform) {
        return true;
    }

    const Frame pf = parent_frame(id);
    const Frame local = relative_to(pf, before.frame);
    node->local_position = local.position;
    node->local_rotation = local.rotation;

    // SIZE, not just pose. A Shape contributes exactly 1 to uniform_scale by
    // construction, so hanging it under a Group scaled 4x would quadruple the
    // box it renders and picks against -- a very visible move, on an operation
    // whose whole promise is that nothing moves. The inherited factor is a
    // single scalar, so dividing it out of the node's own scale is exact.
    //
    // Works for a Group too, and for the same expression: changing its scale
    // changes its own contribution by precisely that factor.
    const float after = placement(id).frame.uniform_scale;
    if (after > 1e-6f && std::isfinite(after)) {
        node->scale *= before.frame.uniform_scale / after;
    }

    // The contact is a fact about a SURFACE, and surfaces do not move because
    // something was re-parented. Its point and normal are stored in the
    // PARENT's frame, so leaving them alone would silently re-read them against
    // a different frame -- anchoring the Placement gizmo, and the rotate drag's
    // pivot, somewhere nowhere near the skin the node rests on.
    if (node->contact.valid && before.contact.has_value()) {
        node->contact.point = inverse_transform_point(pf, before.contact->point);
        node->contact.normal = inverse_transform_direction(pf, before.contact->normal);
    }
    return true;
}

void SceneDocument::detach(int32_t id) {
    attach(id, ParentRef{}, /*preserve_world_transform=*/true);
}

void SceneDocument::remove_node(int32_t id, OrphanPolicy policy) {
    if (find(id) == nullptr) {
        return; // unknown id: nothing to remove, and no orphans to consider
    }

    if (policy == OrphanPolicy::Cascade) {
        // Collect the subtree before erasing anything, because detecting
        // descendants needs the chain intact. Repeated sweeps rather than a
        // child index: the node count is in the tens, and a stale index is a
        // whole class of bug this does not need.
        std::vector<int32_t> doomed{id};
        for (bool grew = true; grew;) {
            grew = false;
            for (const Node& node : nodes_) {
                if (node.parent.kind != ParentRef::Kind::Node) {
                    continue;
                }
                const bool parent_doomed =
                    std::find(doomed.begin(), doomed.end(), node.parent.node) != doomed.end();
                const bool already =
                    std::find(doomed.begin(), doomed.end(), node.id) != doomed.end();
                if (parent_doomed && !already) {
                    doomed.push_back(node.id);
                    grew = true;
                }
            }
        }
        std::erase_if(nodes_, [&doomed](const Node& node) {
            return std::find(doomed.begin(), doomed.end(), node.id) != doomed.end();
        });
        // Contacts resting on ANY removed node dangle, not just on `id`.
        for (Node& node : nodes_) {
            if (std::find(doomed.begin(), doomed.end(), node.contact.surface) != doomed.end()) {
                node.contact = Contact{};
            }
        }
        return;
    }

    // Reparent: every direct child is re-rooted FIRST, while the parent still
    // exists -- detach solves the world pose against a frame that is about to
    // go away, so the order is load-bearing.
    std::vector<int32_t> children;
    for (const Node& node : nodes_) {
        if (node.parent.kind == ParentRef::Kind::Node && node.parent.node == id) {
            children.push_back(node.id);
        }
    }
    for (const int32_t child : children) {
        detach(child);
    }

    std::erase_if(nodes_, [id](const Node& node) { return node.id == id; });

    // Fix up survivors that were resting ON the just-removed node: leaving
    // contact.surface pointing at a dead id would be a dangling reference. The
    // gizmo frame keys off contact.valid, so clearing it here drops the orphan
    // back to its own local axes, exactly like a node that rested on nothing.
    for (Node& node : nodes_) {
        if (node.contact.surface == id) {
            node.contact = Contact{};
        }
    }
}

} // namespace sq
