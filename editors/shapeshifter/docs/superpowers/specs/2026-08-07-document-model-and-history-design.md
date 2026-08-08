# Document model, transform resolution and history — design

Shapeshifter's scene is a flat vector of world-space nodes with no hierarchy, no
undo and no file format. Four capabilities are wanted on top of it — multi-select,
undo/redo, copy/paste and save/load — and three of the four hinge on the same
missing thing: a document model with one mutation seam.

This spec covers **only that foundation**, plus the undo/redo that falls out of it.
Multi-select, the clipboard and serialization are separate specs that build on it.

---

## The problem, stated precisely

`Node` (`core/src/scene.h`) stores `position`, `rotation` and `scale` in **world
space**, and every consumer composes from those fields directly:

| Consumer | Reads |
|---|---|
| `pack_scene` (`sdf.cpp:63`) | position, rotation, scale |
| `raycast_node` / `raycast_scene` (`picking.h:47`) | position, rotation, scale |
| `scene_aabb` (`sdf.cpp:102`) | position, scale |
| `gizmo_frame_for_node` (`gizmo.cpp:50`) | position, rotation, snap fields |
| `append_node_wireframe` (`lines.h:143`) | the whole node |
| `node_bounding_radius` (`navigation.h:48`) | scale |
| all four drag solvers (`editor.cpp:613`) | **write** position, rotation, scale |

Nested transforms are impossible while that holds, because there is no place for a
parent's contribution to enter. Attachment is a second, smaller tangle: `snapped`,
`snap_point`, `snap_normal` and `snap_parent` are four world-space fields on `Node`
read at exactly three sites, and `snap_parent` propagates nothing at all.

---

## Two hard constraints discovered during design

These are not preferences. They bound what any solution is allowed to be.

**1. Node order is semantic.** `sdf_fold` (`shaders/sdf_scene.h:574`) reduces in
vector order, and a Subtract node carves everything accumulated before it. A tree
walk that reordered nodes would silently repaint every scene. Therefore
**hierarchy must stay separate from CSG order**: the document keeps its flat,
ordered node vector as the evaluation order, and parenting is a transform relation
layered over it.

**2. The renderer cannot represent shear, or even non-uniform scale in a frame.**
`SdfNode` is `pos_shape` + `inv_rotation` + `half_extents_op`. World→local is
rotation and translation only; per-axis size lives in `half_extents`, applied in
*local* space as shape data. So the composed world transform of a node must be a
**similarity** — rotation, translation, and one uniform scalar — and nothing else.
Full TRS composition through a non-uniformly scaled parent produces shear, which
has nowhere to go.

