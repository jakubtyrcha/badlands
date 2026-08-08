#pragma once
#include <simd/simd.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <shapeshifter/ShapeshifterCore.h>  // Shape, Op, kInvalidNode

#include "frame.h"  // Frame, NodePlacement, WorldContact

namespace sq {

// Fixed camera-ray distance an unsnapped spawn (ray miss) lands at.
inline constexpr float kUnsnappedSpawnDistance = 6.0f;

// What a node IS, which decides what it contributes and what it propagates.
//
// Shape: everything the editor has had until now. Packs into an SdfNode, and
// its `scale` is BOX DATA -- the size of its own bounding box -- which is never
// inherited by its children. That is Maya's segmentScaleCompensate default, and
// it is what keeps stretching a skull from smearing the horn attached to it.
//
// Group: no box, no SDF contribution, skipped entirely by pack_scene. Carries a
// rigid frame plus one UNIFORM scale, and that scale DOES propagate -- so "make
// the whole head 20% bigger" has a home that is not a shape's bounding box.
// Non-uniform group scale is not merely unimplemented but impossible: S*R is a
// similarity only when S is uniform or R is an axis permutation, and anything
// else is shear, which SdfNode cannot hold.
enum class NodeKind : int32_t { Shape = 0, Group = 1 };

// WHOSE FRAME a node's local transform is expressed in.
//
// Deliberately a sum type rather than a bare parent id, because the third case
// is not a node at all: a node may hang off an EXTERNAL rig attachment, named
// rather than indexed. That is the contract the engine already has --
// AnimationSet::FindAttachment resolves joints and sockets in one namespace and
// nothing public can ask which it found -- and matching it is what keeps a
// socket that later becomes a real joint from changing anything here.
struct ParentRef {
    enum class Kind : int32_t { World = 0, Node = 1, Attachment = 2 };
    Kind kind = Kind::World;
    int32_t node = kInvalidNode;  // Kind::Node only
    std::string attachment;       // Kind::Attachment only -- a NAME, resolved by a FrameProvider
};

// The surface a node was placed on.
//
// A DIFFERENT RELATION FROM `parent`, and keeping the two apart is the point.
// `parent` says whose frame this node's transform is in and governs where it
// ends up; `contact` says what it rests against and governs where the Placement
// gizmo anchors. The editor has always had both -- snap_parent propagated
// nothing while snap_point drove the gizmo -- and naming them separately is
// what lets either change without dragging the other along.
//
// `point` and `normal` are in the PARENT's frame, so they ride whatever the
// parent does and need no fixing up when it moves. For a world-rooted node that
// is world space, which is exactly what the four snap_* fields used to mean.
//
// attach() is then a one-line idea: set parent := Node(contact.surface), and
// re-express the local transform so the world pose does not move.
struct Contact {
    bool valid = false;
    int32_t surface = kInvalidNode;
    simd_float3 point = {0, 0, 0};
    simd_float3 normal = {0, 1, 0};
};

struct Node {
    int32_t id = kInvalidNode;
    std::string name;
    NodeKind kind = NodeKind::Shape;

    ParentRef parent;
    Contact contact;

    // PARENT-LOCAL, not world. Named so on purpose: the rename is what makes
    // every consumer that used to compose placement out of these fields fail to
    // compile, rather than silently keep meaning world space. Ask the document
    // (SceneDocument::placement) for where a node actually is.
    simd_float3 local_position = {0, 0, 0};
    // Unit quaternion, editable via the placement gizmo's rotation rings. The
    // ring drag renormalizes on every update, and so does compose(), which
    // pack_scene relies on: it packs the CONJUGATE as the inverse, and those
    // agree only for a unit quaternion.
    simd_quatf local_rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);

    // Shape: per-axis box size, applied in the node's OWN local space before its
    // rotation -- so stretching one shape along its own axis is exact, rotated
    // or not, and never becomes shear. Never inherited by children.
    // Group: constrained UNIFORM by SceneDocument::set_node_scale, and only that
    // scalar propagates. A Group with {1,2,1} has no defined meaning in compose.
    simd_float3 scale = {1, 1, 1};

    // Shape only; meaningless on a Group.
    Shape shape = Shape::Cube;
    Op op = Op::Add;
    // The shape's one profile parameter -- see ShapeParamSpec. Meaningless for
    // a shape whose spec has no param, and left at 0 for those. make_node
    // seeds it from the spec's default; Editor::setNodeShapeParam is the only
    // thing that should write it afterwards, because it is also what clamps
    // and snaps.
    float shape_param = 0.0f;
};

class SceneDocument {
public:
    Node* find(int32_t id);
    const Node* find(int32_t id) const;
    const std::vector<Node>& nodes() const { return nodes_; }

