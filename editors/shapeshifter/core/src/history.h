#pragma once
#include <cstddef>
#include <cstdint>
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

} // namespace sq
