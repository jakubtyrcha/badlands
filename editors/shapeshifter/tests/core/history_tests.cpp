#include <doctest.h>

#include <vector>

#include <shapeshifter/ShapeshifterCore.h>

#include "history.h"
#include "scene.h"

using namespace sq;

namespace {

// Documents compare by their node vector IN ORDER, not as a set. Order is
// semantic here -- sdf_fold reduces in vector order and a Subtract carves
// everything before it -- so a round trip that restored the right nodes in the
// wrong sequence would be a rendering bug, not a cosmetic one.
bool same_document(const SceneDocument& a, const SceneDocument& b) {
    if (a.nodes().size() != b.nodes().size()) return false;
    for (size_t i = 0; i < a.nodes().size(); ++i) {
        if (a.nodes()[i] != b.nodes()[i]) return false;
    }
    return a.counters() == b.counters();
}

SceneDocument clone(const SceneDocument& doc) { return doc; }

} // namespace

// --- decompose --------------------------------------------------------------

TEST_CASE("decomposing an unchanged document yields an empty delta") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 0, 0});
    const SceneDocument baseline = clone(doc);

    CHECK(decompose(baseline, doc).empty());
}

// The case the whole design turns on: a drag writes local_position on every
// mouse-move, and what comes out is ONE changed node carrying where it ended.
TEST_CASE("a moved node appears once in changed") {
    SceneDocument doc;
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const SceneDocument baseline = clone(doc);

    doc.find(id)->local_position = simd_float3{1, 0, 0};
    doc.find(id)->local_position = simd_float3{2, 0, 0};
    doc.find(id)->local_position = simd_float3{3, 0, 0}; // the path is not recorded

    const Delta delta = decompose(baseline, doc);
    CHECK(delta.changed.size() == 1);
    CHECK(delta.added.empty());
    CHECK(delta.removed.empty());
    CHECK(delta.changed[0].local_position.x == doctest::Approx(3.0f)); // only the destination
}

TEST_CASE("a spawned node appears in added with its index") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const SceneDocument baseline = clone(doc);

    doc.spawn_unsnapped(Shape::Sphere, Op::Add, simd_float3{0, 0, 0});

    const Delta delta = decompose(baseline, doc);
    REQUIRE(delta.added.size() == 1);
    CHECK(delta.added[0].index == 2);
}

TEST_CASE("a node inserted mid-vector records its real index") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const SceneDocument baseline = clone(doc);

    Node inserted;
    inserted.id = 99;
    doc.insert(inserted, 1);

    const Delta delta = decompose(baseline, doc);
    REQUIRE(delta.added.size() == 1);
    CHECK(delta.added[0].index == 1);
}

TEST_CASE("a removed node appears in removed by id") {
    SceneDocument doc;
    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const SceneDocument baseline = clone(doc);

    doc.remove_node(a, SceneDocument::OrphanPolicy::Reparent);

    const Delta delta = decompose(baseline, doc);
    REQUIRE(delta.removed.size() == 1);
    CHECK(delta.removed[0] == a);
    CHECK(delta.added.empty());
}

// Exact, not approximate. A gesture that ended precisely where it began must
// leave no entry -- that is what makes a click that selects, and a dial press
// that turns nothing, cost nothing.
TEST_CASE("a change and its exact reversal decompose to nothing") {
    SceneDocument doc;
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});
    const SceneDocument baseline = clone(doc);

    doc.find(id)->local_position = simd_float3{9, 9, 9};
    doc.find(id)->local_position = simd_float3{1, 2, 3};

    CHECK(decompose(baseline, doc).empty());
}