    // WHERE A NODE IS. The only way to ask, and deliberately the only way:
    // consumers that compose placement out of a Node's own fields are what make
    // nested transforms impossible, because there is no seam for a parent's
    // contribution to enter through.
    //
    // Walks the parent chain and composes. The result is a similarity by
    // construction (Frame cannot spell anything else), so no depth of parenting
    // can produce something SdfNode is unable to render.
    //
    // An unknown id yields a default NodePlacement (identity frame, no contact),
    // which is the same degrade-don't-crash rule nodePosition and nodeScale
    // already follow. So does a chain that exceeds kMaxParentDepth, which only a
    // cycle can reach -- attach() rejects those, so getting there means someone
    // wrote parent fields directly.
    NodePlacement placement(int32_t id) const;

    // The frame a node's LOCAL transform is expressed in -- its parent's
    // resolved frame, or identity when the node is world-rooted. What a caller
    // that solved something in world space (every gizmo drag does) needs in
    // order to store the answer, via relative_to.
    Frame parent_frame(int32_t id) const;

    // The chain-length bound. A real hierarchy is a handful deep; this is a
    // CYCLE GUARD, not a design limit, which is why exceeding it is treated as
    // corruption rather than as a supported case.
    static constexpr int kMaxParentDepth = 64;

    // The only way to write a node's scale, because a Group's must stay UNIFORM
    // and a rule that is merely documented is a rule that gets broken. On a
    // Group all three components are taken from `scale.x`; on a Shape the vector
    // is written through as given.
    void set_node_scale(int32_t id, simd_float3 scale);

    // Direct add for tests; spawn_snapped/spawn_unsnapped below are the real
    // spawn entry points (id/name allocation, contact bookkeeping).
    Node& add(Node node);

    // Creates a Group at the origin: no shape, no box, nothing packed. Named
    // "Group N" off its own counter, since a Group has no row in the per-shape
    // table. World-rooted and unit-scaled; the caller places it.
    //
    // No UI creates one yet -- this exists so the propagation rule a Group
    // carries is exercised rather than merely written down.
    int32_t add_group();

    // hit/unit_normal: world-space raycast_scene hit; the new node is CENTRED
    // on the surface (local_position = hit), so it is half-embedded in whatever
    // it was placed on and its centre coincides with its contact point.
    // parent_id: the node that was hit, recorded as contact.surface.
    //
    // DOES NOT SET `parent`. The node stays world-rooted, so nothing propagates
    // and dragging the surface still leaves its details behind -- exactly as
    // before. The hierarchy is modelled and tested here but deliberately inert
    // until there is a UI to attach with; pack_baseline_tests is what proves it.
    int32_t spawn_snapped(Shape shape, Op op, simd_float3 hit, simd_float3 unit_normal, int32_t parent_id);
    // position: world-space placement as given (e.g. a fixed distance along
    // the spawn ray on a raycast miss). No contact, and world-rooted.
    int32_t spawn_unsnapped(Shape shape, Op op, simd_float3 position);

    // Removes the node with this id from the flat vector; unknown id is a
    // no-op. Per-shape name counters are NOT reset or decremented, so a later
    // spawn of that shape always gets the next number, never a reused name.
    //
    // Survivors resting ON the removed node have their contact invalidated: a
    // contact.surface pointing at a dead id would be a dangling reference, and
    // the Placement gizmo keys off contact.valid, so clearing it drops the
    // orphan back to its own local axes like a node that never rested on
    // anything. The orphan-POLICY overload (children in the transform sense)
    // arrives with attach/detach.
    void remove_node(int32_t id);

private:
    // The frame a node's own locals contribute: its local position and rotation,
    // plus its uniform scale -- which is its scale.x for a Group and exactly 1
    // for a Shape. That single ternary is the whole propagation rule.
    Frame local_frame(const Node& node) const;

    // The shared chain walk behind placement() and parent_frame(). Reports an
    // unresolvable attachment name and a cyclic chain separately, because the
    // two degrade differently: an unresolved binding is world-rooted and
    // usable, a cycle is corruption and yields nothing.
    Frame resolve_parent_frame(const Node& node, bool& binding_resolved, bool& cyclic) const;

    // Allocates id (next_id_++) and an auto name ("<Shape> <per-shape count>",
    // counting spawns of that shape independent of ids/removals); shape, op
    // and shape_param (from the shape's spec default) set; transform, parent and
    // contact left at Node's defaults for the caller to fill in as needed.
    Node make_node(Shape shape, Op op);

    std::vector<Node> nodes_;
    int32_t next_id_ = 1;
    // One counter per shape, indexed by the enum's own value. Was a pair of
    // named ints back when there were two shapes; a table is what keeps adding
    // a ninth shape from being an edit in three places.
    std::array<int32_t, kShapeCount> shape_counts_ = {};
    // Groups are not Shapes and have no row in the table above, so they count
    // separately -- "Group 1", "Group 2".
    int32_t group_count_ = 0;
};

} // namespace sq
