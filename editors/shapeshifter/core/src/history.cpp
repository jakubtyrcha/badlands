#include "history.h"

#include <algorithm>

namespace sq {

// `counters` is deliberately NOT consulted.
//
// An interaction whose only net effect is advancing next_id_ -- spawn then
// delete inside one bracket -- would otherwise push an undo step that visibly
// does nothing when pressed, which is a worse outcome than the one it prevents.
// It is safe to drop: the id it consumed belonged to a node that exists in no
// recorded state, so re-issuing it later collides with nothing.
bool Delta::empty() const {
    return added.empty() && removed.empty() && changed.empty() && order.empty();
}

Delta decompose(const SceneDocument& baseline, const SceneDocument& current) {
    Delta delta;
    delta.counters = current.counters();

    const std::vector<Node>& before = baseline.nodes();
    const std::vector<Node>& after = current.nodes();

    // Linear scans keyed by id. The node count is in the tens, and an index
    // would be a cache to invalidate for no measurable gain.
    const auto find_in = [](const std::vector<Node>& nodes, int32_t id) -> const Node* {
        for (const Node& node : nodes) {
            if (node.id == id) return &node;
        }
        return nullptr;
    };

    for (size_t i = 0; i < after.size(); ++i) {
        const Node& node = after[i];
        const Node* was = find_in(before, node.id);
        if (was == nullptr) {
            // Index in the CURRENT vector, which is where a replay has to put
            // it back. Ascending, because apply inserts in this order and each
            // insertion shifts everything after it.
            delta.added.push_back(Delta::Added{node, i});
        } else if (*was != node) {
            delta.changed.push_back(node);
        }
    }

    for (const Node& node : before) {
        if (find_in(after, node.id) == nullptr) {
            delta.removed.push_back(node.id);
        }
    }

    // A pure reordering changes no field on any node, so everything above comes
    // back empty and the reorder would be lost. Recorded only when the sequence
    // add/remove alone would produce differs from the real one, so the common
    // case still carries nothing.
    std::vector<int32_t> replayed;
    replayed.reserve(before.size());
    for (const Node& node : before) {
        if (find_in(after, node.id) != nullptr) {
            replayed.push_back(node.id); // survivors, in their OLD order
        }
    }
    for (const Delta::Added& added : delta.added) {
        const size_t at = std::min(added.index, replayed.size());
        replayed.insert(replayed.begin() + static_cast<ptrdiff_t>(at), added.node.id);
    }
    std::vector<int32_t> actual;
    actual.reserve(after.size());
    for (const Node& node : after) {
        actual.push_back(node.id);
    }
    if (replayed != actual) {
        delta.order = std::move(actual);
    }

    return delta;
}

void apply(SceneDocument& doc, const Delta& delta) {
    // Removals FIRST: an index in `added` was measured against the vector after
    // these had already gone, so inserting before erasing would place nodes
    // against a longer vector and shift them.
    for (const int32_t id : delta.removed) {
        doc.erase(id);
    }

    for (const Node& node : delta.changed) {
        Node* target = doc.find(node.id);
        if (target != nullptr) {
            *target = node;
        }
    }

    // Ascending index, which decompose produced them in. Each insertion shifts
    // every later element right by one, and the recorded indices already
    // account for that -- they were read off the finished vector.
    for (const Delta::Added& added : delta.added) {
        doc.insert(added.node, added.index);
    }

    // Only present when add/remove alone could not reproduce the sequence.
    // Applied last, so it sees every node it names.
    if (!delta.order.empty()) {
        doc.reorder(delta.order);
    }

    doc.set_counters(delta.counters);
}

// --- History ---------------------------------------------------------------

namespace {

// Rough byte cost, only ever used to answer "would this delta be bigger than a
// snapshot". Names dominate the variable part, so they are the only thing
// measured beyond the fixed struct size.
size_t rough_size(const Node& node) { return sizeof(Node) + node.name.size(); }

size_t rough_size(const SceneDocument& doc) {
    size_t bytes = 0;
    for (const Node& node : doc.nodes()) {
        bytes += rough_size(node);
    }
    return bytes;
}

size_t rough_size(const Delta& delta) {
    size_t bytes = delta.removed.size() * sizeof(int32_t);
    for (const Delta::Added& added : delta.added) {
        bytes += rough_size(added.node) + sizeof(size_t);
    }
    for (const Node& node : delta.changed) {
        bytes += rough_size(node);
    }
    return bytes;
}

const std::string& empty_label() {
    static const std::string kEmpty;
    return kEmpty;
}

} // namespace

History::History(const SceneDocument& initial, int32_t selected) {
    Entry entry;
    entry.label = "";
    entry.selected = selected;
    entry.payload = Snapshot{initial};
    entries_.push_back(std::move(entry));
    cursor_ = 0;
}

void History::set_snapshot_interval(size_t interval) {
    interval_ = interval < 1 ? 1 : interval;
}

bool History::entry_is_snapshot(size_t index) const {
    return index < entries_.size() && std::holds_alternative<Snapshot>(entries_[index].payload);
}

void History::begin_interaction(std::string_view label, const SceneDocument& doc) {
    // Refcounted: only the OUTERMOST begin captures a baseline, so a nested
    // pair cannot split one gesture into two entries -- and an inner begin
    // cannot move the baseline forward past edits the outer one already made.
    if (depth_++ == 0) {
        baseline_ = doc;
        label_.assign(label);
    }
}

void History::end_interaction(const SceneDocument& doc, int32_t selected) {
    if (depth_ == 0) {
        return; // unbalanced end: nothing was opened
    }
    if (--depth_ > 0) {
        return; // still inside an outer interaction
    }

    const Delta delta = decompose(baseline_, doc);
    if (delta.empty()) {
        return; // nothing happened: no entry, and no redo stack to truncate
    }

    Entry entry;
    entry.label = label_;
    entry.selected = selected;

    // A Snapshot when one is due, and also when the delta would be bigger than
    // one -- at which point a delta costs more and buys a slower undo.
    const size_t since_snapshot = cursor_ - [&] {
        size_t i = cursor_;
        while (i > 0 && !entry_is_snapshot(i)) --i;
        return i;
    }();
    const bool due = since_snapshot + 1 >= interval_;
    if (due || rough_size(delta) >= rough_size(doc)) {
        entry.payload = Snapshot{doc};
    } else {
        entry.payload = delta;
    }
    push(std::move(entry));
}

void History::push(Entry entry) {
    // A new edit invalidates the redo stack: those states are no longer
    // reachable from where the document now is.
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(cursor_) + 1, entries_.end());
    entries_.push_back(std::move(entry));
    cursor_ = entries_.size() - 1;