TEST_CASE("every mutable field is noticed") {
    SceneDocument doc;
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});

    SUBCASE("rotation") {
        const SceneDocument baseline = clone(doc);
        doc.find(id)->local_rotation = simd_quaternion(0.5f, simd_float3{0, 1, 0});
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
    SUBCASE("scale") {
        const SceneDocument baseline = clone(doc);
        doc.set_node_scale(id, simd_float3{2, 2, 2});
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
    SUBCASE("op") {
        const SceneDocument baseline = clone(doc);
        doc.find(id)->op = Op::Subtract;
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
    SUBCASE("shape param") {
        const SceneDocument baseline = clone(doc);
        doc.find(id)->shape_param = 0.5f;
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
    SUBCASE("parent") {
        const int32_t other = doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
        const SceneDocument baseline = clone(doc);
        ParentRef ref;
        ref.kind = ParentRef::Kind::Node;
        ref.node = other;
        REQUIRE(doc.attach(id, ref));
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
    SUBCASE("attachment name") {
        const SceneDocument baseline = clone(doc);
        doc.find(id)->parent.kind = ParentRef::Kind::Attachment;
        doc.find(id)->parent.attachment = "hand.R";
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
    SUBCASE("contact") {
        const SceneDocument baseline = clone(doc);
        doc.find(id)->contact.valid = true;
        CHECK_FALSE(decompose(baseline, doc).empty());
    }
}

// --- the round trip ---------------------------------------------------------

TEST_CASE("apply(decompose(a, b)) turns a into b") {
    SceneDocument baseline;
    const int32_t keep = baseline.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t doomed = baseline.spawn_unsnapped(Shape::Sphere, Op::Add, simd_float3{1, 0, 0});
    const int32_t moved = baseline.spawn_unsnapped(Shape::Cone, Op::Add, simd_float3{2, 0, 0});

    SceneDocument current = clone(baseline);
    current.remove_node(doomed, SceneDocument::OrphanPolicy::Reparent);
    current.find(moved)->local_position = simd_float3{7, 7, 7};
    current.spawn_unsnapped(Shape::Prism, Op::Subtract, simd_float3{3, 0, 0});
    current.set_node_scale(keep, simd_float3{4, 1, 1});

    SceneDocument replayed = clone(baseline);
    apply(replayed, decompose(baseline, current));

    CHECK(same_document(replayed, current));
}

// The one that protects CSG order. Restoring the right nodes in the wrong
// sequence would change what the scene renders, silently.
TEST_CASE("the round trip preserves node ORDER, element for element") {
    SceneDocument baseline;
    for (int i = 0; i < 5; ++i) {
        baseline.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{float(i), 0, 0});
    }

    // Remove from the middle, then insert two more in the middle.
    SceneDocument current = clone(baseline);
    current.remove_node(current.nodes()[2].id, SceneDocument::OrphanPolicy::Reparent);
    Node a;
    a.id = 100;
    a.op = Op::Subtract;
    current.insert(a, 1);
    Node b;
    b.id = 101;
    current.insert(b, 3);

    SceneDocument replayed = clone(baseline);
    apply(replayed, decompose(baseline, current));

    REQUIRE(replayed.nodes().size() == current.nodes().size());
    for (size_t i = 0; i < current.nodes().size(); ++i) {
        CHECK(replayed.nodes()[i].id == current.nodes()[i].id);
    }
}

TEST_CASE("the round trip carries the counters") {
    SceneDocument baseline;
    SceneDocument current = clone(baseline);
    current.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    current.add_group();

    SceneDocument replayed = clone(baseline);
    apply(replayed, decompose(baseline, current));

    // Without the counters a spawn after a replay reuses an id, and the
    // auto-names restart at 1.
    CHECK(replayed.counters() == current.counters());
    const int32_t next_replayed = replayed.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{});
    const int32_t next_current = current.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{});
    CHECK(next_replayed == next_current);
    CHECK(replayed.find(next_replayed)->name == current.find(next_current)->name);
}

TEST_CASE("applying an empty delta changes nothing but the counters") {
    SceneDocument doc;
    doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{1, 2, 3});
    const SceneDocument expected = clone(doc);

    apply(doc, decompose(expected, doc));

    CHECK(same_document(doc, expected));
}

TEST_CASE("a hierarchy round-trips, parents and contacts intact") {
    SceneDocument baseline;

    SceneDocument current = clone(baseline);
    const int32_t group = current.add_group();
    current.set_node_scale(group, simd_float3{3, 3, 3});
    const int32_t base = current.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{0, 0, 0});
    const int32_t detail = current.spawn_snapped(Shape::Sphere, Op::Add, simd_float3{0, 0.5f, 0},
                                                 simd_float3{0, 1, 0}, base);
    ParentRef ref;
    ref.kind = ParentRef::Kind::Node;
    ref.node = group;
    REQUIRE(current.attach(base, ref));

    SceneDocument replayed = clone(baseline);
    apply(replayed, decompose(baseline, current));

    CHECK(same_document(replayed, current));
    // And the resolved placement agrees, not merely the stored fields.
    CHECK(replayed.placement(detail).frame.position.y ==
          doctest::Approx(current.placement(detail).frame.position.y));
    CHECK(replayed.find(base)->parent.node == group);
    CHECK(replayed.find(detail)->contact.surface == base);
}

