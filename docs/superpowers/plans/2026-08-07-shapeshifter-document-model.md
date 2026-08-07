# Shapeshifter Document Model, Transform Resolution and History — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Canonical location at execution time:** copy this file to
`docs/superpowers/plans/2026-08-07-shapeshifter-document-model.md` and commit it
(shapeshifter plans live in the repo-root plans dir — see
`docs/superpowers/plans/2026-08-07-shapeshifter-rhi-renderer-port.md`). The spec
it implements is
`editors/shapeshifter/docs/superpowers/specs/2026-08-07-document-model-and-history-design.md`.

---

## Context

Shapeshifter's scene is a flat `std::vector<Node>` where every node stores its
transform in **world space** (`editors/shapeshifter/core/src/scene.h:15`). Seven
consumers compose placement from those fields directly, and four drag solvers
write them. That makes nested transforms impossible: there is no place for a
parent's contribution to enter.

Attachment is a second, smaller tangle. `snapped`, `snap_point`, `snap_normal`
and `snap_parent` are four world-space fields read at exactly three sites, and
`snap_parent` propagates nothing — dragging a cube leaves the details placed on
it behind.

Four capabilities are wanted on top of this — multi-select, undo/redo,
copy/paste, save/load — and three of the four hinge on the same missing thing: a
document model with one mutation seam. This plan builds that foundation and the
undo/redo that falls out of it. The other three are separate specs.

**Intended outcome:** the document gains hierarchy, node kinds, an external
attachment seam designed for animation preview, and undo/redo — while rendering
**exactly the same pixels it renders today**.

---

**Goal:** Replace `Node`'s world-space transform with a parent-local one resolved
by the document, and add interaction-scoped undo/redo on the resulting mutation
seam.

**Architecture:** A new `Frame` type (position + rotation + one uniform scalar —
the most general transform `SdfNode` can represent) plus
`SceneDocument::placement(id)` become the only way to learn where a node is.
Consumers are routed through that accessor *before* the storage changes, so the
build stays green and the behaviour lock stays provable at every step. Above
that sit three separated layers — interactions, temporary state, and serialized
modifications — described in their own section below.

**Tech Stack:** C++20, Apple `simd`, doctest (`shapeshifter_core_tests`), CMake,
Swift/SwiftUI + XcodeGen for the app half.

## Global Constraints

- **Node order is semantic.** `sdf_fold` (`editors/shapeshifter/shaders/sdf_scene.h:574`) reduces in vector order and a Subtract carves everything before it. Hierarchy must never reorder the node vector.
- **`SdfNode` is similarity-only.** `pos_shape` + `inv_rotation` + `half_extents_op`, no matrix. A composed world transform is rotation + translation + one uniform scalar. Shear and non-uniform scale in a *frame* are unrepresentable.
- **The evaluation frame is SANITIZED AT THE BOUNDARY, once.** Source data may carry shear and non-uniform stretch — a rig's joint matrix routinely does — but what enters the document is always position, rotation and one uniform scalar. `frame_from_matrix` is that boundary and the only place the sanitization happens, so no downstream consumer ever has to defend against a transform it cannot represent.
- **Per-node non-uniform scale is unaffected and keeps working.** `Node::scale` is applied in the node's OWN local space, before its rotation (`sdf_eval_node` does `rotate_by_inv(p - pos)` then measures against `half_extents`). Stretching one shape 5x along its own axis is exact, rotated or not. Only *inheriting* non-uniform scale through a rotation is impossible, because `S·R` is a similarity solely when `S` is uniform or `R` is an axis permutation.
- **The parent link is modelled but inert.** `spawn_snapped` must not set `parent`. Every node stays world-rooted; Task 1's gate is what proves it.
- **A Shape's `scale` is box data and is never inherited.** Only a Group's uniform scalar propagates.
- **Approved UI delta, and nothing else:** an Edit menu with Undo/Redo, ⌘Z / ⇧⌘Z, ⌫ / ⌦ to delete. No panel, no outliner, no attach/detach affordance, no group creation.
- **`shapeshifter_core_tests`' source list in `CMakeLists.txt:2552` is spelled out, not globbed.** A new test file is silently not built until added there.
- **`Node::rotation` must stay unit.** `pack_scene` conjugates rather than inverts (`sdf.cpp:61`); `compose` renormalizes for the same reason.
- Build: `scripts/build.sh shapeshifter_core shapeshifter_core_tests`. Test: `scripts/test.sh shapeshifter_core_tests`.

## Architecture: interactions, temporary state, and serialized modifications

Three layers, and keeping them apart is the point.

- **An interaction is a user gesture** with a begin and an end — drag a handle, turn the dial, click to spawn, press ⌫. It is the undo granularity and the *only* thing that decides it. **The app declares every boundary, uniformly**; `beginDrag`/`endDrag` go back to being purely about gizmo state and stop touching history at all. Boundaries are refcounted, so a nested begin never splits an entry.
- **During an interaction the document is mutated live, and that is TEMPORARY state.** The drag solver writes `local_position` on every mouse-move exactly as it does today — no allocation, no recording, no per-event cost.
- **At `endInteraction()` the temporary change is DECOMPOSED into a serialized modification** by diffing the live document against a baseline captured at begin. The delta therefore records the net result of the gesture, never the path the cursor took — one `Move` entry for a drag, whatever its length.

**Modifications never fan out.** Moving a parent records one node's changed local transform; its children move because their world frames are *derived*. The one unavoidable exception, named rather than hidden: `attach(preserve_world_pose)` and `remove_node(Reparent)` genuinely rewrite the affected children's local transforms, because the frame those were expressed in is going away. The diff picks that up as the real edit it is.

### The history record: keyframes plus deltas, and no inverse anywhere

An inverse is the unstable part of a command-undo scheme — float drift on replay, and a `RemoveNode` inverse that must restore the node *at its index* along with every orphan fixup. This design has none.

- Each entry carries either a **Snapshot** or a **Delta**.
- A **Snapshot** is taken every `kSnapshotInterval` entries (**64, configurable**), and whenever the delta would be larger than a snapshot anyway.
- Otherwise a **Delta**: `{added: [{Node, index}], removed: [id], changed: [Node], counters}`. Index is carried on `added` because **node order is semantic** — CSG evaluation folds in vector order.
- **Undo/redo restores the nearest preceding Snapshot and replays Deltas forward** to the target cursor. Worst case is 64 delta applications over a ≤100-node document.
- `counters` carries `next_id_` and the name counters, which are document state — without them a spawn after an undo would reuse an id.
- Selection is per-entry, not per-modification: it is not document state.

Two decisions taken here rather than asked, per the repo's deviation rule:

- **`changed` carries whole `Node` values, not per-field modifications.** A node is ~120 bytes, the whole-node form is trivially correct, and the undo label comes from the interaction rather than from field granularity. Per-field splitting is a later optimisation if delta size ever matters.
- **A debug assert fires when the document is mutated with `interaction_depth == 0`.** With the app owning boundaries, a missed bracket would otherwise be a silently unrecorded edit.

## Spec amendments this plan carries

Two, both agreed after the spec was written. **Amend
`2026-08-07-document-model-and-history-design.md` §4 and §5 to match before Task
8a lands**, so the spec does not describe a design the code no longer has.