Constraint 2 is why every production rigger treats inherited scale as a problem:
Maya gives joints `segmentScaleCompensate` (on by default, so a joint *undoes* its
parent's scale), and Blender exposes `inherit_scale` as `FULL`/`FIX_SHEAR`/
`ALIGNED`/`AVERAGE`/`NONE` — three of those five modes existing purely to launder
non-uniform parent scale. Unity and Unreal do compose fully and do shear; they can
afford to, because they carry a 4×4 per node. Shapeshifter does not.

---

## The animation system this must serve

`src/engine/animation/` is an ozz-animation runtime, and it already answers "what
is a bone" and "what is an attachment":

- `Skeleton` loads from `.ozz` archives; `AnimationSet` is a skeleton plus named
  clips from a JSON manifest; `Pose` holds SoA locals and derived model matrices.
- **Attachments are one namespace over joints and sockets.**
  `AnimationSet::FindAttachment(name)` returns an id; every attachment is
  `(joint index, offset)`, and `AttachmentTransform(id, pose)` composes the joint's
  model matrix with that offset. Per `src/engine/CLAUDE.md`, nothing public can ask
  which kind it found — that is what makes collapsing a prop node into a static
  socket an import-time size decision rather than a contract change.
- Sockets are authored as data in `clips.json`: `{name, parent: <joint name>,
  offset: [16 floats]}`. Rigs come from `tools/rigpack`; **nothing authors rigs
  in-engine**.

Shapeshifter must not invent a second attachment model. Its external-parent
reference is this one, addressed the same way: by name.

Two consequences follow.

- **Creatures built here are articulated rigid parts**, not skinned meshes. A rigid
  SDF node can ride a joint exactly; a node that must bend across two joints cannot
  exist, because skinning needs a per-point blended transform where
  `sdf_eval_node` needs one similarity for the whole node. This is an art-direction
  constraint as much as a technical one.
- **A joint's model matrix must be decomposed to a similarity at the boundary**,
  with shear and non-uniform scale discarded, before it can parent an SDF node.

---

## User rulings

1. **Foundation first.** This spec is the document model; multi-select, clipboard
   and save/load are separate specs on top of it.
2. **The parent link is modelled but inert.** Spawn-snapping does *not* start
   parenting nodes. Every node stays world-rooted until an attach UI exists, so
   this spec changes no rendered pixel.
3. **What a parent carries depends on node kind.** A Shape's non-uniform scale is
   box data and is never inherited (Maya's `segmentScaleCompensate` default). A
   Group carries a rigid frame plus a uniform scale, and *that* propagates.
4. **Shapeshifter consumes rigs and never authors them.** "This is NOT intended to
   be an animation studio." Bones are external, read-only, addressed by name.
5. **Design for animation at the data and interface level; implement none of it.**
   No rig loading, no clips, no scrubbing in this spec.
6. **Groups and slots are wanted**, so the kind discriminant lands here.
7. **The machinery handles both delete policies even if the UI lags.**
8. **Approved UI: an Edit menu with Undo/Redo, ⌘Z / ⇧⌘Z, plus ⌫ / ⌦ to delete.**
   Nothing else.

---

## CORE DELIVERABLES

- `Node` carries a **parent-local** transform, a `NodeKind`, a `ParentRef` and a
  `Contact`; the four `snap_*` fields are gone.
- `SceneDocument::world_frame(id)` is the **only** way to learn where a node is,
  and it returns a similarity that is representable by `SdfNode` by construction.
- `attach` / `detach` / `remove_node(id, OrphanPolicy)` exist and are tested, with
  no UI.
- A `FrameProvider` seam that an external rig pose can later drive, shipped null.
- Snapshot undo/redo with per-gesture coalescing, reachable from ⌘Z / ⇧⌘Z and an
  Edit menu.
- **`pack_scene` output is byte-identical to today's** for scenes built by today's
  spawn paths.

## CORE ASSUMPTIONS

- The document stays small — tens to low hundreds of nodes. Snapshot undo and
  on-demand transform resolution are both sized against that.
- CSG evaluation order remains the flat vector order, independent of hierarchy.
- `SdfNode` remains similarity-only. If the renderer ever gains a full matrix per
  node, the constraint that shapes section 2 disappears and it should be revisited.
- No rig is loaded in this spec, so `FrameProvider` is exercised only by test stubs.

---

## 1. The node model

Three concerns, currently tangled into four world-space fields, become three named
things.

```cpp
enum class NodeKind : int32_t { Shape = 0, Group = 1 };

// Whose frame this node's local transform is expressed in.
struct ParentRef {
    enum class Kind : int32_t { World = 0, Node = 1, Attachment = 2 };
    Kind kind = Kind::World;
    int32_t node = kInvalidNode;   // Kind::Node only
    std::string attachment;        // Kind::Attachment only — a NAME, not an index
};

// The surface this node was placed on. A DIFFERENT relation from `parent`:
// it says what the node rests against, which is what the Placement gizmo
// anchors to. `point` and `normal` are in the PARENT's frame.
struct Contact {
    bool valid = false;
    int32_t surface = kInvalidNode;
    simd_float3 point{0, 0, 0};
    simd_float3 normal{0, 1, 0};
};
```

`Node` becomes:

```cpp
struct Node {
    int32_t id = kInvalidNode;
    std::string name;
    NodeKind kind = NodeKind::Shape;

    ParentRef parent;
    Contact contact;

    // PARENT-LOCAL. Renamed from position/rotation deliberately — see §6.
    simd_float3 local_position = {0, 0, 0};
    simd_quatf local_rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);

    // Shape: per-axis box size, NEVER inherited by children.
    // Group: constrained uniform by the mutation API; only that scalar propagates.
    simd_float3 scale = {1, 1, 1};

    // Shape only. Meaningless on a Group and left at 0.
    Shape shape = Shape::Cube;
    Op op = Op::Add;
    float shape_param = 0.0f;
};
```

**`parent` and `contact` are separate on purpose, and that separation *is* the
decoupling this spec is for.** Today's model already has two distinct relations —
`snap_parent` propagates nothing, while `snap_point` drives the gizmo — and naming
them apart is what lets either change without the other. `attach()` then has a
one-line definition: set `parent := Node(contact.surface)` while preserving world
pose.

Field mapping from today:

| Today | Becomes |
|---|---|
| `position`, `rotation` | `local_position`, `local_rotation` (identical while world-rooted) |
| `snapped` | `contact.valid` |
| `snap_point`, `snap_normal` | `contact.point`, `contact.normal`, in the parent's frame |
| `snap_parent` | `contact.surface` |
| — | `kind`, `parent` (new) |

`Node::world_from_local()` is **deleted**. A node no longer knows where it is.

## 2. Transform resolution

```cpp
// The most general transform SdfNode can represent, and therefore the only thing
// the resolver ever produces. Deliberately NOT a matrix: shear cannot be spelled.
struct Frame {
    simd_float3 position{0, 0, 0};
    simd_quatf  rotation = simd_quaternion(0.f, 0.f, 0.f, 1.f);
    float       uniform_scale = 1.0f;
};

// Compose child-in-parent. Pure, and the single definition of the rule.
Frame compose(const Frame& parent, const Frame& local);
```

Composition:

```
out.rotation      = parent.rotation * local.rotation
out.position      = parent.position + parent.rotation * (parent.uniform_scale * local.position)
out.uniform_scale = parent.uniform_scale * local.uniform_scale
```

A node's own local frame is `{local_position, local_rotation, s}` where `s` is
**1.0 for a Shape** and the group scalar for a Group. That one line encodes ruling
3 in full: a skull's box never stretches the horn on it, while a "head" Group
scales the whole assembly.

```cpp
class SceneDocument {
    // Resolved world placement. The ONLY way to learn where a node is.
    Frame world_frame(int32_t id) const;

    // A Shape's world half-extents: node.scale * 0.5 * world_frame(id).uniform_scale.
    // {0,0,0} for a Group, which has no box.
    simd_float3 world_half_extents(int32_t id) const;

    // The contact point/normal lifted into world space, or nullopt when
    // !contact.valid. What the Placement gizmo anchors to.
    struct WorldContact { simd_float3 point, normal; };
    std::optional<WorldContact> world_contact(int32_t id) const;
};
```

The contact rides the **parent's** frame, not the node's own — it is a fact about
the surface, not about where the node has since been dragged to. `point` is
transformed exactly like `local_position` (rotated, and scaled by the parent's
`uniform_scale`); `normal` is rotated only, so it stays unit.

`pack_scene` becomes, per Shape node:

```cpp
const Frame f = doc.world_frame(node.id);
sn.pos_shape       = {f.position, shape_id};
sn.half_extents_op = {node.scale * 0.5f * f.uniform_scale, op};
sn.inv_rotation    = simd_conjugate(f.rotation).vector;
sn.params          = {node.shape_param, 0, 0, 0};
```

Exactly representable, with no approximation anywhere. Group nodes are **skipped**
by `pack_scene` — they contribute no SDF.

### The external frame seam

```cpp
// A source of frames the document does not own — a rig pose, today; anything
// else later. Spec ships NO implementation; tests supply stubs.
class FrameProvider {
public:
    virtual ~FrameProvider() = default;
    // nullopt when the name is unknown to this provider.
    virtual std::optional<Frame> frame_for_attachment(const std::string& name) const = 0;
};
```

- `SceneDocument` holds a `const FrameProvider*`, null by default.
- **An unresolvable attachment falls back to world-rooted**, and
  `bool SceneDocument::binding_resolved(int32_t id) const` reports it, so a later
  UI can flag a broken binding rather than the node silently sitting at the origin
  with no explanation.
- A provider returning a joint matrix **decomposes it to a similarity at the
  boundary** — extract translation, orthonormalize the rotation basis, take the
  uniform scale as the mean of the basis vector lengths, discard the rest. This
  happens once, in the provider adapter, so no shear ever enters the document.

### Safety

- **Cycles are rejected at mutation time.** `attach` walks the prospective ancestor
  chain and refuses if it reaches the node itself.
- **The walk is depth-capped** (64) as defence in depth; exceeding it returns the
  identity frame and logs once.
- **Resolution is on demand, uncached.** At tens of nodes with a depth of a few,
  this is a handful of quaternion multiplies per node per frame, against a
  raymarch. A per-pack memo is the obvious optimisation if profiling ever asks;
  it is not in this spec.

## 3. Document operations

All implemented and tested. **None reachable from the app in this spec** — the
interop surface gains only what §5 lists.

```cpp
enum class OrphanPolicy { Reparent, Cascade };

// Re-expresses the local transform into the new parent's frame when
// preserve_world_pose. Rejects a cycle, an unknown id, and self-parenting;
// returns false without mutating.
bool SceneDocument::attach(int32_t id, ParentRef parent, bool preserve_world_pose = true);

// Re-roots to world, world pose unchanged. `contact` is untouched: what a node
// rests on is not changed by whose frame it is expressed in.
void SceneDocument::detach(int32_t id);

// Reparent: children survive, re-rooted to world, world pose preserved — today's
//           orphan behaviour, generalised.
// Cascade:  the whole subtree goes.
// REQUIRED, never defaulted: which of the two a deletion means is exactly the
// kind of thing that must not be decided silently at a call site (the same rule
// pick_gizmo_handle's `slot` parameter already follows).
void SceneDocument::remove_node(int32_t id, OrphanPolicy policy);
```

- Any surviving node whose `contact.surface` names a removed node has its contact
  invalidated — today's `remove_node` cleanup, unchanged in effect.
- **`spawn_snapped` does not set `parent`.** It fills `contact` and leaves the node
  world-rooted. This is ruling 2, and §7's inertness test is what proves it.
- The Editor's delete path defaults to `Reparent` for a Shape and `Cascade` for a
  Group. That default is one line; both policies exist regardless.
- Name allocation gains a Group counter beside the per-shape ones
  (`shape_counts_`), so a Group is named "Group 1", "Group 2".

## 4. History — interactions, temporary state, and serialized modifications

*Rewritten after the original draft. The first version had a snapshot per edit
behind an RAII `Transaction` that core opened itself; what follows is what was
designed and built instead. The reason the draft was rejected is worth keeping:
it treated undo granularity as core's business, when granularity is a property
of the GESTURE, and gestures belong to the app.*

Three layers, and keeping them apart is the whole of it.

- **An interaction is a user gesture** with a begin and an end — drag a handle,
  turn the dial, click to spawn, press ⌫. It is the undo granularity and the
  *only* thing that decides it. **The app declares every boundary, uniformly.**
  `beginDrag`/`endDrag` are about gizmo state and touch history not at all.
  Boundaries are refcounted, so a nested begin cannot split one gesture in two
  nor move the baseline past edits the outer one already made.
- **During an interaction the document is mutated live, and that is TEMPORARY
  state.** The drag solver writes `local_position` on every mouse-move exactly
  as before — no allocation, no recording, no per-event cost.
- **At `endInteraction` the temporary change is DECOMPOSED into a serialized
  modification**, by diffing the live document against the baseline captured at
  begin. What lands is the gesture's net result, never the path the cursor took:
  one `Move` entry for a drag of any length, one `Shape` entry for a dial turn
  of any sweep.

**Modifications never fan out.** Moving a parent records one node's changed local
transform; its children move because their world frames are *derived*. The one
unavoidable exception, named rather than hidden: `attach(preserve_world_pose)`
and `remove_node(Reparent)` genuinely rewrite the affected children's local
transforms, because the frame those were expressed in is going away.

### The record: keyframes plus deltas, and no inverse anywhere

An inverse is the unstable part of a command-undo scheme — float drift on replay,
and a removal's inverse that must restore a node *at its index* along with every
orphan fixup it triggered. This design computes none.

```cpp
struct Delta {
    struct Added { Node node; size_t index; };
    std::vector<Added>   added;
    std::vector<int32_t> removed;   // by id
    std::vector<Node>    changed;   // whole-node replace, matched by id
    Counters counters;
    bool empty() const;
};

Delta decompose(const SceneDocument& baseline, const SceneDocument& current);
void  apply(SceneDocument& doc, const Delta& delta);

struct Snapshot { SceneDocument doc; };
struct Entry {
    std::string label;               // "Move", "Delete" — what the Edit menu shows
    int32_t selected;                // the selection AFTER this entry
    std::variant<Snapshot, Delta> payload;
};

class History {
public:
    History(const SceneDocument& initial, int32_t selected);
    void begin_interaction(std::string_view label, const SceneDocument& doc);
    void end_interaction(const SceneDocument& doc, int32_t selected);
    bool in_interaction() const;

    bool can_undo() const;
    bool can_redo() const;
    std::optional<Entry> undo(SceneDocument& out_doc);
    std::optional<Entry> redo(SceneDocument& out_doc);
    const std::string& undo_label() const;   // "" when !can_undo()
    const std::string& redo_label() const;

    static constexpr size_t kSnapshotInterval = 64;
    static constexpr size_t kMaxEntries = 200;
    void set_snapshot_interval(size_t interval);   // test seam
};
```

Rules:

- **Undo restores the nearest preceding Snapshot and replays Deltas forward.**
  Exact by construction, because it only ever runs the arithmetic that produced
  the state in the first place.
- **Two rules pick a payload**, and they are tested apart: a Snapshot every
  `kSnapshotInterval` entries (64), and a Snapshot whenever a Delta would cost
  more than one anyway. The second is not a rounding case — spawning into an
  empty document produces a delta carrying the whole node *plus its index*,
  against a document that is just the node.
- **`added` carries an index** because node order is semantic: `sdf_fold` reduces
  in vector order, so putting a node back at the wrong position silently
  repaints the scene.
- **`counters` rides along.** Id and name allocation is document state; without
  it a spawn after an undo reuses an id.
- **Equality is exact, with no tolerance.** A gesture that ended precisely where
  it began must leave no entry at all.
- **An interaction that changes nothing pushes nothing** — which is what makes a
  click that merely selects, and a dial press that turns nothing, free.
- **Entry 0 always carries a Snapshot.** When the 200-entry cap drops the oldest,
  entry 1 is rebuilt *before* the erase and promoted, because afterwards the
  chain it would replay from is gone.
- **Selection rides per entry; the camera does not.** Undoing a delete returns
  the node *selected*; nothing ever teleports the view.
- **Not undoable:** selection changes, camera gestures, hover, mode switches,
  gizmo visibility.
- **Any active drag is ended before undo/redo runs**, and any open interaction is
  closed. `Impl::drag` holds a `GizmoFrame` and `start_*` values belonging to the
  document about to be discarded; and the app's matching `endInteraction` would
  otherwise decompose the restored document against a baseline from a timeline
  that no longer exists, recording the undo itself as an edit.
- **No `NSUndoManager`.** Core owns the one stack; the menu calls into it. Two
  stacks over one document is a bug factory.

New files: `core/src/frame.h/.cpp` (Frame, `compose`, `relative_to`, the
similarity decomposition, `FrameProvider`) and `core/src/history.h/.cpp`.

## 5. Interop and app layer

Additions to `core/include/shapeshifter/ShapeshifterCore.h`:

```cpp
// The app declares every boundary; core decides none.
void beginInteraction(const char* label);
void endInteraction();

void undo();
void redo();
bool canUndo() const;
bool canRedo() const;
void undoLabel(char* buf, int32_t bufLen) const;   // NUL-terminated, "" when none
void redoLabel(char* buf, int32_t bufLen) const;

// Which handle the running drag grabbed, so the app can name the interaction
// "Move"/"Rotate"/"Scale" without guessing. Safe to call right after a
// successful beginDrag: that call mutates the document not at all, so opening
// the interaction after it still brackets every edit.
GizmoHit activeDragHandle() const;
```

Nothing about hierarchy crosses the boundary. `NodeKind`, `ParentRef` and
`Contact` stay core-internal until a UI needs them; tests reach `SceneDocument`
directly, as `scene_tests.cpp` already does.

App changes:

- **`onKeyDown` gains modifier flags.** `((String) -> Bool)` becomes
  `((String, NSEvent.ModifierFlags) -> Bool)` in `MetalViewport.swift:35` and
  `EditorViewModel.handleKeyDown`. ⌘ chords are currently unreachable because
  `charactersIgnoringModifiers` is all that crosses.
- **⌫ (`\u{7F}`) and ⌦ (`NSDeleteFunctionKey`) delete the selection**, no modifier
  — the Mac canvas-app convention (Figma, Sketch, Keynote). Finder's ⌘⌫ is a
  Finder-specific safety measure and is not the model here.
- **⌘Z undo, ⇧⌘Z redo.** ⌘Y is accepted as a silent alias for redo but is never
  shown: it is a Windows convention and unbound on macOS.
- **An Edit menu** in `ShapeshifterApp`, via
  `.commands { CommandGroup(replacing: .undoRedo) { ... } }`, with titles from
  `undoLabel`/`redoLabel` ("Undo Move"). Disabled when `canUndo`/`canRedo` is
  false. The view model moves up from `ContentView` to `ShapeshifterApp`,
  because a menu is built beside the window rather than inside its content.
- **Every mutating gesture is bracketed**, with no exceptions: the gizmo drag
  (opened *after* `beginDrag` confirms a handle, so an off-handle press leaves
  no entry), the spawn click, the op toggle, the delete key, and the radial
  dial — which brackets in its own `DragGesture`, and must also close in
  `onDisappear`, since `onEnded` never arrives when the knob vanishes
  mid-gesture and an interaction left open would swallow every later edit.

This is the entire approved UI delta. No panel, no hierarchy outliner, no
attach/detach affordance, no group creation.

## 6. Enforcing the seam

The refactor's failure mode is a call site that keeps reading `node.position` and
silently means world space. The defence is a **rename, not a convention**:
`position`/`rotation` become `local_position`/`local_rotation`, so every existing
reader fails to compile and must be visited.

Free functions that take `const Node&` alone can no longer place it. They gain a
resolved frame:

| Today | Becomes |
|---|---|
| `raycast_node(const Node&, const Ray&)` | `raycast_node(const Node&, const Frame&, simd_float3 half_extents, const Ray&)` |
| `append_node_wireframe(out, const Node&, …)` | `append_node_wireframe(out, const Node&, const Frame&, …)` |
| `node_bounding_radius(const Node&)` | `node_bounding_radius(const Node&, const Frame&)` |
| `gizmo_frame_for_node(const Node&, camera, slot)` | `gizmo_frame_for_node(const Node&, const Frame&, std::optional<WorldContact>, camera, slot)` |

The type system then states the rule: **you cannot place a node without a resolved
frame.** Callers that have a `SceneDocument` get it from `world_frame`; the tests
that construct one node in isolation pass an explicit `Frame`, which is clearer
than today's implicit "the node is wherever its fields say".

`build_scene_lines`, `raycast_scene`, `scene_aabb`, `sample_scene` and
`resolve_focus` all already take the document, so they resolve internally.

## 7. Testing

`shapeshifter_core_tests` (doctest, pure CPU, no device). New file
`tests/core/hierarchy_tests.cpp`, plus additions to `scene_tests.cpp`.

**Resolution**
- `compose` associativity and identity.
- `Group → Shape → Shape`: uniform scale composes multiplicatively; a Shape
  contributes 1.0.
- A Shape parent's non-uniform `scale` reaches its children in no way at all.
- A Group's uniform scale scales both the child's offset and its half-extents.
- Depth cap: a hand-built cycle (constructed by bypassing `attach`) returns
  identity and does not hang.

**The external seam**
- A stub `FrameProvider` resolves a named attachment; the node's world frame is
  the provider's frame composed with its local.
- An unknown name falls back to world-rooted and `binding_resolved` is false.
- **A deliberately sheared 4×4 decomposes to a similarity**: the resulting rotation
  is orthonormal and the scale is a single scalar.

**Operations**
- `attach(preserve_world_pose = true)` leaves `world_frame` unchanged, bit-for-bit
  within tolerance.
- `detach` likewise.
- `attach` rejects self-parenting and any cycle, returning false without mutating.
- `remove_node` under both `OrphanPolicy` branches: `Reparent` preserves each
  survivor's world frame; `Cascade` removes exactly the subtree.
- A removed node's `contact.surface` referents are invalidated.

**History**
- Every mutating op round-trips: mutate → undo → document equals the prior state.
- A drag `begin…update×N…end` produces exactly one entry.
- A transaction that changes nothing is discarded.
- A new edit after undo truncates the redo stack.
- Selection restores with undo; the camera does not move.
- The 200-entry cap drops the oldest.

**The inertness gate**
- Build a scene through today's spawn paths (`spawn_snapped`, `spawn_unsnapped`,
  drags), then assert `pack_scene`'s output is **byte-identical** to a recorded
  baseline captured before the refactor. This is the test that proves ruling 2 —
  that the model changed and the pixels did not.

Existing suites (`scene_tests`, `picking_tests`, `gizmo_tests`, `drag_tests`,
`sdf_tests`, `lines_tests`) are updated for the renamed fields and new signatures.
Their expectations do not change; if one has to, that is a regression, not a
migration.

## 8. Implementation hazards

- **No `Node*` may outlive a mutation.** Snapshots replace the whole document, and
  `undo` invalidates every pointer into it. Today's code already re-finds per call
  (`updateDrag` looks the node up each time); keep it that way.
- **`Impl::drag` holds a captured `GizmoFrame` and `start_*` values** belonging to a
  document state that undo can discard. End the drag before applying a snapshot.
- **`gizmos_coalesce` and the tether** read both gizmo origins; the Placement origin
  now comes from `world_contact` rather than `node.snap_point`. The tether's
  behaviour must not change for a world-rooted node.
- **`updateDrag` deliberately does not move the contact** when a node is dragged
  (`editor.cpp:720`) — that offset is what the tether reports. Preserve it: the
  drag writes `local_position`, never `contact`.
- **`simd_quatf` is not trivially serializable and must stay unit.** The rotation
  drag renormalizes on every update because `pack_scene` conjugates rather than
  inverts; `compose` must renormalize too, or a deep chain drifts off unit and the
  conjugate stops being the inverse.
- **Group scale must be kept uniform by the setter**, not merely documented. A
  Group with `{1, 2, 1}` has no defined meaning in `compose`.
- **The Edit menu's titles come from core and change per edit.** SwiftUI caches
  command bodies; the menu must read `undoLabel` at build time of the command, and
  the VM needs an `@Observable` mirror that refreshes after every mutation — the
  same pattern `refreshOverlayState` already uses.

## 9. Out of scope

- Multi-select (Spec 2), copy/paste/duplicate (Spec 3), save/load (Spec 4).
- Any rig loading, clip playback or animation preview. The `FrameProvider` seam is
  built; nothing implements it.
- Any UI for hierarchy: no outliner, no attach/detach affordance, no group
  creation, no reparent drag.
- Skinning, in any form. §"Two hard constraints" explains why it cannot exist here.
- Transform caching. On-demand resolution until profiling says otherwise.

## 10. What this sets up

- **Spec 2 (multi-select)** gets a selection set over the same ids, an aggregate
  gizmo over resolved frames, and one history entry per group gesture for free.
- **Spec 3 (clipboard)** gets its paste semantics stated already: a clone carries
  `local_position`/`local_rotation`/`scale`, its `contact`, and its `parent` when
  that parent still exists — which is exactly "inherits local transform,
  attachment offset, extra properties".
- **Spec 4 (save/load)** gets a document whose every field is plain data. One
  requirement is pinned now: **an attachment name must round-trip even when
  unresolvable**, so opening a creature without its rig loaded does not silently
  drop every binding.
