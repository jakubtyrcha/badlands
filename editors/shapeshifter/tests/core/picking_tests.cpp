#include <doctest.h>

#include <array>
#include <cmath>
#include <string>

#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "picking.h"
#include "scene.h"
#include "sdf.h" // evaluate_scene_sdf -- the zero-set cross-validation below

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — matches the pattern
// already used in lines_tests.cpp/camera_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// A node at the origin with default scale, so its half-extents are 0.5 and it
// occupies exactly the unit primitive the deleted ray_unit_cube/ray_unit_sphere
// were written against -- which is what lets the cases below keep their
// hand-derived geometry unchanged across the switch to tracing.
Node unit_node(Shape shape) {
    Node node;
    node.id = 1;
    node.shape = shape;
    return node;
}

// Trace-derived values land within the hit epsilon of the true surface rather
// than exactly on it, so they need a stated tolerance where the analytic
// results did not. Still far tighter than any of the geometry being
// distinguished here, all of which differs by 0.5 or more.
constexpr double kTraceTol = 1e-3;

void check_float3_traced(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(kTraceTol));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(kTraceTol));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(kTraceTol));
}

// unit_node with the dial set. Node::shape_param defaults to 0, which is NOT
// every shape's spec default (the capsule's is 1, the prism's 6) -- so every
// case that does not say otherwise is at param 0, and anything wanting an
// endpoint has to ask for it.
Node unit_node(Shape shape, float param) {
    Node node = unit_node(shape);
    node.shape_param = param;
    return node;
}

struct ShapeCase {
    Shape shape;
    const char* name;
};

constexpr std::array<ShapeCase, kShapeCount> kAllShapes = {{
    {Shape::Cube, "Cube"},         {Shape::Sphere, "Sphere"},
    {Shape::Cone, "Cone"},         {Shape::Capsule, "Capsule"},
    {Shape::Octahedron, "Octahedron"}, {Shape::Pyramid, "Pyramid"},
    {Shape::Prism, "Prism"},       {Shape::Vesica, "Vesica"},
}};

// |sdf| at an accepted hit is under kTraceHitEps (1e-5) by construction; the
// margin here absorbs the float error in mapping the local hit back to world.
constexpr float kSurfaceTol = 1e-4f;
// sdf_tests pins "no shape escapes its own bounding box" field-side; this is
// the ray-side corollary, so it need only absorb the trace's own slack.
constexpr float kBoxTol = 1e-3f;

// Everything raycast_node promises, checked against the node's OWN field rather
// than a closed form -- which is what lets it run for the shapes that have no
// closed form to check against, and what makes it survive a shape's formula
// being retuned.
//
// `entering` distinguishes the two normal conventions: local_normal points
// OUTWARD on an exit hit as well as an entry one, so it opposes the ray on the
// way in and follows it on the way out.
void check_hit_invariants(const Node& node, const Ray& ray, const RayHit& hit, bool entering) {
    const SdfNode sn = local_sdf_node(node);
    const simd_float3 dir = simd_normalize(ray.dir);

    // On the surface.
    CHECK(std::fabs(sdf_eval_node(sn, hit.point)) < kSurfaceTol);

    // In front of the origin, and `t` really is the distance along the
    // normalized ray to the point reported.
    CHECK(hit.t > 0.0f);
    check_float3_traced(hit.point, ray.origin + hit.t * dir);

    // Inside the box the node's half-extents describe.
    CHECK(std::fabs(hit.point.x) <= 0.5f + kBoxTol);
    CHECK(std::fabs(hit.point.y) <= 0.5f + kBoxTol);
    CHECK(std::fabs(hit.point.z) <= 0.5f + kBoxTol);

    // Unit, and oriented outward.
    CHECK(simd_length(hit.normal) == doctest::Approx(1.0f).epsilon(kTraceTol));
    CHECK(simd_dot(hit.normal, dir) * (entering ? 1.0f : -1.0f) < 0.0f);
}

} // namespace

// --- raycast_node: the sphere trace that replaced the analytic primitives ----
//
// These are the ray_unit_sphere/ray_unit_cube cases, kept case-for-case and
// re-pointed at raycast_node against a unit-scaled node. The geometry they
// assert is unchanged and still hand-derived; what changed is how it is found.