1. **§4 is replaced by the interactions / temporary-state / modifications model
   above.** The spec's snapshot-per-edit with RAII `Transaction` becomes:
   app-declared refcounted interactions, live mutation as temporary state, and a
   decomposition at interaction end into a keyframe-and-delta log with no
   inverses. The spec's rationale for rejecting *command* undo still holds — this
   design writes no inverse either — but "a stack of whole-document snapshots" is
   no longer accurate.
2. **§5 gains `beginInteraction` / `endInteraction` / `activeDragHandle`**, and
   loses the claim that `beginDrag`/`endDrag` bracket history.

Separately, a detail decided here rather than asked, per the repo's deviation
rule: the spec sketched three accessors (`world_frame`, `world_half_extents`,
`world_contact`), and this plan consolidates them into **one
`SceneDocument::placement(id)` returning a `NodePlacement`**.
`gizmo_frame_for_node` then needs no `Node` at all, and every consumer takes one
resolved value instead of three.

## File Structure

| File | Responsibility |
|---|---|
| `editors/shapeshifter/core/src/frame.h/.cpp` | **New.** `Frame`, `WorldContact`, `NodePlacement`, `compose`, `frame_from_matrix`, `FrameProvider`. Pure math + one interface. |
| `editors/shapeshifter/core/src/history.h/.cpp` | **New.** `Snapshot`, `Delta`, `Entry`, `History`, and the diff that decomposes a live document into a Delta. |
| `editors/shapeshifter/core/src/scene.h/.cpp` | `NodeKind`, `ParentRef`, `Contact`, parent-local `Node`, `placement()`, `attach`/`detach`/`remove_node(policy)`. |
| `editors/shapeshifter/core/src/{sdf,picking,lines,navigation,gizmo}.{h,cpp}` | Consume `NodePlacement` instead of reading `Node`'s transform. |
| `editors/shapeshifter/core/src/editor.cpp` | Forwards interaction boundaries to `History`; applies a replayed entry. Decides no boundaries itself. |
| `editors/shapeshifter/core/include/shapeshifter/ShapeshifterCore.h` | `undo`/`redo`/`canUndo`/`canRedo`/`undoLabel`/`redoLabel` only. |
| `editors/shapeshifter/app/Sources/{MetalViewport,EditorViewModel,ShapeshifterApp}.swift` | Modifier flags through keyDown; shortcuts; Edit menu. |
| `editors/shapeshifter/tests/core/{pack_baseline,frame,hierarchy,history}_tests.cpp` | **New.** |
| `CMakeLists.txt:2476` and `:2552` | Source lists for the two new core files and four new test files. |

---

### Task 1: The inertness gate

Pins `pack_scene`'s output **before anything changes**, so every later task is
held to it. Must pass on unmodified code.

**Files:**
- Create: `editors/shapeshifter/tests/core/pack_baseline_tests.cpp`
- Modify: `CMakeLists.txt:2552` (add to the `shapeshifter_core_tests` source list, alphabetical: after `navigation_tests.cpp`)

**Interfaces:**
- Consumes: `sq::SceneDocument::spawn_snapped/spawn_unsnapped/add` (`core/src/scene.h`), `sq::pack_scene` (`core/src/sdf.h`), `SdfNode` (`shaders/sdf_scene.h`)
- Produces: nothing — a pure regression lock later tasks must keep green.

- [ ] **Step 1: Write the baseline test**

```cpp
#include <doctest.h>
#include <cmath>
#include <vector>
#include <shapeshifter/ShapeshifterCore.h>
#include "scene.h"
#include "sdf.h"
#include "sdf_scene.h"

using namespace sq;

// Pins pack_scene's OUTPUT BYTES for a document built through the real spawn
// paths. The document model is about to be rebuilt underneath this; the whole
// claim of that rework is that it changes no pixel, and this is the assertion
// that claim reduces to. Every literal here is hand-derived from today's
// semantics -- half_extents = scale * 0.5, inv_rotation = conjugate, op 0/1.
TEST_CASE("pack_scene output is pinned for a spawned document") {
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});
    const int32_t b = doc.spawn_snapped(Shape::Capsule, Op::Subtract,
                                        simd_float3{0, 1, 0}, simd_float3{0, 1, 0}, a);
    REQUIRE(a != kInvalidNode);
    REQUIRE(b != kInvalidNode);

    const std::vector<SdfNode> packed = pack_scene(doc);
    REQUIRE(packed.size() == 2);

    // Node A: Cube (id 0), Add (op 0), unit scale, identity rotation, param 0.
    CHECK(packed[0].pos_shape.x == doctest::Approx(1.0f));
    CHECK(packed[0].pos_shape.y == doctest::Approx(2.0f));
    CHECK(packed[0].pos_shape.z == doctest::Approx(3.0f));
    CHECK(packed[0].pos_shape.w == doctest::Approx(0.0f));
    CHECK(packed[0].half_extents_op.x == doctest::Approx(0.5f));
    CHECK(packed[0].half_extents_op.w == doctest::Approx(0.0f));
    CHECK(packed[0].inv_rotation.w == doctest::Approx(1.0f));
    CHECK(packed[0].params.x == doctest::Approx(0.0f));

    // Node B: Capsule (id 3), Subtract (op 1), spawned CENTRED on the hit,
    // shape_param at the capsule's default of 1.0.
    CHECK(packed[1].pos_shape.x == doctest::Approx(0.0f));
    CHECK(packed[1].pos_shape.y == doctest::Approx(1.0f));
    CHECK(packed[1].pos_shape.z == doctest::Approx(0.0f));
    CHECK(packed[1].pos_shape.w == doctest::Approx(3.0f));
    CHECK(packed[1].half_extents_op.w == doctest::Approx(1.0f));
    CHECK(packed[1].params.x == doctest::Approx(1.0f));
}

// The conjugate-not-inverse contract, and non-uniform scale reaching
// half_extents. A rotated, non-uniformly scaled node is the case the
// similarity-only rework must leave untouched.
TEST_CASE("pack_scene pins the conjugate and non-uniform half-extents") {
    SceneDocument doc;
    Node n;
    n.id = 1;
    n.shape = Shape::Cube;
    n.op = Op::Add;
    n.position = simd_float3{0, 0, 0};
    n.rotation = simd_quaternion(float(M_PI_2), simd_float3{0, 1, 0}); // +90 deg about Y
    n.scale = simd_float3{2.0f, 1.0f, 0.5f};
    doc.add(n);

    const std::vector<SdfNode> packed = pack_scene(doc);
    REQUIRE(packed.size() == 1);

    CHECK(packed[0].half_extents_op.x == doctest::Approx(1.0f));
    CHECK(packed[0].half_extents_op.y == doctest::Approx(0.5f));
    CHECK(packed[0].half_extents_op.z == doctest::Approx(0.25f));

    // conjugate(q) negates the imaginary part: (0, sin(pi/4), 0, cos(pi/4))
    // becomes (0, -sin(pi/4), 0, cos(pi/4)).
    CHECK(packed[0].inv_rotation.y == doctest::Approx(-std::sin(float(M_PI_4))));
    CHECK(packed[0].inv_rotation.w == doctest::Approx(std::cos(float(M_PI_4))));
}
```

- [ ] **Step 2: Add the file to CMake**

In `CMakeLists.txt`, inside `add_executable(shapeshifter_core_tests ...)` at line 2552, add
`${SHAPESHIFTER_DIR}/tests/core/pack_baseline_tests.cpp` after the
`navigation_tests.cpp` entry.

- [ ] **Step 3: Run and verify it PASSES on unmodified code**