// --- History: keyframes, deltas, replay -------------------------------------

namespace {

// The app's job in miniature: bracket a gesture, do the work, close it.
template <typename Fn>
void interaction(History& history, SceneDocument& doc, const char* label, Fn&& body) {
    history.begin_interaction(label, doc);
    body();
    history.end_interaction(doc, kInvalidNode);
}

int32_t spawn(SceneDocument& doc, float x) {
    return doc.spawn_unsnapped(Shape::Cube, Op::Add, simd_float3{x, 0, 0});
}

} // namespace

TEST_CASE("an edit inside an interaction can be undone") {
    SceneDocument doc;
    History history(doc, kInvalidNode);

    interaction(history, doc, "Spawn", [&] { spawn(doc, 1); });
    REQUIRE(doc.nodes().size() == 1);
    REQUIRE(history.can_undo());

    REQUIRE(history.undo(doc).has_value());
    CHECK(doc.nodes().empty());
    CHECK_FALSE(history.can_undo());
}

TEST_CASE("undo then redo restores the edit") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    interaction(history, doc, "Spawn", [&] { spawn(doc, 1); });

    history.undo(doc);
    REQUIRE(doc.nodes().empty());
    REQUIRE(history.can_redo());
    REQUIRE(history.redo(doc).has_value());
    CHECK(doc.nodes().size() == 1);
}

TEST_CASE("an interaction that changes nothing leaves no entry") {
    SceneDocument doc;
    spawn(doc, 1);
    History history(doc, kInvalidNode);

    interaction(history, doc, "Select", [] {});

    CHECK_FALSE(history.can_undo());
    CHECK(history.entry_count() == 1); // just the initial state
}

TEST_CASE("nested interactions produce exactly one entry") {
    SceneDocument doc;
    History history(doc, kInvalidNode);

    history.begin_interaction("Outer", doc);
    spawn(doc, 1);
    history.begin_interaction("Inner", doc); // must not re-baseline
    spawn(doc, 2);
    history.end_interaction(doc, kInvalidNode);
    CHECK(history.in_interaction());
    history.end_interaction(doc, kInvalidNode);

    CHECK(history.entry_count() == 2); // initial + one
    history.undo(doc);
    CHECK(doc.nodes().empty()); // BOTH spawns undone together
}

