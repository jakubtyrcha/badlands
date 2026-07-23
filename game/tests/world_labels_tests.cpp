// CPU-math oracle for the floating world-label system (src/game/visual/
// world_labels): screen projection + dampened depth scale + the generic
// timed-label pool (timer -> opacity/rise, vertical stacking, live-follow vs
// frozen anchor). Pure glm math, no GPU -- mirrors the decal-math test pattern.

#include "game/visual/world_labels.hpp"

#include "engine/core/camera.hpp"

#include <catch_amalgamated.hpp>

#include <optional>

using namespace badlands;

namespace {

// A camera 10 units up the +Z axis looking at the origin down -Z.
glm::mat4 test_view_proj(float aspect = 1.0f) {
    Camera cam;
    cam.position = {0.0f, 0.0f, 10.0f};
    cam.direction = {0.0f, 0.0f, -1.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.fov = 45.0f;
    cam.aspect = aspect;
    cam.near_plane = 0.1f;
    cam.far_plane = 100.0f;
    return cam.GetProj() * cam.GetView();
}

}  // namespace

TEST_CASE("ProjectLabel centers a point in front of the camera") {
    const glm::mat4 vp = test_view_proj();
    LabelProjection p = ProjectLabel(vp, {0.0f, 0.0f, 0.0f}, 800.0f, 600.0f);

    REQUIRE(p.visible);
    CHECK(p.x == Catch::Approx(400.0f).margin(1.0f));
    CHECK(p.y == Catch::Approx(300.0f).margin(1.0f));
    CHECK(p.depth == Catch::Approx(10.0f).margin(0.01f));  // 10 units in front
}

TEST_CASE("ProjectLabel culls a point behind the camera") {
    const glm::mat4 vp = test_view_proj();
    // The camera sits at z=10 looking toward -Z; z=20 is behind it.
    LabelProjection p = ProjectLabel(vp, {0.0f, 0.0f, 20.0f}, 800.0f, 600.0f);
    CHECK_FALSE(p.visible);
}

TEST_CASE("ProjectLabel culls a point off the side of the screen") {
    const glm::mat4 vp = test_view_proj();
    // Far to the right of the frustum but still in front.
    LabelProjection p = ProjectLabel(vp, {1000.0f, 0.0f, 0.0f}, 800.0f, 600.0f);
    CHECK_FALSE(p.visible);
}

TEST_CASE("LabelDepthScale is 1 at the reference depth and dampened with distance") {
    using namespace label_scale;
    CHECK(LabelDepthScale(kRefDepth) == Catch::Approx(1.0f));

    // Monotonic: closer is bigger, farther is smaller (within the clamp band).
    CHECK(LabelDepthScale(kRefDepth * 0.75f) > LabelDepthScale(kRefDepth));
    CHECK(LabelDepthScale(kRefDepth * 1.5f) < LabelDepthScale(kRefDepth));

    // Dampened: doubling the depth from the reference shrinks the label by LESS
    // than a true 1/depth perspective would (which would be exactly 0.5x).
    const float far_scale = LabelDepthScale(kRefDepth * 2.0f);
    CHECK(far_scale > 0.5f);
    CHECK(far_scale < 1.0f);

    // Clamped both ways.
    CHECK(LabelDepthScale(0.001f) == Catch::Approx(kMaxScale));
    CHECK(LabelDepthScale(100000.0f) == Catch::Approx(kMinScale));
}

TEST_CASE("SampleLabelAnimation fades alpha out and rises over the label's life") {
    // t01 runs 1 (spawn) -> 0 (expiry).
    LabelVisual young = SampleLabelAnimation(LabelAnimation::RiseFade, 1.0f);
    LabelVisual old = SampleLabelAnimation(LabelAnimation::RiseFade, 0.0f);

    CHECK(young.opacity == Catch::Approx(1.0f));  // fully visible at spawn
    CHECK(old.opacity == Catch::Approx(0.0f));    // faded out at expiry

    CHECK(young.rise == Catch::Approx(0.0f));  // no rise yet at spawn
    CHECK(old.rise > young.rise);              // has risen by expiry
}

TEST_CASE("WorldLabelPool advances timers and drops expired labels") {
    WorldLabelPool pool;
    pool.Spawn(0, {0, 0, 0}, 2.0f, "8", 0xffffffffu, 1.0f, LabelAnimation::RiseFade);
    REQUIRE(pool.size() == 1);

    pool.Advance(0.5f);
    CHECK(pool.size() == 1);  // still alive

    pool.Advance(0.6f);  // total 1.1s > 1.0s lifetime
    CHECK(pool.empty());
}

TEST_CASE("WorldLabelPool stacks multiple labels on one anchor vertically") {
    WorldLabelPool pool;
    const auto anim = LabelAnimation::RiseFade;
    pool.Spawn(7, {0, 0, 0}, 2.0f, "4", 0xffffffffu, 2.0f, anim);
    pool.Spawn(7, {0, 0, 0}, 2.0f, "6", 0xffffffffu, 2.0f, anim);

    // Anchor is alive at the same position; both labels resolve.
    auto live = [](uint32_t) -> std::optional<glm::vec3> {
        return glm::vec3{0, 0, 0};
    };
    std::vector<ResolvedLabel> out = pool.Resolve(live);
    REQUIRE(out.size() == 2);
    // Two co-located same-anchor labels must not overlap: different heights.
    CHECK(out[0].world_pos.y != Catch::Approx(out[1].world_pos.y));
}

TEST_CASE("WorldLabelPool stack ranks put newer same-anchor labels lowest") {
    // Guards the O(n) rank refactor: three labels on one anchor + one on another;
    // within the shared anchor, the newest (last spawned) sits lowest.
    WorldLabelPool pool;
    const auto anim = LabelAnimation::RiseFade;
    pool.Spawn(7, {0, 0, 0}, 0.0f, "a", 0xffffffffu, 2.0f, anim);  // oldest
    pool.Spawn(9, {0, 0, 0}, 0.0f, "x", 0xffffffffu, 2.0f, anim);  // other anchor
    pool.Spawn(7, {0, 0, 0}, 0.0f, "b", 0xffffffffu, 2.0f, anim);
    pool.Spawn(7, {0, 0, 0}, 0.0f, "c", 0xffffffffu, 2.0f, anim);  // newest on 7

    auto live = [](uint32_t) -> std::optional<glm::vec3> { return glm::vec3{0, 0, 0}; };
    std::vector<ResolvedLabel> out = pool.Resolve(live);
    REQUIRE(out.size() == 4);
    // out[0]=a (rank 2, highest), out[2]=b (rank 1), out[3]=c (rank 0, lowest).
    CHECK(out[0].world_pos.y > out[2].world_pos.y);  // older a above newer b
    CHECK(out[2].world_pos.y > out[3].world_pos.y);  // b above newest c
    // The other-anchor label (out[1]) is rank 0 on its own anchor (base height).
    CHECK(out[1].world_pos.y == Catch::Approx(out[3].world_pos.y));
}

TEST_CASE("WorldLabelPool follows a live anchor and freezes on a dead one") {
    WorldLabelPool pool;
    pool.Spawn(3, {0, 0, 0}, 2.0f, "9", 0xffffffffu, 2.0f, LabelAnimation::RiseFade);

    // Live: the entity moved to x=10 -> the label follows it.
    auto moved = [](uint32_t) -> std::optional<glm::vec3> {
        return glm::vec3{10, 0, 0};
    };
    CHECK(pool.Resolve(moved)[0].world_pos.x == Catch::Approx(10.0f));

    // Dead: no live position -> the label stays at its spawn x=0.
    auto gone = [](uint32_t) -> std::optional<glm::vec3> { return std::nullopt; };
    CHECK(pool.Resolve(gone)[0].world_pos.x == Catch::Approx(0.0f));
}