Run: `scripts/build.sh shapeshifter_core_tests && scripts/test.sh shapeshifter_core_tests`
Expected: PASS. This test is unusual — it must be green from the moment it is
written, because it describes what already happens. If it fails, a literal is
wrong; fix the literal, not the code.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt editors/shapeshifter/tests/core/pack_baseline_tests.cpp
git commit -m "test(shapeshifter): pin pack_scene output before the document rework"
```

---

### Task 2: `Frame`, composition, and the similarity decomposition

Pure new math. Touches nothing existing.

**Files:**
- Create: `editors/shapeshifter/core/src/frame.h`, `editors/shapeshifter/core/src/frame.cpp`
- Create: `editors/shapeshifter/tests/core/frame_tests.cpp`
- Modify: `CMakeLists.txt:2476` (core sources, alphabetical: before `gizmo.cpp`), `CMakeLists.txt:2552` (test sources)

**Interfaces:**
- Consumes: `simd`, `sq::kInvalidNode`
- Produces: `sq::Frame`, `sq::WorldContact`, `sq::NodePlacement`, `sq::compose(parent, local)`, `sq::frame_from_matrix(m)`, `sq::FrameProvider`

- [ ] **Step 1: Write `frame.h`**

```cpp
#pragma once
#include <simd/simd.h>
#include <optional>
#include <string>

namespace sq {

// The most general transform SdfNode can represent, and therefore the only thing
// the document's resolver ever produces. Deliberately NOT a matrix: shear cannot
// be spelled here, so it cannot enter the document by accident.
struct Frame {
    simd_float3 position = {0, 0, 0};
    simd_quatf  rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);
    float       uniform_scale = 1.0f;
};

// A node's contact with the surface it was placed on, lifted into world space.
struct WorldContact {
    simd_float3 point{0, 0, 0};
    simd_float3 normal{0, 1, 0};
};

// Everything a consumer needs to place a node, resolved by the document. Nothing
// downstream reads a Node's transform fields again -- it asks for one of these.
struct NodePlacement {
    Frame frame;
    // World half-extents: node.scale * 0.5 * frame.uniform_scale. {0,0,0} for a
    // Group, which has no box.
    simd_float3 half_extents{0, 0, 0};
    // nullopt when the node rests on nothing.
    std::optional<WorldContact> contact;
    // false when the node names an attachment no provider resolves; the node is
    // then treated as world-rooted. Queryable so a later UI can say so.
    bool binding_resolved = true;
};

// Compose child-in-parent. The single definition of the propagation rule:
//   rotation      = parent.rotation * local.rotation      (renormalized)
//   position      = parent.position + parent.rotation * (parent.uniform_scale * local.position)
//   uniform_scale = parent.uniform_scale * local.uniform_scale
//
// Renormalized because pack_scene packs the CONJUGATE as the inverse, and those
// agree only for a unit quaternion. A deep chain of quaternion products drifts;
// this is the one place that drift can be stopped.
Frame compose(const Frame& parent, const Frame& local);

// THE SANITIZATION BOUNDARY, and the only one. Decomposes a general 4x4 to the
// nearest similarity: translation, an orthonormalized rotation, and one uniform
// scale (the mean of the basis vector lengths).
//
// SHEAR AND NON-UNIFORM SCALE ARE DISCARDED, deliberately. Source animation may
// inherently contain both -- a joint matrix out of LocalToModel routinely does --
// but the document's evaluation frame is strict position, rotation and one
// uniform scalar before it ever reaches the renderer. Doing that here, once,
// is what keeps every downstream consumer from having to defend against a
// transform SdfNode cannot hold.
//
// Degenerate (near-zero determinant) input yields identity rotation and unit
// scale rather than NaNs.
Frame frame_from_matrix(const simd_float4x4& m);

// A source of frames the document does not own. The animation-preview seam:
// nothing implements this yet, and the document holds a null pointer by default.
class FrameProvider {
public:
    virtual ~FrameProvider() = default;
    // nullopt when this provider does not know the name.
    virtual std::optional<Frame> frame_for_attachment(const std::string& name) const = 0;
};

} // namespace sq
```

- [ ] **Step 2: Write `frame_tests.cpp` (failing)**

Cases, each a `TEST_CASE`:
- `"compose with an identity parent returns the local frame"`
- `"compose is associative"` — `compose(compose(a,b),c)` vs `compose(a,compose(b,c))`, three frames with rotation, offset and scale ≠ 1, compared component-wise with `doctest::Approx`.
- `"a parent's uniform scale scales the child's offset"` — parent `{pos {0,0,0}, identity, 2.0f}`, local `{pos {1,0,0}, identity, 1.0f}` → position `{2,0,0}`, `uniform_scale == 2`.
- `"a parent's rotation swings the child's offset"` — parent rotated +90° about Y, local offset `{1,0,0}` → `{0,0,-1}` (right-handed, Y up).
- `"compose renormalizes"` — feed a deliberately denormalized local rotation (`q.vector *= 1.01f`); assert `simd_length(out.rotation.vector) == Approx(1.0f)`.
- `"frame_from_matrix recovers a rigid transform exactly"` — build with `trs_matrix` (`math_util.h:21`) at uniform scale, round-trip.
- `"frame_from_matrix discards shear"` — build a matrix whose basis is non-orthogonal; assert the result's rotation basis is orthonormal (pairwise dots ≈ 0, lengths ≈ 1).
- `"frame_from_matrix collapses non-uniform scale to a scalar"` — `trs_matrix(p, identity, {2,4,8})` → `uniform_scale == Approx((2+4+8)/3)`.
- `"frame_from_matrix survives a degenerate matrix"` — all-zero basis → identity rotation, `uniform_scale == 1`, no NaN.

- [ ] **Step 3: Run, verify FAIL** (`frame.cpp` does not exist / link error)

Run: `scripts/build.sh shapeshifter_core_tests`
Expected: build failure — no `frame.cpp`.

- [ ] **Step 4: Implement `frame.cpp`**

`compose` per the header's formula, `simd_normalize` on the product quaternion.
`frame_from_matrix`: take `m.columns[3].xyz` as position; take the three basis
columns' lengths, average for `uniform_scale`; Gram-Schmidt the basis to
orthonormal and convert with `simd_quaternion(simd_float3x3)`; guard
`uniform_scale > 1e-6f` and a non-degenerate first column, else return a default
`Frame`.

- [ ] **Step 5: Add both files to CMake, run, verify PASS**

Run: `scripts/build.sh shapeshifter_core_tests && scripts/test.sh shapeshifter_core_tests`
Expected: PASS, including Task 1's baseline.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt editors/shapeshifter/core/src/frame.h \
        editors/shapeshifter/core/src/frame.cpp \
        editors/shapeshifter/tests/core/frame_tests.cpp
git commit -m "feat(shapeshifter): add Frame, similarity composition and the FrameProvider seam"
```

---

### Task 3: `SceneDocument::placement()`, backed by today's fields

Introduces the accessor **before** the storage changes, so the sweep in Task 4
and the storage swap in Task 5 are separable and each keeps the build green.

**Files:**
- Modify: `editors/shapeshifter/core/src/scene.h`, `editors/shapeshifter/core/src/scene.cpp`
- Modify: `editors/shapeshifter/tests/core/scene_tests.cpp`

**Interfaces:**
- Consumes: `sq::Frame`, `sq::NodePlacement` (Task 2)
- Produces: `NodePlacement SceneDocument::placement(int32_t id) const` — the sole placement resolver from here on. Unknown id returns a default-constructed `NodePlacement`.

