#pragma once
#include <simd/simd.h>
#include <optional>
#include <string>

namespace sq {

// A resolved world placement: position, rotation, and ONE uniform scalar.
//
// This is the most general transform SdfNode can represent, and therefore the
// only thing the document's resolver ever produces. SdfNode is pos_shape +
// inv_rotation + half_extents_op -- a quaternion and an axis-aligned box, with
// no matrix anywhere -- so a world transform carrying shear, or non-uniform
// scale in the FRAME, has nowhere to go.
//
// Deliberately not a simd_float4x4. A matrix would let shear be spelled, and
// then every consumer downstream would need to defend against a transform it
// cannot render. Making the type unable to express it is cheaper than checking.
//
// Note what this does NOT constrain: a Shape's own non-uniform scale, which
// lives in half_extents and is applied in the node's own local space BEFORE its
// rotation. Stretching one shape along its own axis is exact, rotated or not.
// Only INHERITING non-uniform scale through a rotation is impossible, because
// S*R is a similarity solely when S is uniform or R is an axis permutation.
struct Frame {
    simd_float3 position = {0, 0, 0};
    simd_quatf  rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);
    float       uniform_scale = 1.0f;
};

// A node's contact with the surface it was placed on, lifted into world space.
// What the Placement gizmo anchors to.
struct WorldContact {
    simd_float3 point = {0, 0, 0};
    simd_float3 normal = {0, 1, 0};
};

// Everything a consumer needs in order to place a node, resolved by the
// document. Nothing downstream reads a Node's transform fields -- it asks for
// one of these, which is what makes the storage behind them a private detail.
struct NodePlacement {
    Frame frame;
    // World half-extents: abs(node.scale) * 0.5 * frame.uniform_scale. Zero for
    // a Group, which has no box and contributes no SDF.
    simd_float3 half_extents = {0, 0, 0};
    // nullopt when the node rests on nothing.
    std::optional<WorldContact> contact;
    // False when the node names an attachment no provider resolves; the node is
    // then treated as world-rooted. Queryable rather than silent, so a later UI
    // can say "this binding is broken" instead of leaving a node at the origin
    // with no explanation.
    bool binding_resolved = true;
};

// Compose child-in-parent. The single definition of the propagation rule:
//
//   rotation      = parent.rotation * local.rotation      (renormalized)
//   position      = parent.position + parent.rotation * (parent.uniform_scale * local.position)
//   uniform_scale = parent.uniform_scale * local.uniform_scale
//
// Renormalized because pack_scene packs the CONJUGATE as the inverse, and those
// two agree only for a unit quaternion (see sdf.cpp). A chain of quaternion
// products drifts off unit; this is the one place that drift can be stopped, so
// it is stopped here rather than re-established per node per frame.
Frame compose(const Frame& parent, const Frame& local);

// A POINT through a frame: rotated, scaled, and translated. Exactly what
// compose does to a child's local position, factored out so the contact point --
// which rides its PARENT's frame rather than the node's own -- cannot drift from
// the rule the transform chain uses.
simd_float3 transform_point(const Frame& f, simd_float3 p);

// A DIRECTION through a frame: rotated only. No translation, and no scale
// either, so a unit normal stays unit -- which the contact normal must, since
// the gizmo builds a tangent basis from it.
simd_float3 transform_direction(const Frame& f, simd_float3 d);

// The inverses of the two above: a world point/direction expressed back in
// `f`. What attach() needs to re-state a contact against a new parent, since a
// surface does not move because something was re-parented. A degenerate
// uniform_scale is treated as 1 rather than dividing the point away.
simd_float3 inverse_transform_point(const Frame& f, simd_float3 p);
simd_float3 inverse_transform_direction(const Frame& f, simd_float3 d);

// The inverse of compose: the local frame that, composed with `parent`, yields
// `world`. What attach() solves to keep a node's world pose while changing
// whose frame its transform is expressed in.
//
//   rotation = conj(parent.rotation) * world.rotation
//   position = conj(parent.rotation) * (world.position - parent.position) / parent.uniform_scale
//
// A parent whose uniform_scale is at or below zero is degenerate and would
// divide the position away; it is treated as 1 rather than producing infinities.
Frame relative_to(const Frame& parent, const Frame& world);

// THE SANITIZATION BOUNDARY, and the only one.
//
// Decomposes a general 4x4 to the nearest similarity: translation, an
// orthonormalized rotation, and one uniform scale (the mean of the basis
// vectors' lengths).
//
// SHEAR AND NON-UNIFORM SCALE ARE DISCARDED, deliberately. Source animation may
// inherently contain both -- a joint matrix out of ozz's LocalToModel routinely
// does -- but the document's evaluation frame is strict position, rotation and
// one uniform scalar before it ever reaches the renderer. Doing that here, once,
// is what keeps every consumer downstream from having to defend against a
// transform SdfNode cannot hold.
//
// Degenerate input (a basis with a near-zero column, or a near-zero mean scale)
// yields identity rotation and unit scale rather than NaNs. Upstream matrices
// are well-formed by construction, so this is defense in depth -- but a NaN here
// would propagate silently into every descendant's frame.
Frame frame_from_matrix(const simd_float4x4& m);

// A source of frames the document does not own.
//
// THE ANIMATION-PREVIEW SEAM. A node may name an external attachment rather than
// another node, and this is what resolves the name. Nothing implements it yet;
// the document holds a null pointer by default and such a node falls back to
// world-rooted.
//
// Named, not indexed, because that is the contract the engine already has:
// AnimationSet::FindAttachment resolves joints and sockets in one namespace, and
// nothing public can ask which it found (src/engine/animation/animation_set.hpp).
// A real provider will be backed by AnimationSet::AttachmentTransform(id, pose),
// whose glm::mat4 crosses frame_from_matrix above on the way in.
class FrameProvider {
public:
    virtual ~FrameProvider() = default;
    // nullopt when this provider does not know the name.
    virtual std::optional<Frame> frame_for_attachment(const std::string& name) const = 0;
};

} // namespace sq
