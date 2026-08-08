#include "history.h"

#include <algorithm>

namespace sq {

bool Delta::empty() const {
    return added.empty() && removed.empty() && changed.empty();
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

    doc.set_counters(delta.counters);
}

} // namespace sq