- [ ] **Step 1: Write the failing test** in `scene_tests.cpp`

```cpp
TEST_CASE("placement reports a spawned node's world frame") {
    SceneDocument doc;
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});

    const NodePlacement p = doc.placement(id);
    check_float3_approx(p.frame.position, simd_float3{1, 2, 3});
    CHECK(p.frame.uniform_scale == doctest::Approx(1.0f));
    check_float3_approx(p.half_extents, simd_float3{0.5f, 0.5f, 0.5f});
    CHECK_FALSE(p.contact.has_value());
    CHECK(p.binding_resolved);
}

TEST_CASE("placement reports a snapped node's contact in world space") {
    SceneDocument doc;
    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t b = doc.spawn_snapped(Shape::Sphere, Op::Add,
                                        simd_float3{0, 0.5f, 0}, simd_float3{0, 1, 0}, a);

    const NodePlacement p = doc.placement(b);
    REQUIRE(p.contact.has_value());
    check_float3_approx(p.contact->point, simd_float3{0, 0.5f, 0});
    check_float3_approx(p.contact->normal, simd_float3{0, 1, 0});
}

TEST_CASE("placement of an unknown id is the default") {
    SceneDocument doc;
    const NodePlacement p = doc.placement(kInvalidNode);
    check_float3_approx(p.frame.position, simd_float3{0, 0, 0});
    CHECK_FALSE(p.contact.has_value());
}
```

- [ ] **Step 2: Run, verify FAIL** — `placement` undeclared.

- [ ] **Step 3: Implement** `placement()` in `scene.cpp`: look the node up; fill
`frame` from `node.position`/`node.rotation` with `uniform_scale = 1.0f`;
`half_extents = simd_abs(node.scale) * 0.5f`; `contact` from the `snapped` /
`snap_point` / `snap_normal` triple. Declare it in `scene.h` with a comment
saying it is the only way to learn where a node is.

- [ ] **Step 4: Run, verify PASS** (`scripts/test.sh shapeshifter_core_tests`)

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(shapeshifter): add SceneDocument::placement, the single placement resolver"
```

---

### Task 4: Route every consumer through `placement()`

The mechanical sweep. **No behaviour change** — Task 1's gate and every existing
suite must stay green with unmodified expectations.

**Files:**
- Modify: `core/src/sdf.cpp` (`pack_scene`, `scene_aabb`, `local_sdf_node`), `core/src/picking.{h,cpp}` (`raycast_node`, `raycast_scene`), `core/src/lines.{h,cpp}` (`append_node_wireframe`, `build_scene_lines`), `core/src/navigation.{h,cpp}` (`node_bounding_radius`), `core/src/gizmo.{h,cpp}` (`gizmo_frame_for_node`), `core/src/editor.cpp` (call sites)
- Modify: `tests/core/{picking,lines,gizmo,sdf,navigation,drag}_tests.cpp` — updated for the new signatures only

**Interfaces:**
- Consumes: `NodePlacement SceneDocument::placement(int32_t)` (Task 3)
- Produces, the new signatures every later task and test uses:

```cpp
// picking.h
std::optional<RayHit> raycast_node(const Node& node, const NodePlacement& placement,
                                   const Ray& world);
// lines.h
void append_node_wireframe(std::vector<LineVertex>& out, const Node& node,
                           const NodePlacement& placement, simd_float4 color,
                           simd_float3 eye_world);
// navigation.h
float node_bounding_radius(const Node& node, const NodePlacement& placement);
// gizmo.h -- no longer takes a Node at all
GizmoFrame gizmo_frame_for_node(const NodePlacement& placement, const Camera& camera,
                                GizmoSlot slot);
// sdf.h
SdfNode local_sdf_node(const Node& node, simd_float3 world_half_extents);
```

- [ ] **Step 1: Change the five signatures and fix every call site**

Guidance per site:
- `pack_scene`: `const NodePlacement p = doc.placement(node.id);` then write
  `p.frame.position`, `simd_conjugate(p.frame.rotation)`, `p.half_extents`.
- `raycast_node`: replace `node.rotation`/`node.position` with
  `placement.frame.rotation`/`.position`, and pass `placement.half_extents` to
  `local_sdf_node`. The rigid world→local reasoning in its header comment
  (`picking.h:33`) is unchanged and still correct.
- `append_node_wireframe`: build `m` from
  `trs_matrix(p.frame.position, p.frame.rotation, simd_abs(node.scale) * p.frame.uniform_scale)`
  and `half` from `p.half_extents`. Keep the `simd_abs` note at `lines.cpp:707` —
  it is still load-bearing.
- `node_bounding_radius`: `const simd_float3 half = placement.half_extents;`
- `gizmo_frame_for_node`: `node.snapped` → `placement.contact.has_value()`;
  `node.snap_point`/`snap_normal` → `placement.contact->point`/`->normal`;
  `node.position`/`node.rotation` → `placement.frame.position`/`.rotation`.
- `editor.cpp`: every `gizmo_frame_for_node(*node, camera, slot)` becomes
  `gizmo_frame_for_node(impl_->scene.placement(node->id), camera, slot)`. Resolve
  the placement ONCE per call site and reuse it for both slots.

- [ ] **Step 2: Update the tests for signatures only**

Tests that construct a bare `Node` now build a `NodePlacement` beside it. Add a
helper to each affected file rather than a shared header (matches the existing
per-file `check_float3_approx` convention):

```cpp
// A NodePlacement for a stand-alone Node, as the document would resolve it.
NodePlacement placement_of(const Node& n) {
    NodePlacement p;
    p.frame.position = n.position;
    p.frame.rotation = n.rotation;
    p.half_extents = simd_abs(n.scale) * 0.5f;
    if (n.snapped) { p.contact = WorldContact{n.snap_point, n.snap_normal}; }
    return p;
}
```

**No test EXPECTATION changes.** If one has to change, stop — that is a
regression, not a migration.

- [ ] **Step 3: Run the full suite, verify PASS**

Run: `scripts/build.sh shapeshifter_core shapeshifter_core_tests && scripts/test.sh shapeshifter_core_tests`
Expected: PASS, Task 1's baseline included.

- [ ] **Step 4: Verify the renderer suite too**

Run: `scripts/test.sh shapeshifter_rhi_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git commit -am "refactor(shapeshifter): route every consumer through SceneDocument::placement"
```

---

### Task 5: Swap the storage — node kinds, parent-local transforms, real resolution

The semantic change. Consumers already go through `placement()`, so this touches
`scene.{h,cpp}` and the three former snap sites only.

**Files:**
- Modify: `editors/shapeshifter/core/src/scene.h`, `editors/shapeshifter/core/src/scene.cpp`
- Create: `editors/shapeshifter/tests/core/hierarchy_tests.cpp`
- Modify: `CMakeLists.txt:2552`; `tests/core/scene_tests.cpp` and any test writing `snap_*` directly

**Interfaces:**
- Consumes: `compose` (Task 2), `placement()` (Task 3)
- Produces: `sq::NodeKind`, `sq::ParentRef`, `sq::Contact`, and `Node`'s renamed
  `local_position` / `local_rotation`. `placement()` now walks the parent chain.

- [ ] **Step 1: Write the failing hierarchy tests** in `hierarchy_tests.cpp`

```cpp
// A Group's uniform scale propagates; a Shape's box never does. That asymmetry
// is the whole propagation rule -- Maya's segmentScaleCompensate default, and
// the only version representable by SdfNode.
TEST_CASE("a Group's uniform scale scales its child's offset and box") {
    SceneDocument doc;
    Node g;
    g.id = 1; g.kind = NodeKind::Group;
    g.local_position = simd_float3{0, 0, 0};
    g.scale = simd_float3{2, 2, 2};        // uniform, as a Group must be
    doc.add(g);

    Node c;
    c.id = 2; c.kind = NodeKind::Shape; c.shape = Shape::Cube;
    c.parent.kind = ParentRef::Kind::Node;
    c.parent.node = 1;
    c.local_position = simd_float3{1, 0, 0};
    c.scale = simd_float3{1, 1, 1};
    doc.add(c);

    const NodePlacement p = doc.placement(2);
    check_float3_approx(p.frame.position, simd_float3{2, 0, 0});   // offset scaled
    CHECK(p.frame.uniform_scale == doctest::Approx(2.0f));
    check_float3_approx(p.half_extents, simd_float3{1, 1, 1});     // 1 * 0.5 * 2
}