TEST_CASE("raycast_node, sphere: head-on hit through the near pole") {
    // Radius 0.5 at the origin, eye 2 out along +z: the surface is 1.5 away and
    // the outward normal there is +z.
    const auto hit = raycast_node(unit_node(Shape::Sphere),
                                  Ray{{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(1.5f).epsilon(kTraceTol));
    check_float3_traced(hit->point, simd_float3{0.0f, 0.0f, 0.5f});
    check_float3_traced(hit->normal, simd_float3{0.0f, 0.0f, 1.0f});
}

TEST_CASE("raycast_node, sphere: outside the silhouette misses, just inside hits") {
    // origin.x = 0.6 is outside the radius-0.5 silhouette of a -z ray.
    CHECK_FALSE(raycast_node(unit_node(Shape::Sphere),
                             Ray{{0.6f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}}).has_value());

    // origin.x = 0.45 is inside it: the chord's half-length is
    // sqrt(0.25 - 0.2025) = 0.2179, so entry is at z = +0.2179, i.e. t = 1.7821.
    const auto hit = raycast_node(unit_node(Shape::Sphere),
                                  Ray{{0.45f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    const float chord = std::sqrt(0.25f - 0.45f * 0.45f);
    CHECK(hit->t == doctest::Approx(2.0f - chord).epsilon(kTraceTol));
    check_float3_traced(hit->point, simd_float3{0.45f, 0.0f, chord});

    // The EXACTLY tangent ray (origin.x = 0.5) is deliberately not asserted
    // either way. A sphere trace approaches a tangency quadratically -- the
    // step size falls off as fast as the remaining distance -- so it exhausts
    // its budget rather than converging, and reports a miss where the analytic
    // solver found a double root. This is the grazing-ray artifact raycast_node
    // documents, it is measure-zero among real cursor rays, and pinning either
    // answer here would be pinning a wart.
}

TEST_CASE("raycast_node, sphere: origin inside takes the exit surface") {
    // The behaviour that dictates marching by |d| rather than d: dollying in is
    // unclamped, so the eye really does end up inside geometry, and whatever
    // comes back must be in front of the ray.
    const auto hit = raycast_node(unit_node(Shape::Sphere),
                                  Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(0.5f).epsilon(kTraceTol));
    check_float3_traced(hit->point, simd_float3{0.0f, 0.0f, 0.5f});
    check_float3_traced(hit->normal, simd_float3{0.0f, 0.0f, 1.0f});
}

TEST_CASE("raycast_node, cube: face hits from all 6 axis directions") {
    struct Case {
        simd_float3 origin;
        simd_float3 dir;
        simd_float3 normal;
        simd_float3 point;
    };
    // Every case starts 2 units out along one axis aimed straight in; it
    // crosses that axis's near face (at +-0.5) after traveling 2-0.5=1.5
    // units, with the outward normal opposing the direction of travel.
    const std::array<Case, 6> cases = {{
        {{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.5f}},
        {{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -0.5f}},
        {{2.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}},
        {{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-0.5f, 0.0f, 0.0f}},
        {{0.0f, 2.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.5f, 0.0f}},
        {{0.0f, -2.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -0.5f, 0.0f}},
    }};

    for (size_t i = 0; i < cases.size(); ++i) {
        CAPTURE(i);
        const Case& c = cases[i];
        const auto hit = raycast_node(unit_node(Shape::Cube), Ray{c.origin, c.dir});
        REQUIRE(hit.has_value());
        CHECK(hit->t == doctest::Approx(1.5f).epsilon(kTraceTol));
        check_float3_traced(hit->point, c.point);
        check_float3_traced(hit->normal, c.normal);
    }
}

TEST_CASE("raycast_node, cube: angled entry hits the correct face") {
    // Aimed along -x from (2, 0.25, 0.25): it crosses the +x face at x = 0.5,
    // 1.5 away, well inside that face's y/z extent.
    const simd_float3 dir = simd_normalize(simd_float3{-1.0f, 0.0f, 0.0f});
    const auto hit = raycast_node(unit_node(Shape::Cube), Ray{{2.0f, 0.25f, 0.25f}, dir});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(1.5f).epsilon(kTraceTol));
    check_float3_traced(hit->point, simd_float3{0.5f, 0.25f, 0.25f});
    check_float3_traced(hit->normal, simd_float3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("raycast_node, cube: a ray passing clear of the box misses") {
    // y = 0.8 is outside [-0.5, 0.5] for the whole of the ray's travel, so it
    // never comes within reach of the surface.
    CHECK_FALSE(raycast_node(unit_node(Shape::Cube),
                             Ray{{0.0f, 0.8f, 2.0f}, {0.0f, 0.0f, -1.0f}}).has_value());
}

TEST_CASE("raycast_node, cube: origin inside takes the exit face") {
    const auto hit = raycast_node(unit_node(Shape::Cube),
                                  Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(0.5f).epsilon(kTraceTol));
    check_float3_traced(hit->point, simd_float3{0.5f, 0.0f, 0.0f});
    check_float3_traced(hit->normal, simd_float3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("raycast_node: a non-unit ray direction still reports t as a world distance") {
    // The contract that lets raycast_scene compare t across nodes: dir is
    // normalized on entry, so a caller handing over an unnormalized ray gets
    // the same answer rather than a t scaled by its length.
    const auto unit = raycast_node(unit_node(Shape::Cube),
                                   Ray{{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}});
    const auto scaled = raycast_node(unit_node(Shape::Cube),
                                     Ray{{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -7.0f}});
    REQUIRE(unit.has_value());
    REQUIRE(scaled.has_value());
    CHECK(scaled->t == doctest::Approx(unit->t).epsilon(kTraceTol));
    check_float3_traced(scaled->point, unit->point);
}

// --- ray vs the underlying primitives, for all eight shapes -----------------
//
// The cases above cover the two shapes that had hand-written intersectors
// before the switch to tracing. The six added since have never been traced in a
// test at all: sdf_tests.cpp pins their FIELDS (zero set inside the box,
// 1-Lipschitz, numpy-pinned samples), which is a different claim from "a ray
// finds that surface where the primitive puts it".
//
// Deliberately not exact. The trace stops within kTraceHitEps of the surface
// and the normal is a tetrahedron-offset finite difference, so these assert a
// hit is ON the primitive to a stated tolerance rather than at a pinned float.
// The editor does not need better: a click selects a node, and a snapped spawn
// places the new node's CENTRE on the hit.

TEST_CASE("raycast_node: every shape is hit from all six axis directions") {
    const std::array<simd_float3, 6> dirs = {{
        {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
    }};
    for (const ShapeCase& sc : kAllShapes) {
        CAPTURE(sc.name);
        const Node node = unit_node(sc.shape);
        for (size_t i = 0; i < dirs.size(); ++i) {
            CAPTURE(i);
            // Origin 2 units out along the opposite axis, aimed straight in.
            const Ray ray{-2.0f * dirs[i], dirs[i]};
            const auto hit = raycast_node(node, ray);
            REQUIRE(hit.has_value());
            check_hit_invariants(node, ray, *hit, true);
        }
    }
}

TEST_CASE("raycast_node: every shape's hit lands where its own primitive says") {
    // Cone and pyramid share a slant normal: both taper from a half-size of 0.5
    // at y = -0.5 to a point at y = +0.5, so the face direction is (-0.5, 1)
    // and its outward perpendicular is (1, 0.5) normalized. That also puts the
    // half-size at y = 0 at exactly 0.25, which is where the two slant cases
    // below are aimed.
    const simd_float3 taper_n = simd_normalize(simd_float3{1.0f, 0.5f, 0.0f});
    // The regular polygon's `r` is its CIRCUMRADIUS (pinned in sdf_tests), so
    // the face opposite the +z vertex sits at the apothem, r*cos(pi/3) = 0.25.
    constexpr float kPrismApothem = 0.25f;
    // Octahedron face plane |x|+|y|+|z| = 0.5, met by a ray from (2,2,2) aimed
    // at the origin: the sum falls as 6 - sqrt(3)t, so t = 5.5/sqrt(3) and the
    // hit is at 1/6 on each axis -- inside the face triangle, not on an edge.
    const float oct_t = 5.5f / std::sqrt(3.0f);
    const float inv_sqrt3 = 1.0f / std::sqrt(3.0f);

    struct Pin {
        Shape shape;
        const char* what;
        simd_float3 origin;
        simd_float3 dir;
        float t;
        simd_float3 point;
        bool pin_normal;   // false where the hit is an edge or a vertex
        simd_float3 normal;
    };

    const std::array<Pin, 14> pins = {{
        {Shape::Cube, "face at +z", {0, 0, 2}, {0, 0, -1},
         1.5f, {0, 0, 0.5f}, true, {0, 0, 1}},
        {Shape::Sphere, "radius 0.5 from +z", {0, 0, 2}, {0, 0, -1},
         1.5f, {0, 0, 0.5f}, true, {0, 0, 1}},

        // Sharp cone at param 0: base radius 0.5 at y = -0.5, apex at y = +0.5.
        {Shape::Cone, "base cap", {0, -2, 0}, {0, 1, 0},
         1.5f, {0, -0.5f, 0}, true, {0, -1, 0}},
        {Shape::Cone, "slant at y = 0", {2, 0, 0}, {-1, 0, 0},
         1.75f, {0.25f, 0, 0}, true, taper_n},

        // Param 0 is a FLAT-capped cylinder, not a capsule -- the spec default
        // of 1 is what makes it look like its name, and unit_node does not
        // apply spec defaults.
        {Shape::Capsule, "flat top cap", {0, 2, 0}, {0, -1, 0},
         1.5f, {0, 0.5f, 0}, true, {0, 1, 0}},
        {Shape::Capsule, "cylinder wall", {2, 0, 0}, {-1, 0, 0},
         1.5f, {0.5f, 0, 0}, true, {1, 0, 0}},

        {Shape::Octahedron, "face centre", {2, 2, 2}, {-1, -1, -1},
         oct_t, {1.0f / 6.0f, 1.0f / 6.0f, 1.0f / 6.0f}, true,
         {inv_sqrt3, inv_sqrt3, inv_sqrt3}},
        // Vertices sit at s = 0.5 on each axis. No normal pinned: four faces
        // meet here, so the finite difference is averaging an ambiguity.
        {Shape::Octahedron, "+y vertex", {0, 2, 0}, {0, -1, 0},
         1.5f, {0, 0.5f, 0}, false, {0, 0, 0}},

        // Square frustum at param 0: base half-size 0.5 at y = -0.5, apex above.
        {Shape::Pyramid, "base cap", {0, -2, 0}, {0, 1, 0},
         1.5f, {0, -0.5f, 0}, true, {0, -1, 0}},
        {Shape::Pyramid, "slant at y = 0", {2, 0, 0}, {-1, 0, 0},
         1.75f, {0.25f, 0, 0}, true, taper_n},

        // Param 0 floors the side count at 3, so this is a triangular prism.
        {Shape::Prism, "+z vertex edge", {0, 0, 2}, {0, 0, -1},
         1.5f, {0, 0, 0.5f}, false, {0, 0, 0}},
        {Shape::Prism, "face opposite that vertex", {0, 0, -2}, {0, 0, 1},
         2.0f - kPrismApothem, {0, 0, -kPrismApothem}, true, {0, 0, -1}},
        {Shape::Prism, "top cap", {0, 2, 0}, {0, -1, 0},
         1.5f, {0, 0.5f, 0}, true, {0, 1, 0}},

        // w == h.y in a cube box, which sdf_sd_vesica documents as EXACTLY a
        // sphere of radius h -- so the default vesica is a ball, and that is
        // the fact worth pinning here.
        {Shape::Vesica, "sphere of radius 0.5", {0, 0, 2}, {0, 0, -1},
         1.5f, {0, 0, 0.5f}, true, {0, 0, 1}},
    }};

    for (const Pin& p : pins) {
        CAPTURE(p.what);
        const Node node = unit_node(p.shape);
        const Ray ray{p.origin, p.dir};
        const auto hit = raycast_node(node, ray);
        REQUIRE(hit.has_value());
        CHECK(hit->t == doctest::Approx(p.t).epsilon(kTraceTol));
        check_float3_traced(hit->point, p.point);
        check_hit_invariants(node, ray, *hit, true);
        if (p.pin_normal) {
            check_float3_traced(hit->normal, p.normal);
        }
    }
}

TEST_CASE("raycast_node: a ray clear of the box misses every shape") {
    for (const ShapeCase& sc : kAllShapes) {
        CAPTURE(sc.name);
        // No shape escapes its own bounding box (pinned in sdf_tests), so a ray
        // held a whole box-width off every axis of travel cannot reach one.
        CHECK_FALSE(raycast_node(unit_node(sc.shape),
                                 Ray{{1.5f, 1.5f, 2.0f}, {0.0f, 0.0f, -1.0f}}).has_value());
    }
}

TEST_CASE("raycast_node: an origin inside any shape returns its exit surface") {
    // Dollying in is unclamped, so the eye really does end up inside geometry.
    // Every shape contains its own centre, so this is well defined for all of
    // them -- and whatever comes back must be in FRONT of the ray.
    for (const ShapeCase& sc : kAllShapes) {
        CAPTURE(sc.name);
        const Node node = unit_node(sc.shape);
        const Ray ray{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
        const auto hit = raycast_node(node, ray);
        REQUIRE(hit.has_value());
        check_hit_invariants(node, ray, *hit, false);
    }
}

TEST_CASE("raycast_node: both ends of every dial are still traceable") {
    // The dial reshapes the primitive -- a cone becomes a cylinder, a cube
    // becomes a ball, a prism goes from a triangle to a 12-gon -- so the closed
    // forms above stop applying and the invariants are what carries over.
    for (const ShapeCase& sc : kAllShapes) {
        CAPTURE(sc.name);
        const ShapeParamSpec spec = shape_param_spec(sc.shape);
        if (!spec.has_param) {
            continue;   // the sphere, which has nothing left to vary
        }
        for (const float param : {spec.min_value, spec.default_value, spec.max_value}) {
            CAPTURE(param);
            const Node node = unit_node(sc.shape, param);
            const Ray ray{{2.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
            const auto hit = raycast_node(node, ray);
            REQUIRE(hit.has_value());
            check_hit_invariants(node, ray, *hit, true);
        }
    }
}

// --- raycast_scene ---------------------------------------------------------

TEST_CASE("raycast_scene: non-uniform scale + translation still renormalizes to a unit world normal") {
    // world_from_local: local (x,y,z) -> (3+2x, y, z). Its inverse maps
    // world X -> local ((X-3)/2, Y, Z), so local origin (6,0,0) -> (1.5,0,0)
    // and local dir (-1,0,0,w=0) -> (-0.5,0,0) (NOT renormalized).
    // Sphere test: a=dot(d,d)=0.25, b=2*dot(o,d)=2*(1.5*-0.5)=-1.5,
    // c=dot(o,o)-0.25=2.25-0.25=2.0, disc=2.25-2.0=0.25, sqrt=0.5;
    // roots (1.5-0.5)/0.5=2.0 and (1.5+0.5)/0.5=4.0 -> t_local=2.0,
    // local point=(1.5,0,0)+2*(-0.5,0,0)=(0.5,0,0), local normal=(1,0,0).
    // World point = M*(0.5,0,0,1) = (3+2*0.5,0,0) = (4,0,0).
    // Minv's linear part is diag(0.5,1,1); its transpose is itself, so the
    // raw world normal is (0.5,0,0) before renormalizing back to unit length.
    SceneDocument doc;
    Node node;
    node.id = 7;
    node.shape = Shape::Sphere;
    node.position = {3.0f, 0.0f, 0.0f};
    node.scale = {2.0f, 1.0f, 1.0f};
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{{6.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 7);
    check_float3_traced(hit->hit.point, simd_float3{4.0f, 0.0f, 0.0f});
    CHECK(simd_length(hit->hit.normal) == doctest::Approx(1.0f));
    check_float3_traced(hit->hit.normal, simd_float3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("raycast_scene: translated cube") {
    SceneDocument doc;
    Node node;
    node.id = 9;
    node.shape = Shape::Cube;
    node.position = {0.0f, 3.0f, 0.0f};
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{{0.0f, 3.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 9);
    check_float3_traced(hit->hit.point, simd_float3{0.0f, 3.0f, 0.5f});
}

TEST_CASE("raycast_scene: nearest node wins, miss-all is nullopt, Subtract nodes are pickable") {
    SceneDocument doc;

    Node front; // z=0
    front.id = 10;
    front.shape = Shape::Sphere;
    front.op = Op::Add;
    front.position = {0.0f, 0.0f, 0.0f};
    doc.add(front);

    Node back; // z=-3
    back.id = 20;
    back.shape = Shape::Sphere;
    back.op = Op::Subtract;
    back.position = {0.0f, 0.0f, -3.0f};
    doc.add(back);

    SUBCASE("ray down -z from z=5 hits the front sphere first") {
        const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
        REQUIRE(hit.has_value());
        CHECK(hit->node_id == 10);
    }

    SUBCASE("a ray offset far enough in xy misses both spheres") {
        CHECK_FALSE(raycast_scene(doc, Ray{{5.0f, 5.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}).has_value());
    }

    SUBCASE("Subtract-op node is pickable like any other") {
        // Starting between the two spheres (z=-2) and aiming further -z only
        // reaches the back (Subtract) sphere's near face at world z=-2.5.
        const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, -1.0f}});
        REQUIRE(hit.has_value());
        CHECK(hit->node_id == 20);
    }
}

// --- raycast_scene: validation for camera-navigation use -------------------
//
// resolve_focus (navigation.h) runs raycast_scene at every camera-gesture start
// and on every mouse-move that feeds the focus-preview dot, so the cases below
// pin the properties that whole path leans on. Worth recording because it is
// worth recording because it changed: raycast_scene used to solve analytic
// ray-primitive intersections per node, and now sphere-traces each node's own
// SDF in its rigid local frame (see raycast_node). Hits therefore land within
// the trace epsilon of the surface rather than exactly on it, which is what
// kTraceTol above exists to state out loud.

TEST_CASE("raycast_scene: oblique ellipsoid normal matches the closed form") {
    // A sphere scaled (2,1,1) is the ellipsoid (x/a)^2 + (y/b)^2 + (z/c)^2 = 1
    // with a=1, b=c=0.5. The gradient of that implicit function gives the exact
    // world normal at P: normalize((Px/a^2, Py/b^2, Pz/c^2)).
    //
    // This case exists because the non-uniform test above hits ALONG the scale
    // axis, where the expected normal is trivially +x and every plausible
    // implementation agrees. Here they diverge: the correct
    // transpose(inverse(M)) normal comes out (1,2,0)/sqrt(5), while both common
    // mistakes -- the radial direction normalize(P), and the local normal
    // pushed through M's linear part -- give (2,1,0)/sqrt(5), components
    // swapped.
    constexpr float a = 1.0f, b = 0.5f, c = 0.5f;

    // A point on the ellipsoid: local (0.5cos, 0.5sin, 0) -> world (cos, 0.5sin, 0),
    // at 45 degrees so neither component can be confused for the other.
    const float k = std::sqrt(0.5f);
    const simd_float3 P = {k, 0.5f * k, 0.0f};

    const simd_float3 expected_normal =
        simd_normalize(simd_float3{P.x / (a * a), P.y / (b * b), P.z / (c * c)});

    // Approach along the outward normal. For a convex body every point of
    // P + (d-t)*n with t < d lies strictly outside the supporting plane at P,
    // so the first surface crossing is exactly P, at t == d.
    const simd_float3 origin = P + 3.0f * expected_normal;

    SceneDocument doc;
    Node node;
    node.id = 3;
    node.shape = Shape::Sphere;
    node.scale = {2.0f, 1.0f, 1.0f};
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{origin, -expected_normal});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 3);
    CHECK(hit->hit.t == doctest::Approx(3.0f).epsilon(kTraceTol));
    check_float3_traced(hit->hit.point, P);
    check_float3_traced(hit->hit.normal, expected_normal);
    CHECK(simd_length(hit->hit.normal) == doctest::Approx(1.0f));
    // Explicit discrimination against the swapped-component answer, so a
    // regression to either wrong transform fails on an obvious comparison
    // rather than only on the approx check above.
    CHECK(hit->hit.normal.x < hit->hit.normal.y);
}

TEST_CASE("raycast_scene: world_t is a true distance and the hit lies on the query ray") {
    // world_t is derived as dot(world_point - origin, dir), a real distance
    // only because `dir` is unit (Camera::ray_through_view_point normalizes,
    // and resolve_focus feeds it those same rays). Pan and dolly scale their
    // step by this distance, so an error here surfaces as gestures moving the
    // wrong AMOUNT rather than as a visibly wrong pick.
    //
    // The related invariant, worth recording because it explains why the local
    // ray's direction is deliberately left un-renormalized after the inverse
    // transform: M*(Minv*o + t*Minv_lin*d) == o + t*d, so the local parameter
    // and the world parameter coincide. That is what lets nearest-wins compare
    // across nodes with wildly different, non-uniform scales at all.
    SceneDocument doc;

    Node squashed; // world semi-axes (0.1, 1.5, 0.5); reaches z=+0.5
    squashed.id = 1;
    squashed.shape = Shape::Sphere;
    squashed.position = {0.0f, 0.0f, 0.0f};
    squashed.scale = {0.2f, 3.0f, 1.0f};
    doc.add(squashed);

    Node stretched; // world half-extents (4, 0.25, 1); near face at z=-5
    stretched.id = 2;
    stretched.shape = Shape::Cube;
    stretched.position = {0.0f, 0.0f, -6.0f};
    stretched.scale = {8.0f, 0.5f, 2.0f};
    doc.add(stretched);

    const simd_float3 origin = {0.0f, 0.0f, 9.0f};
    const simd_float3 dir = {0.0f, 0.0f, -1.0f};

    const auto hit = raycast_scene(doc, Ray{origin, dir});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 1); // the sphere is nearer despite the cube being far larger
    CHECK(hit->hit.t == doctest::Approx(8.5f).epsilon(kTraceTol));
    CHECK(hit->hit.t == doctest::Approx(simd_length(hit->hit.point - origin)).epsilon(kTraceTol));
    check_float3_traced(hit->hit.point, origin + hit->hit.t * dir);

    SUBCASE("removing the nearer node promotes the farther one, still with a true t") {
        doc.remove_node(1);
        const auto far_hit = raycast_scene(doc, Ray{origin, dir});
        REQUIRE(far_hit.has_value());
        CHECK(far_hit->node_id == 2);
        CHECK(far_hit->hit.t == doctest::Approx(14.0f).epsilon(kTraceTol));
        CHECK(far_hit->hit.t == doctest::Approx(simd_length(far_hit->hit.point - origin)).epsilon(kTraceTol));
        check_float3_traced(far_hit->hit.point, origin + far_hit->hit.t * dir);
    }
}

TEST_CASE("raycast_scene: an origin inside a node returns its exit face, in front of the ray") {
    // Newly reachable: dolly translates the whole rig along the eye->focus ray,
    // so the eye can end up inside geometry. Whatever comes back must stay in
    // FRONT of the camera -- a focus point behind the eye would invert the next
    // orbit rather than merely misplace it.
    SceneDocument doc;
    Node node;
    node.id = 5;
    node.shape = Shape::Sphere;
    node.scale = {4.0f, 4.0f, 4.0f}; // world radius 2
    doc.add(node);

    const simd_float3 origin = {0.0f, 0.0f, 0.0f}; // dead centre, inside
    const auto hit = raycast_scene(doc, Ray{origin, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 5);
    CHECK(hit->hit.t > 0.0f);
    CHECK(hit->hit.t == doctest::Approx(2.0f).epsilon(kTraceTol));
    check_float3_traced(hit->hit.point, simd_float3{0.0f, 0.0f, -2.0f});
}

TEST_CASE("raycast_scene round-trips against the camera: ray -> hit -> project returns the pixel") {
    // Cross-validation with no hand-computed literals: whatever
    // ray_through_view_point aims at, projecting the resulting hit must land
    // back on the pixel it was fired through. Catches sign, aspect and y-flip
    // errors that the per-function tests agree on individually -- and it is
    // precisely the property auto-pivot rests on, since a pivot that isn't
    // under the cursor makes "point at the feature and drag" silently wrong.
    Camera camera;
    camera.eye = {4.0f, 3.0f, 6.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fov_y_radians = 1.0472f;
    camera.aspect = 1.6f;
    constexpr float w = 800.0f, h = 500.0f;

    SceneDocument doc;
    Node node;
    node.id = 11;
    node.shape = Shape::Sphere;
    node.position = camera.target;
    node.scale = {3.0f, 3.0f, 3.0f}; // world radius 1.5 -- covers the sampled pixels
    doc.add(node);

    for (const float x : {350.0f, 400.0f, 450.0f}) {
        for (const float y : {200.0f, 250.0f, 300.0f}) {
            CAPTURE(x);
            CAPTURE(y);
            const Ray ray = camera.ray_through_view_point(x, y, w, h);
            const auto hit = raycast_scene(doc, ray);
            REQUIRE(hit.has_value());

            const ViewPoint vp = camera.project(hit->hit.point, w, h);
            CHECK(vp.visible);
            CHECK(vp.x == doctest::Approx(x).epsilon(1e-3));
            CHECK(vp.y == doctest::Approx(y).epsilon(1e-3));
        }
    }
}

TEST_CASE("raycast_scene: both ends of the scale clamp hit finitely with unit normals") {
    // Editor's scale gesture clamps each component to [0.05, 50]. Those bounds
    // are what simd_inverse actually has to invert here, and a degenerate
    // inverse would poison the focus point with NaN -- which, since the pivot
    // is derived from it, breaks the camera unrecoverably rather than merely
    // failing a pick. resolve_focus rejects non-finite candidates, but the
    // cheapest place to know they cannot arise is here.
    struct Case {
        const char* label;
        simd_float3 scale;
        float origin_z;
        float expect_t;
    };
    const std::array<Case, 3> cases = {{
        {"min uniform", {0.05f, 0.05f, 0.05f}, 5.0f, 4.975f},          // world radius 0.025
        {"max uniform", {50.0f, 50.0f, 50.0f}, 50.0f, 25.0f},          // world radius 25
        {"extreme non-uniform", {0.05f, 50.0f, 0.05f}, 5.0f, 4.975f},  // needle along +y
    }};

    for (const Case& c : cases) {
        INFO("case: " << c.label); // CAPTURE on a const char* logs the pointer, not the text
        SceneDocument doc;
        Node node;
        node.id = 1;
        node.shape = Shape::Sphere;
        node.scale = c.scale;
        doc.add(node);

        const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, c.origin_z}, {0.0f, 0.0f, -1.0f}});
        REQUIRE(hit.has_value());
        CHECK(std::isfinite(hit->hit.t));
        CHECK(hit->hit.t == doctest::Approx(c.expect_t).epsilon(kTraceTol));
        CHECK(std::isfinite(hit->hit.point.x));
        CHECK(std::isfinite(hit->hit.point.y));
        CHECK(std::isfinite(hit->hit.point.z));
        CHECK(simd_length(hit->hit.normal) == doctest::Approx(1.0f));
    }
}

TEST_CASE("raycast_scene: a rotated node is picked in its own frame") {
    // Rotation was unreachable before it became renderable, so this pins
    // behaviour raycast_scene already had rather than behaviour it gained: it
    // inverts the full world_from_local (position, rotation AND scale), so the
    // only thing that had to be true was that nothing else assumed identity.
    //
    // A 1 x 1 x 3 box (half = 0.5, 0.5, 1.5) turned 45 deg about +Y, hit by a
    // ray straight down -z through x = 0. A CUBE would be the wrong subject
    // twice over: it is symmetric under this rotation at 90 deg, and at 45 deg
    // the ray lands exactly on an edge where the slab test's normal is
    // ambiguous. The elongated box puts the same ray cleanly on the local -X
    // face instead.
    //
    // Slab test in local space (numpy, Rodrigues inverse-rotating the ray):
    //   t = 4.292893219, local hit (-0.5, 0, 0.5), local normal (-1, 0, 0)
    //   -> world hit (0, 0, 0.70710678), world normal (-0.70710678, 0, 0.70710678)
    SceneDocument doc;
    Node node;
    node.id = 1;
    node.shape = Shape::Cube;
    node.scale = {1.0f, 1.0f, 3.0f};
    node.rotation = simd_quaternion(static_cast<float>(M_PI_4), simd_float3{0.0f, 1.0f, 0.0f});
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->hit.t == doctest::Approx(4.292893219f).epsilon(kTraceTol));
    check_float3_traced(hit->hit.point, simd_float3{0.0f, 0.0f, 0.70710678f});
    check_float3_traced(hit->hit.normal, simd_float3{-0.70710678f, 0.0f, 0.70710678f});
    // Unrotated, the same ray would hit the long +z face at z = 1.5 with normal
    // (0, 0, 1) -- so both assertions above are discriminating, not incidental.
    CHECK(hit->hit.t != doctest::Approx(3.5f));
}

TEST_CASE("raycast_scene hits lie on the SDF's zero set, for rotated and non-uniformly "
          "scaled nodes alike") {
    // The cross-validation between the two descriptions of a node's surface --
    // and since the analytic intersectors were deleted, the ONLY one left, which
    // is why it now runs across every shape rather than a representative two.
    //
    // Picking transforms the ray into the node's rigid frame and marches; the
    // SDF sampler translates, rotates and measures at the node's true
    // half-extents. They agree only if scale stays baked into the shape rather
    // than folded into the transform -- so this is still the assertion that
    // fails if anyone "simplifies" sdf_eval_node by dividing q by half_extents,
    // which would warp the field and desync the two paths.
    //
    // No hand-derived literals: the claim is agreement, not any particular
    // number. Every case carries a rotation AND a non-uniform scale, so the
    // cross-section contraction is exercised on each shape rather than only in
    // the isotropic case where it is the identity.
    struct Case { const char* label; Shape shape; simd_float3 scale; float param; };
    const std::array<Case, 12> cases = {{
        {"cube, uniform", Shape::Cube, {1.0f, 1.0f, 1.0f}, 0.0f},
        {"cube, non-uniform", Shape::Cube, {2.0f, 0.5f, 3.0f}, 0.0f},
        {"ellipsoid, uniform", Shape::Sphere, {1.5f, 1.5f, 1.5f}, 0.0f},
        {"ellipsoid, non-uniform", Shape::Sphere, {1.0f, 3.0f, 2.0f}, 0.0f},
        {"cone, sharp", Shape::Cone, {1.5f, 3.0f, 1.0f}, 0.0f},
        {"cone, truncated", Shape::Cone, {2.0f, 1.5f, 2.0f}, 0.45f},
        {"capsule, round", Shape::Capsule, {1.0f, 3.5f, 1.5f}, 1.0f},
        {"capsule, flat", Shape::Capsule, {2.0f, 1.0f, 1.0f}, 0.0f},
        {"octahedron", Shape::Octahedron, {2.0f, 1.0f, 3.0f}, 0.0f},
        {"pyramid, frustum", Shape::Pyramid, {2.5f, 2.0f, 1.0f}, 0.35f},
        {"prism, 5 sides", Shape::Prism, {1.5f, 2.0f, 2.5f}, 5.0f},
        {"vesica", Shape::Vesica, {1.0f, 2.5f, 1.5f}, 0.0f},
    }};

    for (const Case& c : cases) {
        INFO("case: " << std::string(c.label));
        SceneDocument doc;
        Node node;
        node.id = 1;
        node.shape = c.shape;
        node.shape_param = c.param;
        node.position = {0.25f, -0.5f, 0.75f};
        node.scale = c.scale;
        node.rotation = simd_quaternion(0.9f, simd_normalize(simd_float3{1.0f, 2.0f, -1.0f}));
        doc.add(node);

        // A spread of oblique rays, so no single lucky alignment can carry the
        // case and each one meets the surface at a different orientation.
        const std::array<simd_float3, 4> eyes = {{
            {0.0f, 0.0f, 8.0f}, {7.0f, 2.0f, 3.0f}, {-4.0f, -5.0f, 2.0f}, {1.0f, 9.0f, -2.0f},
        }};
        for (const simd_float3& eye : eyes) {
            INFO("eye: (" << eye.x << ", " << eye.y << ", " << eye.z << ")");
            const simd_float3 dir = simd_normalize(node.position - eye);
            const auto hit = raycast_scene(doc, Ray{eye, dir});
            REQUIRE(hit.has_value());
            const auto d = evaluate_scene_sdf(doc, hit->hit.point);
            REQUIRE(d.has_value());
            // Scaled by the distance travelled, because the trace's hit epsilon
            // is (see picking.cpp). A fixed absolute bound was the right shape
            // of assertion only while the epsilon was absolute -- and that
            // epsilon was the bug: on a contracted field it made the trace
            // crawl and miss interior rays entirely. The 3x is margin over the
            // epsilon itself; at these eye distances it is ~1.2e-3 world units,
            // still three orders tighter than any geometry being distinguished.
            CHECK(std::fabs(*d) < 3.0f * (1e-5f + 5e-5f * hit->hit.t));
        }
    }
}

// --- ray_plane -------------------------------------------------------------

TEST_CASE("ray_plane: literals from the task brief") {
    SUBCASE("head-on ray onto the z=0 plane hits at the origin") {
        const auto hit = ray_plane(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, simd_float3{0.0f, 0.0f, 0.0f},
                                    simd_float3{0.0f, 0.0f, 1.0f});
        REQUIRE(hit.has_value());
        check_float3_approx(*hit, simd_float3{0.0f, 0.0f, 0.0f});
    }

    SUBCASE("oblique ray onto the y=0 plane hits at the origin") {
        // denom = dot(normalize(0,-1,-1), (0,1,0)) = -1/sqrt(2).
        // t = dot((0,0,0)-(0,2,2), (0,1,0)) / denom = -2 / (-1/sqrt(2)) = 2*sqrt(2).
        // point = (0,2,2) + 2*sqrt(2) * (0,-1/sqrt(2),-1/sqrt(2)) = (0, 2-2, 2-2) = (0,0,0).
        const simd_float3 dir = simd_normalize(simd_float3{0.0f, -1.0f, -1.0f});
        const auto hit = ray_plane(Ray{{0.0f, 2.0f, 2.0f}, dir}, simd_float3{0.0f, 0.0f, 0.0f},
                                    simd_float3{0.0f, 1.0f, 0.0f});
        REQUIRE(hit.has_value());
        check_float3_approx(*hit, simd_float3{0.0f, 0.0f, 0.0f});
    }

    SUBCASE("ray parallel to the plane misses") {
        // dir.z=-1 is perpendicular to the plane normal (0,1,0): dot == 0, |0| < 1e-6.
        CHECK_FALSE(ray_plane(Ray{{0.0f, 1.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, simd_float3{0.0f, 0.0f, 0.0f},
                               simd_float3{0.0f, 1.0f, 0.0f})
                        .has_value());
    }

    SUBCASE("plane behind the ray origin misses") {
        // Ray at z=5 heading toward +z (away from the z=0 plane): t would be
        // negative, so the plane is behind the ray origin.
        CHECK_FALSE(ray_plane(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}}, simd_float3{0.0f, 0.0f, 0.0f},
                               simd_float3{0.0f, 0.0f, 1.0f})
                        .has_value());
    }
}

// drag_plane_for_node's two cases moved to gizmo_tests.cpp when the function
// was folded into gizmo_frame_for_node -- the behaviour they pinned (snapped ->
// the snap frame, free -> the node's own) is now a property of the frame, and
// the camera no longer takes part in either.

// --- Editor integration: scene built entirely through spawn() -------------

TEST_CASE("Editor: spawn/pick/select/nodeName integration, scene built entirely through spawn()") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    // Same camera Editor::create() wires up internally (core/src/editor.cpp):
    // eye {4,3,6}, target {0,0.5,0}, up {0,1,0}, fov_y 1.0472 rad, and aspect
    // 800/500=1.6 — matching the setViewportSize call above means this is
    // exactly the camera editor->spawn()/pick() use, which is the whole
    // point of this cross-validation. The scene starts empty (no hardcoded
    // nodes as of this milestone), so every id/name below comes from spawn().
    const simd_float3 eye = {4.0f, 3.0f, 6.0f};
    const simd_float3 target = {0.0f, 0.5f, 0.0f};
    // Independent re-derivation of ray_through_view_point's direction at the
    // exact viewport center (400,250 of 800x500): there ndc_x=ndc_y=0, so
    // the ray direction collapses to the look-at forward vector, matching
    // the same pattern camera_tests.cpp uses for its own center-ray check.
    const simd_float3 dir_center = simd_normalize(target - eye);

    SUBCASE("spawning on an empty scene misses everything and lands unsnapped at the fixed distance") {
        const SpawnResult cube = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
        CHECK(cube.node_id == 1); // first spawn into a fresh, empty scene
        CHECK(cube.snapped == false);
        CHECK(editor->selectedNode() == cube.node_id);

        char buf[64];
        editor->nodeName(cube.node_id, buf, sizeof(buf));
        CHECK(std::string(buf) == "Cube 1");

        // nodeName buffer-safety, exercised here since it needs a real node
        // (the hardcoded-scene node this used to check against is gone).
        char short_buf[3] = {'X', 'X', 'X'};
        editor->nodeName(cube.node_id, short_buf, 3);
        CHECK(short_buf[0] == 'C');
        CHECK(short_buf[1] == 'u');
        CHECK(short_buf[2] == '\0');

        // kUnsnappedSpawnDistance (core/src/scene.h) == 6.0f: an unsnapped
        // spawn's position is exactly eye + dir_center * that distance (no
        // surface offset, unlike the snapped case).
        const simd_float3 expected_position = eye + dir_center * kUnsnappedSpawnDistance;

        // Verify indirectly: pick() along the same ray must now hit the
        // newly-spawned cube. Kept robust rather than brittle — assert the
        // hit is finite, in front of the camera, and near the cube's known
        // center (within a unit cube's bounding-sphere radius,
        // sqrt(3)*0.5 ~= 0.866), rather than pinning an exact face point.
        const PickResult picked = editor->pick(400.0f, 250.0f);
        CHECK(picked.node_id == cube.node_id);

        const simd_float3 point = {picked.point.x, picked.point.y, picked.point.z};
        const simd_float3 normal = {picked.normal.x, picked.normal.y, picked.normal.z};
        CHECK(std::isfinite(point.x));
        CHECK(std::isfinite(point.y));
        CHECK(std::isfinite(point.z));
        CHECK(simd_length(normal) == doctest::Approx(1.0f));

        const float t = simd_dot(point - eye, dir_center); // in front of the camera along the ray
        CHECK(t > 0.0f);
        CHECK(simd_length(point - expected_position) < 0.87f);

        SUBCASE("spawning again at the same point snaps onto the cube just placed") {
            const SpawnResult sphere = editor->spawn(Shape::Sphere, Op::Subtract, 400.0f, 250.0f);
            CHECK(sphere.node_id == 2);
            CHECK(sphere.snapped == true); // SpawnResult carries no parent id; snap_parent == cube's
                                            // id is covered at the SceneDocument level (scene_tests.cpp)
            CHECK(editor->selectedNode() == sphere.node_id); // selection moved off the cube

            editor->nodeName(sphere.node_id, buf, sizeof(buf));
            CHECK(std::string(buf) == "Sphere 1");
        }
    }

    SUBCASE("pick at the far corner misses everything") {
        const PickResult result = editor->pick(5.0f, 5.0f);
        CHECK(result.node_id == kInvalidNode);
    }

    SUBCASE("select/selectedNode round-trips, including clearing with kInvalidNode") {
        CHECK(editor->selectedNode() == kInvalidNode);
        const SpawnResult cube = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
        CHECK(editor->selectedNode() == cube.node_id); // spawn() selects the new node
        editor->select(kInvalidNode);
        CHECK(editor->selectedNode() == kInvalidNode);
    }

    SUBCASE("nodeName returns an empty string for an unknown id") {
        char buf[64];
        editor->nodeName(99, buf, sizeof(buf));
        CHECK(std::string(buf) == "");
    }
}

// --- anisotropic shapes, which the unit-node cases above never reach --------
//
// Every ray test above uses unit_node: scale (1,1,1). That is one aspect ratio
// of each shape, and it leaves the whole cross-section-contraction path -- the
// machinery that exists precisely FOR non-cubic boxes -- untested. These sweep
// flattened shapes instead.

TEST_CASE("raycast_node: no interior ray misses a flattened shape") {
    // The claim is narrow and total: if a ray's closest approach is INSIDE the
    // solid, the trace must find the surface. Anything else is a click that
    // selects nothing, or selects whatever is behind.
    struct Case { Shape shape; simd_float3 scale; const char* name; };
    const std::array<Case, 3> cases = {{
        {Shape::Sphere, {1.0f, 0.1f, 1.0f}, "sphere, aspect 10"},
        {Shape::Sphere, {2.0f, 0.1f, 2.0f}, "sphere, aspect 20"},
        {Shape::Vesica, {2.0f, 0.3f, 2.0f}, "vesica, flattened"},
    }};

    for (const Case& c : cases) {
        CAPTURE(c.name);
        Node node = unit_node(c.shape);
        node.scale = c.scale;
        const SdfNode sn = local_sdf_node(node);

        int interior = 0, missed = 0;
        for (int i = -80; i <= 80; ++i) {
            for (int j = -80; j <= 80; ++j) {
                const float x = i * 0.025f;
                const float y = j * (c.scale.y / 160.0f);
                // Only rays that genuinely pass through the solid: the ray runs
                // down -z, so its closest approach is (x, y, 0).
                if (sdf_eval_node(sn, simd_float3{x, y, 0.0f}) >= 0.0f) continue;
                ++interior;
                if (!raycast_node(node, Ray{{x, y, 10.0f}, {0.0f, 0.0f, -1.0f}})) {
                    ++missed;
                }
            }
        }
        CAPTURE(interior);
        CAPTURE(missed);
        CHECK(interior > 1000); // the sweep really did cover the shape
        CHECK(missed == 0);
    }
}