// THE ONE THAT PINS "the delta applies at drag end, not along the way".
TEST_CASE("a drag's intermediate states are not recorded") {
    SceneDocument doc;
    const int32_t id = spawn(doc, 0);
    History history(doc, kInvalidNode);

    history.begin_interaction("Move", doc);
    for (int i = 1; i <= 20; ++i) { // twenty mouse-moves
        doc.find(id)->local_position = simd_float3{float(i), 0, 0};
    }
    history.end_interaction(doc, kInvalidNode);

    CHECK(history.entry_count() == 2); // initial + ONE, not twenty-one
    REQUIRE(history.undo(doc).has_value());
    // Bound to a const first: doctest's CHECK binds the compared sub-expression
    // to a reference, and Clang only allows that for const accesses of
    // ext_vector_type components (the pattern scene_tests.cpp documents).
    const simd_float3 restored = doc.find(id)->local_position;
    CHECK(restored.x == doctest::Approx(0.0f));
    CHECK_FALSE(history.can_undo());
}

TEST_CASE("a new edit truncates the redo stack") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    interaction(history, doc, "A", [&] { spawn(doc, 1); });
    interaction(history, doc, "B", [&] { spawn(doc, 2); });

    history.undo(doc);
    REQUIRE(history.can_redo());
    interaction(history, doc, "C", [&] { spawn(doc, 3); });

    CHECK_FALSE(history.can_redo());
}

TEST_CASE("selection is restored per entry") {
    SceneDocument doc;
    const int32_t id = spawn(doc, 0);
    History history(doc, id);

    history.begin_interaction("Delete", doc);
    doc.remove_node(id, SceneDocument::OrphanPolicy::Reparent);
    history.end_interaction(doc, kInvalidNode);

    const std::optional<Entry> entry = history.undo(doc);
    REQUIRE(entry.has_value());
    CHECK(entry->selected == id); // the node comes back SELECTED
    CHECK(doc.find(id) != nullptr);
}

TEST_CASE("labels report the pending entries") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    interaction(history, doc, "Move", [&] { spawn(doc, 1); });

    CHECK(history.undo_label() == "Move");
    CHECK(history.redo_label().empty());

    history.undo(doc);
    CHECK(history.undo_label().empty());
    CHECK(history.redo_label() == "Move");
}

// --- keyframe cadence and replay --------------------------------------------

// Two rules decide a payload, and they are tested apart: the interval, and
// "would a delta cost more than a snapshot anyway".
TEST_CASE("a snapshot lands every interval") {
    // A document big enough that ONE changed node is unambiguously the cheaper
    // payload -- otherwise the size rule below fires first and the cadence is
    // not what is under test.
    SceneDocument doc;
    for (int i = 0; i < 50; ++i) {
        spawn(doc, float(i));
    }
    const int32_t id = doc.nodes()[0].id;

    History history(doc, kInvalidNode);
    history.set_snapshot_interval(4);

    for (int i = 1; i <= 12; ++i) {
        interaction(history, doc, "Move",
                    [&] { doc.find(id)->local_position = simd_float3{float(i), 0, 0}; });
    }

    REQUIRE(history.entry_count() == 13); // initial + 12
    for (size_t i = 0; i < history.entry_count(); ++i) {
        CHECK(history.entry_is_snapshot(i) == (i % 4 == 0));
    }
}

// The other rule. Spawning the first node into an empty document produces a
// delta carrying that whole node PLUS its index, against a document that is
// just the node -- so the snapshot is both smaller and faster to replay.
TEST_CASE("a delta that would cost more than a snapshot is stored as a snapshot") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    history.set_snapshot_interval(1000); // far out of reach, so only size decides

    interaction(history, doc, "Spawn", [&] { spawn(doc, 0); });

    REQUIRE(history.entry_count() == 2);
    CHECK(history.entry_is_snapshot(1));
}

// And the converse, so the rule is not vacuously "always snapshot": a small
// edit to a large document stays a delta.
TEST_CASE("a small edit to a large document is stored as a delta") {
    SceneDocument doc;
    for (int i = 0; i < 50; ++i) {
        spawn(doc, float(i));
    }
    const int32_t id = doc.nodes()[0].id;

    History history(doc, kInvalidNode);
    history.set_snapshot_interval(1000);

    interaction(history, doc, "Move",
                [&] { doc.find(id)->local_position = simd_float3{9, 9, 9}; });

    REQUIRE(history.entry_count() == 2);
    CHECK_FALSE(history.entry_is_snapshot(1));
}