TEST_CASE("a Shape parent's non-uniform scale reaches its child in no way") {
    SceneDocument doc;
    Node s;
    s.id = 1; s.kind = NodeKind::Shape; s.shape = Shape::Cube;
    s.scale = simd_float3{5, 1, 1};        // a stretched skull
    doc.add(s);

    Node horn;
    horn.id = 2; horn.kind = NodeKind::Shape; horn.shape = Shape::Cone;
    horn.parent.kind = ParentRef::Kind::Node;
    horn.parent.node = 1;
    horn.local_position = simd_float3{1, 0, 0};
    horn.scale = simd_float3{1, 1, 1};
    doc.add(horn);

    const NodePlacement p = doc.placement(2);
    check_float3_approx(p.frame.position, simd_float3{1, 0, 0});   // NOT {5,0,0}
    CHECK(p.frame.uniform_scale == doctest::Approx(1.0f));
    check_float3_approx(p.half_extents, simd_float3{0.5f, 0.5f, 0.5f});
}

TEST_CASE("a parent's rotation swings its child") { /* parent +90 deg about Y,
    child offset {1,0,0} -> world {0,0,-1} */ }

TEST_CASE("Group scale composes multiplicatively through a chain") { /* Group 2x
    -> Group 3x -> Shape: uniform_scale == 6 */ }

TEST_CASE("a hand-built cycle terminates and yields identity") { /* two nodes
    parented to each other by direct field write, bypassing attach(); placement()
    hits the depth cap, returns a default Frame, does not hang */ }

TEST_CASE("the contact rides the parent frame") { /* parented child with a
    contact; assert placement().contact->point is the parent frame applied to
    contact.point, and the normal is rotated only and still unit */ }
```

- [ ] **Step 2: Run, verify FAIL** — `NodeKind`, `ParentRef`, `local_position` undeclared.

- [ ] **Step 3: Rewrite `Node` and `placement()` in `scene.h`/`scene.cpp`**

Add `NodeKind`, `ParentRef` and `Contact` exactly as the spec's §1 defines them.
Rename `position`/`rotation` → `local_position`/`local_rotation`. Delete
`snapped`, `snap_point`, `snap_normal`, `snap_parent` and
`Node::world_from_local()`. Implement `placement()` as a parent-chain walk with a
**depth cap of 64**, composing with `compose()`; a Shape contributes
`uniform_scale = 1.0f` to its own local frame, a Group contributes `scale.x`.

`spawn_snapped` fills `contact` (`valid`, `surface`, `point`, `normal`) and
**leaves `parent` at `Kind::World`** — the inertness ruling. `remove_node`'s
existing orphan cleanup becomes "invalidate any `contact` whose `surface` is the
removed id"; the policy overload arrives in Task 6.

Add a Group name counter beside `shape_counts_` so a Group is named "Group 1".

**Three invariants to hold while doing this:**

- **A Group's scale must be kept uniform by a setter, not merely documented.**
  A Group with `{1, 2, 1}` has no defined meaning in `compose`. Add
  `void SceneDocument::set_node_scale(int32_t id, simd_float3 scale)` which
  writes all three components from `scale.x` when the node is a Group, and route
  the scale drag (`editor.cpp:653`) through it. Add a test:
  `"a Group's scale is forced uniform"`.
- **The drag writes `local_position` and never `contact`.** `updateDrag`'s
  comment at `editor.cpp:720` explains why the attachment stays on the skin when
  a node is pulled off it — that offset is exactly what the tether reports. The
  rename must not quietly change which field the drag touches.
- **No `Node*` may outlive a mutation.** Today's code re-finds per call
  (`updateDrag` looks the node up every time); keep it that way, because Task 9's
  `undo` assigns over the whole document and invalidates every pointer into it.

- [ ] **Step 4: Fix the compile errors the rename produces**

The rename is the forcing function: every stale reader fails to compile. Work
through them; `placement()` is the answer at almost every site. Test files
writing `snap_*` directly switch to `contact`, and each file's `placement_of`
helper from Task 4 updates to `n.local_position` / `n.local_rotation` /
`n.contact`.

Check `gizmos_coalesce` and the tether (`rhi_renderer.cpp:206`) explicitly: both
read the two gizmo origins, which now come from `placement().contact` rather than
`node.snap_point`. For a world-rooted node their behaviour must be unchanged.

- [ ] **Step 5: Run the full suite, verify PASS — the gate especially**

Run: `scripts/build.sh shapeshifter_core shapeshifter_core_tests && scripts/test.sh shapeshifter_core_tests`
Expected: PASS. **Task 1's `pack_baseline_tests` passing here is the proof that
the model changed and the pixels did not.** If it fails, the inertness ruling has
been broken — stop and find out where.

- [ ] **Step 6: Commit**

```bash
git add -A editors/shapeshifter CMakeLists.txt
git commit -m "feat(shapeshifter): parent-local transforms, node kinds and real resolution"
```

---

### Task 6: Hierarchy operations

The machinery the UI does not yet expose.

**Files:**
- Modify: `editors/shapeshifter/core/src/scene.{h,cpp}`, `editors/shapeshifter/core/src/editor.cpp`
- Modify: `editors/shapeshifter/tests/core/hierarchy_tests.cpp`

**Interfaces:**
- Produces:

```cpp
enum class OrphanPolicy { Reparent, Cascade };
bool SceneDocument::attach(int32_t id, ParentRef parent, bool preserve_world_pose = true);
void SceneDocument::detach(int32_t id);
// REQUIRED, never defaulted -- which of the two a deletion means must not be
// decided silently at a call site (the rule pick_gizmo_handle's `slot` follows).
void SceneDocument::remove_node(int32_t id, OrphanPolicy policy);
```

- [ ] **Step 1: Write the failing tests**

- `"attach preserving world pose leaves placement unchanged"` — offset+rotated parent; capture `placement(child)` before, attach, compare after.
- `"detach leaves placement unchanged"` — the inverse.
- `"attach rejects self-parenting"` — returns false, node unmodified.
- `"attach rejects a cycle"` — A→B then B→A returns false and leaves B's parent alone.
- `"attach to an unknown id is rejected"`
- `"remove_node Reparent preserves each survivor's world frame"` — grandparent/parent/child chain, remove the middle, assert the child's placement is unchanged.
- `"remove_node Cascade removes exactly the subtree"` — assert node count and that a sibling outside the subtree survives.
- `"removing a node invalidates contacts naming it"` — the surviving node's `placement().contact` is `nullopt`.

- [ ] **Step 2: Run, verify FAIL**

- [ ] **Step 3: Implement**

`attach`: reject unknown id, self, and any prospective ancestor equal to `id`
(walk up from the proposed parent). When `preserve_world_pose`, capture
`placement(id).frame` first, set the parent, then solve the new local transform
from the new parent frame — `local = compose_inverse(parent_frame, world_frame)`,
implemented directly as: rotation `= conj(parent.rotation) * world.rotation`,
position `= conj(parent.rotation) * (world.position - parent.position) / parent.uniform_scale`.
Add that inverse as `Frame relative_to(const Frame& parent, const Frame& world)`
in `frame.h`/`frame.cpp`, with its own round-trip test
(`compose(p, relative_to(p, w)) == w`).

`remove_node(id, Cascade)`: collect the subtree by repeated sweep (the node
vector is small), then erase. `Reparent`: for each direct child, `detach()` it
first, then erase the parent.

`editor.cpp`'s `deleteSelectedNode` picks the policy from the node's kind —
`Reparent` for `Shape`, `Cascade` for `Group`.

- [ ] **Step 4: Run, verify PASS. Step 5: Commit**

```bash
git commit -am "feat(shapeshifter): attach, detach and both orphan policies"
```

---

### Task 7: The external attachment seam

**Files:**
- Modify: `editors/shapeshifter/core/src/scene.{h,cpp}`
- Modify: `editors/shapeshifter/tests/core/hierarchy_tests.cpp`

**Interfaces:**
- Produces: `void SceneDocument::set_frame_provider(const FrameProvider*)`; `placement()` resolving `ParentRef::Kind::Attachment`.

- [ ] **Step 1: Write the failing tests**, with a stub provider in the test file:

```cpp
// The animation-preview seam, exercised without any animation code. A real
// provider will be backed by AnimationSet::AttachmentTransform(id, pose)
// (src/engine/animation/animation_set.hpp) -- name-addressed, exactly like this.
class StubProvider : public FrameProvider {
public:
    std::optional<Frame> frame_for_attachment(const std::string& name) const override {
        if (name != "hand.R") return std::nullopt;
        Frame f;
        f.position = simd_float3{0, 2, 0};
        f.rotation = simd_quaternion(float(M_PI_2), simd_float3{0, 1, 0});
        return f;
    }
};
```

- `"a node parented to a known attachment resolves through the provider"` — child at local `{1,0,0}` under `"hand.R"` → world `{0,2,-1}`, `binding_resolved` true.
- `"an unknown attachment name falls back to world-rooted"` — `binding_resolved` false, `frame.position` equals the node's local position.
- `"a null provider falls back to world-rooted"` — same, with no provider set at all.

- [ ] **Step 2: Run, verify FAIL. Step 3: Implement.**

`SceneDocument` holds `const FrameProvider* provider_ = nullptr;`. In
`placement()`'s walk, `Kind::Attachment` calls
`provider_->frame_for_attachment(ref.attachment)`; `nullopt` (or a null provider)
means identity parent frame and `binding_resolved = false`.

- [ ] **Step 4: Run, verify PASS. Step 5: Commit**

```bash
git commit -am "feat(shapeshifter): resolve attachment-named parents through a FrameProvider"
```

---

### Task 8a: The decomposition — diffing a live document into a `Delta`

Pure function, no history yet. This is the "decompose the temporary change into
something serialized" step, isolated so it can be tested on its own.

**Files:**
- Create: `editors/shapeshifter/core/src/history.h`, `editors/shapeshifter/core/src/history.cpp`
- Create: `editors/shapeshifter/tests/core/history_tests.cpp`
- Modify: `editors/shapeshifter/core/src/scene.h` (equality operators, counter accessors), `CMakeLists.txt:2476` and `:2552`

**Interfaces:**
- Produces:

```cpp
// The document state a Delta cannot reach by touching nodes alone. Without it a
// spawn after an undo reuses an id.
struct Counters {
    int32_t next_id = 1;
    std::array<int32_t, kShapeCount> shape_counts{};
    int32_t group_count = 0;
};

