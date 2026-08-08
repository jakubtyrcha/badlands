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