    if (entries_.size() > kMaxEntries) {
        // Entry 1 is about to become entry 0, and entry 0 MUST carry a Snapshot
        // or a replay has no base to start from. Rebuild what it represents
        // BEFORE dropping entry 0, because afterwards the chain it would have
        // replayed from is gone.
        Snapshot promoted{rebuild(1)};
        entries_.erase(entries_.begin());
        entries_[0].payload = std::move(promoted);
        cursor_ = entries_.size() - 1;
    }
}

SceneDocument History::rebuild(size_t index) const {
    size_t base = index;
    while (base > 0 && !entry_is_snapshot(base)) {
        --base;
    }
    // entry 0 is always a Snapshot, so this cannot fail to find one.
    SceneDocument doc = std::get<Snapshot>(entries_[base].payload).doc;
    for (size_t i = base + 1; i <= index && i < entries_.size(); ++i) {
        if (const Delta* delta = std::get_if<Delta>(&entries_[i].payload)) {
            apply(doc, *delta);
        } else {
            doc = std::get<Snapshot>(entries_[i].payload).doc;
        }
    }
    return doc;
}

std::optional<Entry> History::undo(SceneDocument& out_doc) {
    if (!can_undo()) {
        return std::nullopt;
    }
    --cursor_;
    out_doc = rebuild(cursor_);
    return entries_[cursor_];
}

std::optional<Entry> History::redo(SceneDocument& out_doc) {
    if (!can_redo()) {
        return std::nullopt;
    }
    ++cursor_;
    out_doc = rebuild(cursor_);
    return entries_[cursor_];
}

const std::string& History::undo_label() const {
    // The label of the entry being undone is the one at the CURSOR -- entry N
    // is what edit N produced, so undoing it means undoing that edit.
    return can_undo() ? entries_[cursor_].label : empty_label();
}

const std::string& History::redo_label() const {
    return can_redo() ? entries_[cursor_ + 1].label : empty_label();
}

} // namespace sq