// One committed edit, in the form it will be serialized. Produced by decomposing
// the live document against the baseline captured when the interaction began --
// so it records a gesture's NET RESULT, never the path the cursor took.
//
// `added` carries an index because NODE ORDER IS SEMANTIC: sdf_fold reduces in
// vector order and a Subtract carves everything before it, so re-adding a node
// at the wrong position silently repaints the scene.
struct Delta {
    struct Added { Node node; size_t index; };
    std::vector<Added>   added;
    std::vector<int32_t> removed;   // by id
    std::vector<Node>    changed;   // whole-node replace, matched by id
    Counters counters;
    bool empty() const;
};

// The two halves of the round trip. apply() must be exact: undo replays deltas
// forward from a snapshot, so any drift here accumulates.
Delta decompose(const SceneDocument& baseline, const SceneDocument& current);
void  apply(SceneDocument& doc, const Delta& delta);
```

Requires `operator==` on `Node`, `ParentRef` and `Contact` in `scene.h`, plus
`SceneDocument::counters()` / `set_counters()` so the diff can reach them.

- [ ] **Step 1: Write the failing tests**

- `"decomposing an unchanged document yields an empty delta"` — `d.empty()` true.
- `"a moved node appears once in changed"` — one node's `local_position` written; `changed.size() == 1`, `added`/`removed` empty.
- `"a spawned node appears in added with its index"` — spawn into a 2-node document; `added[0].index == 2`.
- `"a removed node appears in removed by id"`
- `"a node inserted mid-vector keeps its index"` — build a 3-node document, insert at index 1 by direct vector manipulation, assert `added[0].index == 1`.
- `"apply(decompose(a, b)) turns a into b"` — the round trip, over a document exercising all three lists at once plus changed counters. Compare with `operator==`.
- `"the round trip preserves node ORDER"` — assert `doc.nodes()[i].id` matches element for element, not just set membership. This is the one that protects CSG order.
- `"counters round-trip"` — spawn, decompose, apply to a fresh document, then spawn again and assert the new id does not collide.

- [ ] **Step 2: Run FAIL → Step 3: Implement → Step 4: Run PASS**

`decompose`: single pass over each vector keyed by id. `apply`: process `removed`
first, then `changed` in place, then `added` by inserting at `index` in ascending
index order.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt editors/shapeshifter/core/src/history.* \
        editors/shapeshifter/tests/core/history_tests.cpp editors/shapeshifter/core/src/scene.h
git commit -m "feat(shapeshifter): decompose a live document into a serializable Delta"
```

---

### Task 8b: `History` — keyframes, deltas, and replay

**Files:**
- Modify: `editors/shapeshifter/core/src/history.{h,cpp}`, `editors/shapeshifter/tests/core/history_tests.cpp`

**Interfaces:**
- Consumes: `Delta`, `decompose`, `apply` (Task 8a)
- Produces:

```cpp
struct Snapshot { SceneDocument doc; };

struct Entry {
    std::string label;                       // what the Edit menu shows
    int32_t selected = kInvalidNode;         // selection AFTER this entry
    std::variant<Snapshot, Delta> payload;
};

class History {
public:
    // Entry 0 is the initial state and always carries a Snapshot, so undo always
    // has somewhere to land.
    History(const SceneDocument& initial, int32_t selected);

    // Refcounted: the app brackets every gesture, and a nested begin must not
    // split one gesture into two entries.
    void begin_interaction(std::string_view label);
    // Decomposes `doc` against the baseline captured at the outermost begin. An
    // empty delta pushes NOTHING -- that is what makes a click that selects
    // rather than drags, and a dial press that turns nothing, leave no entry.
    void end_interaction(const SceneDocument& doc, int32_t selected);
    bool in_interaction() const;             // the debug assert's question

    bool can_undo() const;
    bool can_redo() const;
    // Rebuild the document at the new cursor: restore the nearest preceding
    // Snapshot, then replay Deltas forward. NO INVERSE IS EVER COMPUTED.
    // nullopt at either end of the stack.
    std::optional<Entry> undo(SceneDocument& out_doc);
    std::optional<Entry> redo(SceneDocument& out_doc);

    const std::string& undo_label() const;   // "" when !can_undo()
    const std::string& redo_label() const;

    // A Snapshot every N entries, and whenever a Delta would be larger anyway.
    // 64 is a wall-clock choice: worst-case undo replays 64 deltas over a
    // document of tens of nodes, which is far below a frame.
    void set_snapshot_interval(size_t n);
    static constexpr size_t kSnapshotInterval = 64;
    static constexpr size_t kMaxEntries = 200;
};
```

- [ ] **Step 1: Write the failing tests**

- `"an edit inside an interaction can be undone"` — begin, spawn, end, `undo()` yields a document with zero nodes.
- `"undo then redo restores the edit"`
- `"an interaction that changes nothing leaves no entry"` — begin, end with an untouched document, `can_undo()` false.
- `"nested interactions produce one entry"` — begin, begin, spawn, end, end → exactly one undoable step.
- `"a drag's intermediate states are not recorded"` — begin, write `local_position` three times, end; one `undo()` returns the node to its pre-gesture position and `can_undo()` is then false. **This is the test that pins "the delta applies at drag end, not along the way."**
- `"a new edit truncates the redo stack"`
- `"selection is restored per entry"`
- `"a snapshot lands every interval"` — `set_snapshot_interval(4)`, run 12 edits, assert entries 0/4/8/12 hold a `Snapshot` and the rest hold a `Delta`.
- `"undo across a snapshot boundary replays correctly"` — `set_snapshot_interval(4)`, 10 edits, undo to entry 6, assert the document matches a reference built by applying edits 1..6 directly. **The replay correctness test.**
- `"undoing all the way reaches the initial state"` — N edits, N undos, document is empty.
- `"the entry cap drops the oldest"` — `kMaxEntries + 10` edits; the stack is capped and **the oldest surviving entry still carries a Snapshot**, or replay has no base. Dropping an entry must promote the new oldest to a Snapshot.

- [ ] **Step 2: Run FAIL → Step 3: Implement → Step 4: Run PASS**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(shapeshifter): keyframe-and-delta history with replay, no inverses"
```

---

### Task 9: Editor exposes interactions and undo/redo

Core does **not** decide boundaries. It exposes them and lets the app declare
them, uniformly.

**Files:**
- Modify: `editors/shapeshifter/core/include/shapeshifter/ShapeshifterCore.h`, `editors/shapeshifter/core/src/editor.cpp`
- Modify: `editors/shapeshifter/tests/core/history_tests.cpp`

**Interfaces:**
- Consumes: `History` (Task 8b)
- Produces, on `sq::Editor`:

```cpp
// --- interactions -------------------------------------------------------
//
// A gesture, and the unit of undo. The APP declares every boundary, because
// the app is what owns gestures -- a drag, a dial turn, a click that spawns,
// a key that deletes. beginDrag/endDrag are about GIZMO STATE and touch
// history not at all.
//
// Refcounted, so a nested pair never splits one gesture into two entries.
// Everything mutated between the outermost begin and end is decomposed into
// ONE delta at the end -- the intermediate states a drag writes are temporary
// and are never recorded.
void beginInteraction(const char* label);
void endInteraction();

void undo();
void redo();
bool canUndo() const;
bool canRedo() const;
void undoLabel(char* buf, int32_t bufLen) const;   // NUL-terminated, "" when none
void redoLabel(char* buf, int32_t bufLen) const;

