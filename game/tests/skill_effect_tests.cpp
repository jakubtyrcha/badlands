// Skill EFFECTS in isolation: no world, no Sim, no registry -- exactly the
// isolation the contract promises (game/src/skill_abi.h). A test builds a
// BlSkillCastContext by hand, calls the effect, and inspects the op batch. If
// one of these ever needs a BadlandsGame to run, the contract has been broken.

#include "skill_abi.h"
#include "skills.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstring>

using badlands::SkillEffectOf;
using badlands::SkillId;
using badlands::StatusKind;

namespace {

BlSkillCastContext bash_context(int32_t test_outcome, float test_damage = 6.0f) {
    BlSkillCastContext ctx{};
    ctx.version = BL_SKILL_ABI_VERSION;
    ctx.skill_id = static_cast<int32_t>(SkillId::ShieldBash);
    ctx.world_millis = 12345;
    ctx.caster.slot = 1u;
    ctx.caster.accuracy = 1.0f;
    ctx.caster.melee_range = 1.5f;
    ctx.target_count = 1;
    ctx.targets[0].slot = 4u;
    ctx.targets[0].dist = 1.0f;
    ctx.targets[0].attack_test = test_outcome;
    ctx.targets[0].test_damage = test_outcome == BL_TEST_HIT ? test_damage : 0.0f;
    ctx.targets[0].relation = BL_REL_ENEMY;
    std::snprintf(ctx.constants[0].name, BL_SKILL_NAME_LEN, "stun_seconds");
    ctx.constants[0].value = 3.0f;
    ctx.constant_count = 1;
    return ctx;
}

}  // namespace

TEST_CASE("shield-bash stuns a target its test landed on", "[skill_effect]") {
    const BlSkillCastContext ctx = bash_context(BL_TEST_HIT);
    BlSkillEffectBatch out{};
    SkillEffectOf(SkillId::ShieldBash)(ctx, out);

    REQUIRE(out.count == 1);
    CHECK(out.ops[0].kind == BL_FX_APPLY_STATUS);
    CHECK(out.ops[0].target_slot == 4u);
    CHECK(out.ops[0].param_i == static_cast<int32_t>(StatusKind::Stunned));
    CHECK(out.ops[0].param_f == Catch::Approx(3000.0f));  // seconds -> ms
}

TEST_CASE("a blocked or dodged bash produces no ops at all", "[skill_effect]") {
    for (int32_t outcome : {BL_TEST_BLOCKED, BL_TEST_DODGED, BL_TEST_NOT_RUN}) {
        const BlSkillCastContext ctx = bash_context(outcome);
        BlSkillEffectBatch out{};
        SkillEffectOf(SkillId::ShieldBash)(ctx, out);
        CHECK(out.count == 0);
    }
}

TEST_CASE("shield-bash never asks for damage, however hard it hit",
          "[skill_effect]") {
    // Pure control by design: the engine rolled a real blow and the effect
    // deliberately discards it, asking only for the stun.
    const BlSkillCastContext ctx = bash_context(BL_TEST_HIT, /*test_damage=*/99.0f);
    BlSkillEffectBatch out{};
    SkillEffectOf(SkillId::ShieldBash)(ctx, out);

    REQUIRE(out.count == 1);
    CHECK(out.ops[0].kind == BL_FX_APPLY_STATUS);
}

TEST_CASE("a missing constant falls back to zero rather than stunning forever",
          "[skill_effect]") {
    BlSkillCastContext ctx = bash_context(BL_TEST_HIT);
    ctx.constant_count = 0;  // manifest authored no stun_seconds
    BlSkillEffectBatch out{};
    SkillEffectOf(SkillId::ShieldBash)(ctx, out);

    REQUIRE(out.count == 1);
    CHECK(out.ops[0].param_f == Catch::Approx(0.0f));
    // apply_status treats a non-positive duration as a no-op (status.h), so a
    // missing constant costs the cast rather than freezing the target for good.
}

TEST_CASE("every target that was hit gets its own op", "[skill_effect]") {
    BlSkillCastContext ctx = bash_context(BL_TEST_HIT);
    ctx.target_count = 3;
    ctx.targets[1] = ctx.targets[0];
    ctx.targets[1].slot = 5u;
    ctx.targets[1].attack_test = BL_TEST_DODGED;
    ctx.targets[2] = ctx.targets[0];
    ctx.targets[2].slot = 6u;

    BlSkillEffectBatch out{};
    SkillEffectOf(SkillId::ShieldBash)(ctx, out);
    REQUIRE(out.count == 2);  // the dodger is skipped
    CHECK(out.ops[0].target_slot == 4u);
    CHECK(out.ops[1].target_slot == 6u);
}

TEST_CASE("skill_constant reads by name and honours its fallback",
          "[skill_effect]") {
    const BlSkillCastContext ctx = bash_context(BL_TEST_HIT);
    CHECK(badlands::skill_constant(ctx, "stun_seconds", -1.0f) == Catch::Approx(3.0f));
    CHECK(badlands::skill_constant(ctx, "absent", -1.0f) == Catch::Approx(-1.0f));
    CHECK(badlands::skill_constant(ctx, "absent") == Catch::Approx(0.0f));
}

TEST_CASE("push_effect_op drops past capacity instead of overrunning",
          "[skill_effect]") {
    BlSkillEffectBatch out{};
    for (int i = 0; i < BL_SKILL_MAX_OPS + 3; ++i) {
        badlands::push_effect_op(out, BlSkillEffectOp{BL_FX_APPLY_STATUS,
                                                      static_cast<uint32_t>(i), 1, 100.0f});
    }
    CHECK(out.count == BL_SKILL_MAX_OPS);
    CHECK(out.ops[BL_SKILL_MAX_OPS - 1].target_slot ==
          static_cast<uint32_t>(BL_SKILL_MAX_OPS - 1));
}

TEST_CASE("every skill has an effect, and Calcify's is a documented no-op",
          "[skill_effect]") {
    for (int32_t i = 0; i < badlands::kSkillCount; ++i) {
        CHECK(SkillEffectOf(static_cast<SkillId>(i)) != nullptr);
    }
    const BlSkillCastContext ctx = bash_context(BL_TEST_HIT);
    BlSkillEffectBatch out{};
    SkillEffectOf(SkillId::Calcify)(ctx, out);
    CHECK(out.count == 0);  // the ward's own mechanic is a later slice
}

TEST_CASE("the contract's layout is pinned", "[skill_effect]") {
    CHECK(sizeof(BlSkillCaster) == 32);
    CHECK(sizeof(BlSkillTarget) == 48);
    CHECK(sizeof(BlSkillConstant) == 32);
    CHECK(sizeof(BlSkillCastContext) == 712);
    CHECK(sizeof(BlSkillEffectOp) == 16);
    CHECK(sizeof(BlSkillEffectBatch) == 136);
    CHECK(BL_SKILL_MAX_TARGETS == badlands::kMaxSkillTargets);
    CHECK(BL_SKILL_MAX_CONSTANTS == badlands::kMaxSkillConstants);
}
