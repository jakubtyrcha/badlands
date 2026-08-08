#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "scene.h"

namespace sq {

// ONE COMMITTED EDIT, in the form it will be serialized.
//
// Produced by DECOMPOSING the live document against a baseline captured when
// the interaction began -- so it records a gesture's NET RESULT, never the path
// the cursor took. Dragging a node across the viewport writes local_position on
// every mouse-move, and all of that is temporary; only what is true at the end
// becomes a Delta.
//
// `added` carries an index because NODE ORDER IS SEMANTIC: sdf_fold reduces in
// vector order and a Subtract carves everything before it, so putting a node
// back at the wrong position silently repaints the scene.
//
// `changed` carries whole Node values rather than per-field edits. A node is
// ~120 bytes, whole-node replacement is trivially correct, and the undo label
// comes from the INTERACTION rather than from field granularity -- so per-field
// splitting would buy only delta size, which nothing is short of.
struct Delta {
    struct Added {
        Node node;
        size_t index = 0;
    };
    std::vector<Added> added;
    std::vector<int32_t> removed;  // by id
    std::vector<Node> changed;     // matched by id
    // The full id sequence, recorded ONLY when the surviving nodes' order
    // changed. A pure reordering touches no field on any node, so add/remove/
    // change would all come back empty and the reorder would vanish on replay --
    // and order is not cosmetic here, since sdf_fold reduces in vector order
    // and a Subtract carves everything before it. Empty means "unchanged".
    std::vector<int32_t> order;
    Counters counters;

    bool empty() const;
};

// What changed between two documents. Exact, not approximate: a gesture that
// ended precisely where it began must produce an empty Delta and leave no undo
// entry at all.
Delta decompose(const SceneDocument& baseline, const SceneDocument& current);

// Applies a Delta in place. THE INVERSE IS NEVER COMPUTED anywhere in this
// design -- undo restores the nearest preceding snapshot and replays deltas
// forward -- so this has to be exact, because any drift accumulates over a
// replay rather than cancelling.
//
// Order matters: removals first, then in-place changes, then insertions in
// ascending index, so each `Added::index` means what it meant when the delta
// was decomposed.
void apply(SceneDocument& doc, const Delta& delta);

// A whole document, stored outright. The keyframe a replay starts from.
struct Snapshot {
    SceneDocument doc;
};

// One step of the undo stack.
struct Entry {
    std::string label;                // what the Edit menu shows: "Move", "Delete"
    int32_t selected = kInvalidNode;  // the selection AFTER this entry
    std::variant<Snapshot, Delta> payload;
};

// KEYFRAMES AND DELTAS, AND NO INVERSE ANYWHERE.
//
// An inverse is the unstable part of a command-undo scheme: float drift on
// replay, and a removal's inverse that has to restore a node AT ITS INDEX along
// with every orphan fixup it triggered. This design computes none. Undo rebuilds
// the document by restoring the nearest preceding Snapshot and replaying Deltas
// forward, which is exact by construction because it only ever runs the same
// arithmetic that produced the state in the first place.
//
// Snapshots land every kSnapshotInterval entries, and whenever a Delta would be
// bigger than one anyway. 64 is a wall-clock choice: worst-case undo replays 64
// deltas over a document of tens of nodes, far below a frame.
//
// The APP declares interaction boundaries, uniformly, for every gesture -- a
// drag, a dial turn, a click that spawns, a key that deletes. Nothing in core
// decides them. Boundaries are refcounted so a nested begin cannot split one
// gesture into two entries.
class History {
public:
    // Entry 0 is the initial state and always carries a Snapshot, so a replay
    // always has a base and undo always has somewhere to land.
    History(const SceneDocument& initial, int32_t selected);

    // `doc` is the document AS IT STANDS, captured as the baseline the eventual
    // decomposition runs against. Only the outermost begin captures one.
    void begin_interaction(std::string_view label, const SceneDocument& doc);
    // Decomposes `doc` against the baseline captured at the OUTERMOST begin. An
    // empty delta pushes nothing -- which is what makes a click that merely
    // selects, and a dial press that turns nothing, cost no undo step.
    void end_interaction(const SceneDocument& doc, int32_t selected);
    bool in_interaction() const { return depth_ > 0; }

    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ + 1 < entries_.size(); }

    // Rebuild the document at the neighbouring cursor position and return that
    // entry. nullopt at either end, leaving `out_doc` untouched.
    std::optional<Entry> undo(SceneDocument& out_doc);
    std::optional<Entry> redo(SceneDocument& out_doc);

    // "" when there is nothing to undo/redo.
    const std::string& undo_label() const;
    const std::string& redo_label() const;

    static constexpr size_t kSnapshotInterval = 64;
    static constexpr size_t kMaxEntries = 200;
    // Test seam. Anything below 1 is treated as 1 (every entry a snapshot).
    void set_snapshot_interval(size_t interval);

    size_t entry_count() const { return entries_.size(); }
    size_t cursor() const { return cursor_; }
    bool entry_is_snapshot(size_t index) const;

private:
    // The document as of `index`, rebuilt from the nearest preceding Snapshot.
    SceneDocument rebuild(size_t index) const;
    void push(Entry entry);

    std::vector<Entry> entries_;
    size_t cursor_ = 0;
    size_t interval_ = kSnapshotInterval;

    int depth_ = 0;
    SceneDocument baseline_;
    std::string label_;
};

} // namespace sq