// Which handle the running drag grabbed, so the app can name the interaction
// "Move" / "Rotate" / "Scale" without guessing. .handle == None when no drag
// is active. Safe to call right after a successful beginDrag: that call
// captures gizmo state and mutates the document not at all, so opening the
// interaction AFTER it still brackets every edit.
GizmoHit activeDragHandle() const;
```

`nodeName`'s NUL-terminated fill pattern (`editor.cpp:462`) is the model for the
two label getters.

- [ ] **Step 1: Write the failing tests** (Editor-level, headless, no device)

- `"a drag gesture produces exactly one undo entry"` — `beginInteraction("Move")`, `beginDrag`, three `updateDrag`, `endDrag`, `endInteraction()`; then one `undo()` returns the node to its pre-drag position and `canUndo()` is false.
- `"an off-handle press that starts no drag leaves no entry"` — `beginInteraction` / `beginDrag` returns false / `endInteraction`; `canUndo()` false.
- `"a dial turn produces exactly one undo entry"` — `beginInteraction("Shape")`, five `setNodeShapeParam` calls with different values, `endInteraction()`; one entry, and undo restores the original value.
- `"undo after delete restores the node and the selection"`
- `"undo mid-gesture ends the active drag"` — begin an interaction and a drag, call `undo()`, then `updateDrag`; assert nothing moves. `Impl::drag` holds a captured `GizmoFrame` and `start_*` values belonging to a document state undo has just discarded.
- `"undoLabel reports the pending entry"` — `"Move"` after a move drag.

- [ ] **Step 2: Run FAIL → Step 3: Implement**

`Impl` gains `History history;`, constructed in `Editor::create` from the empty
document. `beginInteraction`/`endInteraction` forward to it, `endInteraction`
passing `impl_->scene` and `impl_->selected`.

`undo()`/`redo()`: call `endDrag()` **first**, then apply the returned `Entry` —
the history rebuilds `impl_->scene` in place, then set `impl_->selected` from the
entry, clear `impl_->hover`, and `markSceneLinesDirty()`.

Add the debug guard: a `SceneDocument` mutation reached with
`history.in_interaction() == false` trips an assert in debug builds. With the app
owning boundaries, a missed bracket would otherwise be a silently unrecorded
edit. Compile it out of release.

- [ ] **Step 4: Run PASS → Step 5: Commit**

```bash
git commit -am "feat(shapeshifter): expose interactions and undo/redo on the interop surface"
```

---

### Task 10: App layer — shortcuts and the Edit menu

The only approved UI delta.

**Files:**
- Modify: `editors/shapeshifter/app/Sources/MetalViewport.swift:35,120`, `app/Sources/EditorViewModel.swift:338`, `app/Sources/ShapeshifterApp.swift`

**Interfaces:**
- Consumes: `Editor.beginInteraction/endInteraction/activeDragHandle/undo/redo/canUndo/canRedo/undoLabel/redoLabel` (Task 9)

- [ ] **Step 1: Widen the key-down callback**

`ViewportNSView.onKeyDown` becomes `((String, NSEvent.ModifierFlags) -> Bool)`;
`keyDown(with:)` passes `event.modifierFlags`. ⌘ chords are currently unreachable
because `charactersIgnoringModifiers` is all that crosses.

- [ ] **Step 2: Extend `EditorViewModel.handleKeyDown`**

```swift
func handleKeyDown(_ characters: String, modifiers: NSEvent.ModifierFlags) -> Bool {
    // ⌫ and ⌦ both delete, no modifier — the Mac canvas-app convention
    // (Figma, Sketch, Keynote). Finder's ⌘⌫ is a Finder-specific safety
    // measure, not the model here.
    if characters == "\u{7F}" || characters == String(UnicodeScalar(NSDeleteFunctionKey)!) {
        deleteSelected()
        return true
    }
    if modifiers.contains(.command) {
        switch characters.lowercased() {
        // ⇧⌘Z is the Mac redo. ⌘Y is a Windows convention — accepted as a
        // silent alias, never shown in the menu.
        case "z": modifiers.contains(.shift) ? redo() : undo(); return true
        case "y": redo(); return true
        default: return false
        }
    }
    switch characters {
    case "1": setMode(.edit); return true
    case "2": setMode(.spawn); return true
    case "f", "F": editor.frameSelected(); refreshOverlayState(); return true
    default: return false
    }
}
```

- [ ] **Step 3: Bracket every mutating gesture in an interaction**

This is the load-bearing step, and the rule has no exceptions: **if it changes
the document, it is inside a `beginInteraction`/`endInteraction` pair.**
Selection, camera and mode changes are not mutations and get none.

| Site (`EditorViewModel.swift`) | Bracket |
|---|---|
| `handleMouseDown`, `.manipulating` branch (:131) | after a successful `beginDrag`, `beginInteraction(dragLabel(editor.activeDragHandle()))` |
| `handleMouseUp`, `.manipulating` branch (:171) | `editor.endDrag()` then `editor.endInteraction()` |
| `performClick`, `.spawn` branch (:199) | `"Spawn"` |
| `performClick`, `.edit` branch (:194) | **none** — selection is not undoable |
| `radialToggleOp` (:308) | `"Change Op"` |
| `deleteSelected` (:330) | `"Delete"` |
| `handleKeyDown` ⌫ / ⌦ | via `deleteSelected` |
| camera gestures, `setMode`, `frameSelected` | **none** — no document change |

The dial is bracketed where it already tracks its own gesture, in
`RadialMenu.knob`'s `DragGesture` (`RadialMenu.swift:156`): open on the branch
that creates `drag`, close in `onEnded`. Add VM passthroughs
`beginInteraction(_:)` / `endInteraction()` so the view never touches `editor`
directly.

```swift
// Label a gizmo drag by what it grabbed, so the Edit menu says what happened.
private static func dragLabel(_ hit: sq.GizmoHit) -> String {
    if hit.slot == .Shape { return "Scale" }
    switch hit.handle {
    case .RingU, .RingV, .RingN: return "Rotate"
    default: return "Move"
    }
}
```

**Hazard, and it is the same one `RadialMenu.swift:180` already documents:** the
knob only exists while the selection has a parameter. If that stops being true
mid-gesture — a delete, a selection change — `onEnded` never arrives. Its
existing `.onDisappear { drag = nil }` must also call `endInteraction()`, or the
interaction leaks open and every later edit folds into one undo entry.

- [ ] **Step 4: Add the VM's undo surface and its observable mirrors**

`undo()` / `redo()` call core, then `refreshSelectionMirrors()`, `syncGizmo()`
and `refreshHistoryMirrors()`. Add `var canUndo = false`, `var canRedo = false`,
`var undoTitle = "Undo"`, `var redoTitle = "Redo"`, refreshed by
`refreshHistoryMirrors()` — called from `refreshSelectionMirrors()` and after
every `endInteraction()`, the same pattern `refreshOverlayState` already follows
(`EditorViewModel.swift:377`).

- [ ] **Step 5: Add the Edit menu**

`ContentView` needs the VM hoisted so `ShapeshifterApp` can reach it — move
`@State private var vm` up into `ShapeshifterApp` and pass it down.

```swift
WindowGroup { ContentView(vm: vm) }
    .commands {
        CommandGroup(replacing: .undoRedo) {
            Button(vm.undoTitle) { vm.undo() }
                .keyboardShortcut("z", modifiers: .command)
                .disabled(!vm.canUndo)
            Button(vm.redoTitle) { vm.redo() }
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .disabled(!vm.canRedo)
        }
    }
```

- [ ] **Step 6: Build and drive the app**

```bash
xcodebuild -scheme Shapeshifter build      # runs scripts/build.sh shapeshifter_core first
```

Manual checks, each one pinning a decision this plan turned on:

- Spawn three shapes, drag one a long way, then ⌘Z four times. **Each press must
  undo one gesture, not one mouse-move event** — that is "the delta applies at
  drag end, not along the way", visible.
- Turn the shape dial across its whole range in one hold, release, ⌘Z once. The
  parameter returns to where it started, in **one** step.
- ⇧⌘Z forward through the same edits; the scene retraces exactly.
- Select a node, press ⌫, ⌘Z — the node comes back **still selected**.
- Click empty space to deselect, then ⌘Z. Nothing happens: selection is not an
  edit and left no entry.
- The Edit menu reads "Undo Move" / "Redo Delete" and greys out at both ends.

- [ ] **Step 7: Commit**

```bash
git add -A editors/shapeshifter/app
git commit -m "feat(shapeshifter): undo/redo shortcuts, delete key and the Edit menu"
```

---

## Verification

**Per task:** `scripts/build.sh shapeshifter_core shapeshifter_core_tests && scripts/test.sh shapeshifter_core_tests`

**Full shapeshifter surface, before declaring done:**

```bash
scripts/build.sh shapeshifter_core shapeshifter_core_tests shapeshifter_rhi_tests
scripts/test.sh shapeshifter          # core, rhi, slang, shader-hash, bundle-freshness
xcodebuild -scheme Shapeshifter build
```

**The gate that matters:** `pack_baseline_tests` from Task 1 must pass unchanged
at Tasks 4, 5, 6 and 7. It is the assertion that the document model changed and
the rendered image did not. If it ever fails, the inertness ruling has been
broken — stop rather than adjusting the literals.

**Visual confirmation** that the raymarched viewport is unchanged, after Task 5:

```bash
scripts/test.sh shapeshifter_presented_frame   # behind ctest -L display
```

That suite renders through a real `CAMetalLayer` drawable and reads pixels back,
because every other suite was green both times the window was black
(`editors/shapeshifter/CLAUDE.md`).

## Out of scope

Multi-select (Spec 2), copy/paste/duplicate (Spec 3), save/load (Spec 4). Any rig
loading, clip playback or animation preview — the `FrameProvider` seam is built
and nothing implements it. Any UI for hierarchy. Skinning, which the
similarity-only constraint makes impossible here. Transform caching.

**CSG scoping and smooth blending are deliberately not here**, both by ruling.
A Subtract carves everything before it in vector order, group or not, exactly as
today; and blending stays a per-node property whose home already exists in
`params.yzw` (`sdf_scene.h:171`). Either would change rendering and so would
break the inertness gate this plan is built on.

**Stretching a finished assembly** — "make the arm longer" — is also out, and
worth recording *how* it comes back, since it is not by widening `SdfNode`. It
arrives as a **destructive modification** that fans out once: scale each child's
position by `S` in group space (exact — positions are points), and rewrite each
child's stored `scale` by `S` expressed in that child's own frame (exact for an
axis-aligned child, an approximation for a rotated one). That edits stored data
permanently and undoably rather than composing a matrix, so no shear ever enters
the transform chain. It is the same deliberate fan-out this plan already accepts
for `attach(preserve_world_pose)`.