// The replay-correctness test: undoing across a keyframe boundary has to land
// on the same document a direct sequence of edits would have produced.
TEST_CASE("undo across a snapshot boundary replays correctly") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    history.set_snapshot_interval(4);

    for (int i = 0; i < 10; ++i) {
        interaction(history, doc, "Spawn", [&] { spawn(doc, float(i)); });
    }

    // A reference built by replaying the first 6 edits and nothing else.
    SceneDocument reference;
    for (int i = 0; i < 6; ++i) {
        spawn(reference, float(i));
    }

    for (int i = 0; i < 4; ++i) { // 10 -> 6
        history.undo(doc);
    }

    CHECK(same_document(doc, reference));
}

TEST_CASE("undoing all the way reaches the initial state") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    history.set_snapshot_interval(3);

    for (int i = 0; i < 10; ++i) {
        interaction(history, doc, "Spawn", [&] { spawn(doc, float(i)); });
    }
    while (history.can_undo()) {
        history.undo(doc);
    }

    CHECK(doc.nodes().empty());
    CHECK(same_document(doc, SceneDocument{}));
}

TEST_CASE("redo forward retraces the same states") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    history.set_snapshot_interval(3);

    for (int i = 0; i < 8; ++i) {
        interaction(history, doc, "Spawn", [&] { spawn(doc, float(i)); });
    }
    const SceneDocument expected = clone(doc);

    while (history.can_undo()) history.undo(doc);
    while (history.can_redo()) history.redo(doc);

    CHECK(same_document(doc, expected));
}

// Undo past a REMOVAL is where an inverse-based scheme would have to restore a
// node at its index; replaying forward from a keyframe gets it for free.
TEST_CASE("undo restores a removed node at its original index") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    history.set_snapshot_interval(8);
    for (int i = 0; i < 5; ++i) {
        interaction(history, doc, "Spawn", [&] { spawn(doc, float(i)); });
    }
    const SceneDocument before = clone(doc);
    const int32_t middle = doc.nodes()[2].id;

    interaction(history, doc, "Delete", [&] {
        doc.remove_node(middle, SceneDocument::OrphanPolicy::Reparent);
    });
    history.undo(doc);

    CHECK(same_document(doc, before));
    CHECK(doc.nodes()[2].id == middle); // back where it was, not merely back
}

TEST_CASE("the entry cap drops the oldest and keeps a replayable base") {
    SceneDocument doc;
    History history(doc, kInvalidNode);
    history.set_snapshot_interval(8);

    for (size_t i = 0; i < History::kMaxEntries + 10; ++i) {
        interaction(history, doc, "Spawn", [&] { spawn(doc, float(i)); });
    }

    CHECK(history.entry_count() == History::kMaxEntries);
    // Entry 0 must carry a Snapshot, or every replay has no base.
    CHECK(history.entry_is_snapshot(0));
    // And undo still works all the way back through the surviving range.
    const SceneDocument expected = clone(doc);
    while (history.can_undo()) history.undo(doc);
    while (history.can_redo()) history.redo(doc);
    CHECK(same_document(doc, expected));
}

TEST_CASE("an unbalanced end_interaction is ignored") {
    SceneDocument doc;
    History history(doc, kInvalidNode);

    history.end_interaction(doc, kInvalidNode); // never begun
    spawn(doc, 1);
    history.end_interaction(doc, kInvalidNode);

    CHECK_FALSE(history.can_undo());
}

TEST_CASE("undo and redo at the ends return nullopt and touch nothing") {
    SceneDocument doc;
    spawn(doc, 1);
    History history(doc, kInvalidNode);

    CHECK_FALSE(history.undo(doc).has_value());
    CHECK_FALSE(history.redo(doc).has_value());
    CHECK(doc.nodes().size() == 1);
}
