// Wasm hero brain wire v2 (the intention contract, docs/design/
// intention-contract.html): a brain suggests ONE intention per wake, the
// engine (game/src/intention.h) validates/executes/tracks it and decides
// when to wake the brain again. This file exercises the WASM PLUMBING --
// pack_view_wire (statuses/events/self land in the wire correctly),
// decode_suggestion (the wire trust boundary: each BL_INT_* maps, malformed
// wires are rejected, BL_INT_USE_SKILL warns+ignores), and tick_wasm_brain
// end to end through badlands::Sim/tick_world (spawn/tick, combat pre-empt,
// the wake gate, determinism). Decision CORRECTNESS is not a parity target
// (no C++ reference brain exists anymore -- "behavior-in-spirit" is the bar,
// see the design doc's §3); cases that need a brain pinned to a fixed
// decision load game/tests/fixtures/idle_brain.wasm
// (scripts/brains/nim/idle_test.nim) instead of the real, shipping
// assets/brains/hero.wasm.

#include "badlands_sim.hpp"
#include "command.h"
#include "components.h"
#include "combat.h"
#include "threat_table.h"
#include "duel_common.h"
#include "game_state.h"
#include "hero_perception.h"  // observe_hero/weights_for/WorldView/ActivityWeights
#include "intention.h"
#include "sim_internal.hpp"
#include "skills.h"
#include "status.h"
#include "wasm_brain.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace badlands;
using namespace testfix;

namespace {

// Reads a binary wasm fixture, repo-root-relative like every other asset
// path in this codebase (add_test sets WORKING_DIRECTORY to the repo root,
// see CMakeLists.txt).
std::vector<uint8_t> read_wasm_file(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.data()), size));
    return bytes;
}

// The shipping brain artifact (LFS binary; scripts/brains/nim/hero.nim) --
// the real, ported hero decision layer.
std::vector<uint8_t> read_hero_wasm() { return read_wasm_file("assets/brains/hero.wasm"); }

// Test-only fixture (LFS binary; scripts/brains/nim/idle_test.nim -- same
// export surface as hero.nim, but bl_tick always suggests BL_INT_IDLE/
// ActivityId::Idle, unconditionally): built by scripts/build_brains.sh
// alongside hero.wasm.
std::vector<uint8_t> read_idle_wasm() {
    return read_wasm_file("game/tests/fixtures/idle_brain.wasm");
}

BrainDesc wasm_desc(const std::vector<uint8_t>& bytes) {
    return BrainDesc{.wasm_bytes = bytes.data(), .wasm_len = bytes.size()};
}

// A minimal home-less hero (Archetype defaults to Hero), for the
// pack_view_wire/decode_suggestion fixtures -- only the components those
// functions actually read matter, so the rest of CharacterDesc is left at
// its zero default.
CharacterDesc bare_hero(float x, float z) {
    CharacterDesc d{};
    d.pos_x = x;
    d.pos_z = z;
    d.team = 0;
    d.hp = 10.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    return d;
}

// Places one guild + recruits `n` heroes through the command path, exactly
// as determinism_tests.cpp's seed_town does (duplicated per that file's own
// fixture-independence convention) -- so the seed itself is part of the
// recorded/replayed log.
void seed_heroes(BadlandsGame* g, int n) {
    Action place{ActionKind::PlaceBuilding, 0, -14.0f, -8.0f,
                static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    const int64_t guild = dispatch_into(*g, place);
    REQUIRE(guild >= 0);
    for (int i = 0; i < n; ++i) {
        Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(guild), 0.0f, 0.0f, 0, 0};
        REQUIRE(dispatch_into(*g, recruit) >= 0);
    }
}

bool same_command(const Command& a, const Command& b) {
    return a.kind == b.kind && a.actor == b.actor && a.target_id == b.target_id &&
          a.point == b.point && a.param_a == b.param_a && a.param_b == b.param_b &&
          a.at_millis == b.at_millis;
}

// A rat durable enough that a duel against it spans several attack-cooldown
// windows (the stock catalog rat, 6 hp, dies to a single mercenary swing --
// too short to pin "no double-swing" or "prefers the higher-damage ready
// attack" across more than one window). Spawned a couple of units off (not
// already in melee reach), so the FIRST combat wake -- while still
// approaching -- is a genuine no-op for hero.nim's own pick too (out of
// range), same as every wake after it until the merc closes the gap.
CharacterDesc durable_rat(float x, float z) {
    CharacterDesc d = DefaultCreatureCatalog().defs[static_cast<int>(CreatureId::Rat)];
    d.pos_x = x;
    d.pos_z = z;
    d.team = 1;
    d.hp = 500.0f;
    return d;
}

// A deer built with LOTS of hp (Finding A's own framing) -- durable enough
// that it survives many bow volleys inside this test's window, so "does the
// hunter keep firing" is what the test measures, never "did it just die
// after one shot" (which the pre-fix one-shot-at-adoption bug would also
// pass trivially against a normal 8-hp deer).
uint32_t spawn_durable_deer(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Critter;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 2;
    d.hp = 100000.0f;
    d.move_speed = 3.0f;
    d.size_x = d.size_y = d.size_z = 0.7f;
    return spawn_into(g, d);
}

}  // namespace

// --- wasm plumbing smokes (fixed-decision brains) ---------------------------

TEST_CASE("wasm: every hero stays Idle over 30 ticks with the idle fixture brain, no bugs") {
    std::vector<uint8_t> bytes = read_idle_wasm();
    Sim sim(wasm_desc(bytes));

    std::vector<uint32_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(sim.Spawn(mercenary(static_cast<float>(i) * 6.0f, kDuelGroundZ)));
    }
    for (int i = 0; i < 30; ++i) {
        sim.Tick(kTickDt);
    }

    auto rows = sim.Characters();
    REQUIRE(rows.size() == 3);
    for (const CharacterState& r : rows) {
        CHECK(r.behavior == static_cast<int32_t>(ActivityId::Idle));
    }

    // Decisions were actually applied: every adopted intention logs a
    // SetBehavior command (apply_intention, intention.cpp), so a nonempty
    // command log is the "a decision landed" signal.
    CHECK_FALSE(sim.CommandLog().empty());
}

TEST_CASE("wasm: single-gateway combat resolves a two-brained duel") {
    // V5 (single-gateway cutover): combat_preempt is deleted outright --
    // engagement and swings both come from a brain's Attack intention/
    // action on EACH side now: the wasm hero brain (hero.nim, restating
    // BL_INT_ATTACK + bl_enqueue_action) and the goblin's simple monster
    // brain (monster_brain.cpp, the SAME apply_intention/resolve_action
    // seams, engine-side). Both sides fight for real; the mercenary's
    // better stats (accuracy/armour/damage, creature_catalog.cpp) still
    // decide the duel, exactly as they did when combat_preempt drove the
    // goblin's half of it -- see wasm_brain_tests.cpp's other two cases
    // below for the wasm brain's own contribution to the log.
    std::vector<uint8_t> bytes = read_hero_wasm();
    Sim sim(wasm_desc(bytes));

    CharacterDesc merc = mercenary(-8.0f, kDuelGroundZ);
    CharacterDesc gob = goblin(8.0f, kDuelGroundZ);
    uint32_t merc_id = sim.Spawn(merc);
    sim.Spawn(gob);

    CharacterState survivor = run_duel(sim);

    CHECK(survivor.id == merc_id);
    CHECK(survivor.team == 0);
    CHECK(survivor.hp < survivor.max_hp);  // the goblin got its licks in
}

TEST_CASE("wasm: hero vs rat -- the brain's own swings land, no double-swing per tick") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    Sim sim(wasm_desc(bytes));

    CharacterDesc merc = mercenary(-8.0f, kDuelGroundZ);
    uint32_t merc_id = sim.Spawn(merc);
    sim.Spawn(durable_rat(-6.0f, kDuelGroundZ));

    auto count_merc_set_behavior = [&]() {
        int32_t n = 0;
        for (const CommandRecord& c : sim.CommandLog()) {
            if (c.kind == CommandKindId::SetBehavior && c.actor == merc_id) {
                ++n;
            }
        }
        return n;
    };

    // Warm up to combat steady state first (gap closed, Attack intention
    // adopted, several swings already landed) before taking the flat-log
    // checkpoint below -- a checkpoint taken mid-adoption would still
    // legitimately see growth, and that is not what this test is about.
    constexpr int kWarmupTicks = 100;  // ~3.3 sim-seconds: plenty to close the 2-unit gap and swing
    for (int i = 0; i < kWarmupTicks; ++i) {
        sim.Tick(kTickDt);
    }
    const int32_t set_behavior_at_checkpoint = count_merc_set_behavior();
    REQUIRE(set_behavior_at_checkpoint >= 1);  // the initial Attack adoption landed

    for (int i = kWarmupTicks; i < 600; ++i) {  // 20 sim-seconds total: several cooldown windows
        sim.Tick(kTickDt);
    }

    // restate-log dedup (docs/design/intention-contract.html §2/§6, "Resume-
    // by-default"): should_wake's high-stakes clause re-consults this hero
    // EVERY tick for the rest of the duel (~500 more wakes -- a threat in
    // view / MeleeLock), and hero.nim restates the identical Attack/ActCombat
    // suggestion wake after wake -- an unchanged decision now logs NOTHING
    // (apply_intention's restate-resume path, intention.cpp, no longer
    // force-logs a SetBehavior on every restate; only the live
    // wake_at_millis refreshes, off-log). So the SetBehavior count for this
    // actor must be EXACTLY what it was at the warmup checkpoint, even after
    // ~500 more ticks of unchanged engagement -- the STRONG invariant this
    // test pins now: zero NEW commands of any kind from continued identical
    // restates, not merely "same activity_label throughout" (a design that
    // force-logged every restate would also have satisfied that weaker
    // claim). The Attack/Engage commands the fight itself legitimately keeps
    // producing below (swings, re-aiming at the live target) are untouched by
    // this assertion -- it is scoped to SetBehavior specifically, the one
    // command kind restate-log dedup changes.
    CHECK(count_merc_set_behavior() == set_behavior_at_checkpoint);

    std::vector<CommandRecord> merc_attacks;
    for (const CommandRecord& c : sim.CommandLog()) {
        if (c.kind == CommandKindId::Attack && c.actor == merc_id) {
            merc_attacks.push_back(c);
        }
    }
    // Several ready-windows' worth of swings, not just one -- otherwise the
    // "no double-swing" pin below would be checking an empty/trivial set.
    REQUIRE(merc_attacks.size() >= 4);

    // The brain's own picks (bl_enqueue_action -> resolve_action,
    // game/src/intention.h) carry an explicit attack index -- param_a >= 0 --
    // never the legacy auto-pick sentinel (-1). Single-gateway combat (V5):
    // apply_intention's Attack case never pushes an Attack command of its
    // own anymore (adoption/restatement is engagement-only) -- ZERO
    // param_a == -1 entries now, not just "at most one"; every attack in
    // the log is brain-picked.
    int brain_picked = 0;
    int legacy_auto_pick = 0;
    for (const CommandRecord& c : merc_attacks) {
        if (c.param_a >= 0) {
            ++brain_picked;
        } else {
            ++legacy_auto_pick;
        }
    }
    CHECK(brain_picked >= 4);
    CHECK(legacy_auto_pick == 0);

    // Never two Attack commands from the SAME actor landing in the SAME
    // tick: hero.nim's soft one-action convention (at most one
    // bl_enqueue_action per wake) plus resolve_action's own cooldown
    // validation at resolve time is what holds this, not a special guard --
    // there is no separate combat path left that could double up on it.
    std::vector<int64_t> at_millis_seen;
    for (const CommandRecord& c : merc_attacks) {
        at_millis_seen.push_back(c.at_millis);
    }
    std::sort(at_millis_seen.begin(), at_millis_seen.end());
    CHECK(std::adjacent_find(at_millis_seen.begin(), at_millis_seen.end()) ==
          at_millis_seen.end());
}

TEST_CASE("wasm: a two-attack hero's brain-picked swings prefer the higher-damage ready attack") {
    std::vector<uint8_t> bytes = read_hero_wasm();

    // Two melee attacks, same range, DIFFERENT base_damage -- pick_attack's own
    // legacy preference (combat.h: ranged-if-free, else first usable) would pick
    // index 0 here (neither is ranged); hero.nim's own rule (highest base_damage
    // among ready/legal/in-range) should consistently pick index 1 instead.
    //
    // Index 0's cooldown is deliberately NOT equal to index 1's (30s vs 1s), and
    // that asymmetry is load-bearing, not cosmetic. should_wake's v3 high-stakes
    // clause (intention.h) means tick_wasm_brain -- and so pickBestAttack --
    // runs again literally every tick of this duel: at the FIRST combat wake
    // both attacks are ready and index 1 (higher damage) correctly wins, but
    // that fires ONLY index 1's cooldown -- index 0, never yet used, is still
    // sitting fully ready. On the very NEXT wake index 1 is still recovering
    // and index 0 is the ONLY ready attack, so hero.nim, correctly following
    // its own per-wake-snapshot rule ("highest base_damage AMONG READY", not
    // "wait for the best overall"), picks it -- a real, unavoidable
    // consequence of a greedy per-wake picker under EQUAL cooldowns, not a
    // hero.nim preference bug. With equal 1s cooldowns that keeps recurring
    // (empirically: 20 index-1 picks vs. 19 index-0 picks over this test's 600
    // ticks, not the "always 1" a naive setup would promise) -- so index 0
    // carries a cooldown longer than the whole 20s test window instead,
    // ensuring its one forced use is never followed by a second ready window:
    // every brain-picked (param_a >= 0) swing after that first contested
    // decision is deterministically index 1, which is what "prefer the
    // higher-damage ready attack" is actually testing.
    // The scenario runs TWICE, with the damage assignment mirrored across the
    // two indices, and asserts the brain follows the DAMAGE both times: this
    // is what rules out "picks a fixed index that happens to be right" -- a
    // regression to index-order (or ignoring base_damage entirely) fails one
    // of the two mirrors loudly. In each mirror the low-damage attack carries
    // the 30s cooldown (the load-bearing asymmetry explained above) and the
    // preferred attack the 1s one, so every brain-picked swing is the
    // preference in action. The FIRST brain-picked swing is asserted
    // separately and is the genuinely contested decision: at the first combat
    // wake BOTH attacks are ready (nothing has fired yet), so that pick alone
    // proves the two-candidate comparison; the later picks pin that the
    // preference holds across every subsequent ready-window.
    auto run_mirror = [&](float dmg0, float cd0, float dmg1, float cd1,
                          int32_t preferred) {
        Sim mirror_sim(wasm_desc(bytes));
        CharacterDesc m = mercenary(-8.0f, kDuelGroundZ);
        m.attack_count = 2;
        m.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, dmg0, 2.0f, cd0, 0.1f};
        m.attacks[1] = {AttackCategory::Melee, DamageType::Slashing, dmg1, 2.0f, cd1, 0.1f};
        uint32_t id = mirror_sim.Spawn(m);
        mirror_sim.Spawn(durable_rat(-6.0f, kDuelGroundZ));
        for (int i = 0; i < 600; ++i) {  // 20 sim-seconds, inside the 30s cooldown
            mirror_sim.Tick(kTickDt);
        }
        int picked_preferred = 0;
        int picked_other = 0;
        int32_t first_pick = -1;
        for (const CommandRecord& c : mirror_sim.CommandLog()) {
            if (c.kind == CommandKindId::Attack && c.actor == id && c.param_a >= 0) {
                if (first_pick < 0) {
                    first_pick = c.param_a;
                }
                if (c.param_a == preferred) {
                    ++picked_preferred;
                } else {
                    ++picked_other;
                }
            }
        }
        // The contested decision: both attacks ready, higher damage must win.
        CHECK(first_pick == preferred);
        CHECK(picked_preferred >= 4);
        // Exactly ONE other-index pick, and it is CORRECT greedy behavior,
        // not slack in the assertion: on the wake right after the preferred
        // attack's first (contested) use, it is still on its 1s cooldown and
        // the low-damage attack -- never yet used -- is legitimately "the
        // best (only) ready attack" for that one wake, after which its own
        // 30s cooldown removes it for the rest of the run.
        // A real regression (hero.nim ignoring base_damage, or picking by
        // index position) fails first_pick above AND pushes this well past 1.
        CHECK(picked_other <= 1);
    };
    // Mirror A: index 1 carries the damage -> every brain pick is index 1.
    run_mirror(/*dmg0=*/4.0f, /*cd0=*/30.0f, /*dmg1=*/9.0f, /*cd1=*/1.0f, /*preferred=*/1);
    // Mirror B: damages swapped -> the SAME brain now picks index 0, proving
    // the choice follows base_damage, not index position.
    run_mirror(/*dmg0=*/9.0f, /*cd0=*/1.0f, /*dmg1=*/4.0f, /*cd1=*/30.0f, /*preferred=*/0);
}

TEST_CASE("wasm: a hunter keeps shooting a wounded deer that survives the first arrow "
         "(Shoot-restate livelock)") {
    // Finding A: the Attack command for a Shoot intention used to be pushed
    // ONLY in apply_intention's one-shot Shoot-adoption case (intention.cpp)
    // -- an identical restatement (same target_slot) resumes without
    // re-running that case, and advance_intentions only ends Shoot when the
    // target dies. A prey durable enough to survive the opening arrow was
    // therefore never shot again. Follows "a hunter runs down a deer and
    // kills it" (hunter_tests.cpp) for the world setup, but pins the deer
    // well inside bow range from the start (no chase needed) and with LOTS
    // of hp (spawn_durable_deer, above) so many volleys are needed, not one.
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    const glm::vec2 hunter_pos{0.0f, kCastleSpawnZ};
    const glm::vec2 prey_pin{4.0f, kCastleSpawnZ};  // inside the Hunter's 8-unit bow range
    uint32_t hunter = spawn_creature_into(*g, CreatureId::Hunter, 0, hunter_pos);
    uint32_t deer = spawn_durable_deer(*g, prey_pin);
    entt::entity de = g->slots[deer];

    for (int i = 0; i < 900; ++i) {  // 30 sim-seconds: many 1.2s bow-cooldown windows
        tick_world(*g, 1.0f / 30.0f);
        REQUIRE(g->registry.valid(de));  // durable -- never actually dies here
        g->registry.get<Position>(de).pos = prey_pin;  // pinned: never drifts out of range
    }

    int32_t shots_at_prey = 0;
    for (const Command& c : g->command_log) {
        if (c.kind == CommandKind::Attack && c.actor == hunter && c.target_id == deer) {
            ++shots_at_prey;
        }
    }
    CHECK(shots_at_prey >= 2);

    int32_t damage_events = 0;
    for (const GameEvent& ev : g->events) {
        if (ev.kind == GameEventKind::DamageDealt && ev.target_id == deer) {
            ++damage_events;
        }
    }
    CHECK(damage_events >= 2);  // the swings are real, not just logged
}

// --- pack_view_wire: the view side of the wire trust boundary --------------

TEST_CASE("pack_view_wire: v5 carries both sides of the fight-or-flee comparison",
          "[wasm_brain]") {
    // Threat's second role (game/src/threat_table.h): the brain gets its own
    // combat potential AND the hostile's, plus what that hostile can reach and
    // how fast it closes -- everything a standoff decision needs, and nothing
    // the engine decides on its brain's behalf.
    auto g = make_world(BrainDesc{});
    const uint32_t hero = spawn_creature_into(*g, CreatureId::Hunter, kPlayerTeam, {0.0f, 0.0f});
    const uint32_t foe = spawn_creature_into(*g, CreatureId::Bandit, /*team=*/1, {3.0f, 0.0f});
    entt::entity he = g->slots[hero];
    entt::entity fe = g->slots[foe];

    const ActivityWeights& weights = weights_for(*g, he);
    const WorldView view = observe_hero(*g, hero, he, weights);
    const BlViewWire wire = pack_view_wire(*g, he, view, weights);

    CHECK(wire.self.threat == Catch::Approx(threat_of(g->registry, he)));
    REQUIRE(wire.suggest.threat_count >= 1);
    const BlThreat& t = wire.suggest.threats[0];
    CHECK(t.slot == foe);
    CHECK(t.threat == Catch::Approx(threat_of(g->registry, fe)));
    CHECK(t.reach == Catch::Approx(melee_range(g->registry.get<Attacks>(fe))));
    CHECK(t.ranged_reach == Catch::Approx(ranged_range(g->registry.get<Attacks>(fe))));
    CHECK(t.move_speed == Catch::Approx(g->registry.get<Stats>(fe).move_speed));
    // The hunter outranges the bandit and the bandit cannot shoot back: this
    // is exactly the shape that makes the brain want a standoff.
    CHECK(t.ranged_reach == Catch::Approx(0.0f));
}

TEST_CASE("pack_view_wire: a disengaged hero carries BL_ST_DISENGAGED", "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    const uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];
    REQUIRE(apply_status(*g, e, StatusKind::Disengaged, 2500, slot));

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    bool found = false;
    for (int32_t i = 0; i < wire.status_count; ++i) {
        if (wire.statuses[i].kind == BL_ST_DISENGAGED) {
            found = true;
            CHECK(wire.statuses[i].remaining_millis == 2500);
        }
    }
    CHECK(found);
}

TEST_CASE("pack_view_wire: statuses assemble from Chatting/MeleeLock/InsideBuilding",
          "[wasm_brain]") {
    auto g = make_world(BrainDesc{});  // host-only: no wasm module needed

    SECTION("Chatting") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];
        g->registry.emplace<ChattingState>(e, /*partner_slot=*/7u, /*remaining=*/2.5f);

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        REQUIRE(wire.status_count == 1);
        CHECK(wire.statuses[0].kind == BL_ST_CHATTING);
        CHECK(wire.statuses[0].remaining_millis == 2500);
    }

    SECTION("MeleeLock") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];
        g->registry.emplace<MeleeLock>(e);

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        REQUIRE(wire.status_count == 1);
        CHECK(wire.statuses[0].kind == BL_ST_MELEE_LOCKED);
        CHECK(wire.statuses[0].remaining_millis == 0);  // indefinite
    }

    SECTION("InsideBuilding") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];
        g->registry.emplace<InsideBuilding>(e, /*building_id=*/0, /*purpose=*/0);

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        REQUIRE(wire.status_count == 1);
        CHECK(wire.statuses[0].kind == BL_ST_INSIDE_BUILDING);
        CHECK(wire.statuses[0].remaining_millis == 0);  // indefinite
    }

    SECTION("none of the three -> status_count 0") {
        uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
        entt::entity e = g->slots[slot];

        const ActivityWeights& weights = weights_for(*g, e);
        const WorldView view = observe_hero(*g, slot, e, weights);
        const BlViewWire wire = pack_view_wire(*g, e, view, weights);

        CHECK(wire.status_count == 0);
    }
}

TEST_CASE("pack_view_wire: the skills block is packed 1:1 with the Skills component",
          "[wasm_brain][skills]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];
    Skills& sk = g->registry.get<Skills>(e);
    sk = Skills{};
    learn_skill(sk, SkillId::ShieldBash);
    learn_skill(sk, SkillId::Calcify);
    sk.cooldown_remaining[1] = 4.0f;  // Calcify still cooling

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    REQUIRE(wire.skill_count == 2);
    // Index on the wire IS the index in Skills -- that is what a brain hands
    // back as BL_ACT_USE_SKILL's arg.
    CHECK(wire.skills[0].skill_id == static_cast<int32_t>(SkillId::ShieldBash));
    CHECK(wire.skills[0].cooldown_remaining == Catch::Approx(0.0f));
    CHECK(wire.skills[0].ready == 1u);
    CHECK(wire.skills[0].trigger == static_cast<int32_t>(SkillTrigger::Action));
    CHECK(wire.skills[0].target_mode == static_cast<int32_t>(SkillTargetMode::Any));
    CHECK(wire.skills[1].skill_id == static_cast<int32_t>(SkillId::Calcify));
    CHECK(wire.skills[1].cooldown_remaining == Catch::Approx(4.0f));
    CHECK(wire.skills[1].ready == 0u);
    CHECK(wire.skills[1].target_mode == static_cast<int32_t>(SkillTargetMode::SelfOnly));
}

TEST_CASE("pack_view_wire: a stun rides the statuses block with its remaining time",
          "[wasm_brain][skills]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];
    apply_status(*g, e, StatusKind::Stunned, 1500, 3u);

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    REQUIRE(wire.status_count == 1);
    CHECK(wire.statuses[0].kind == BL_ST_STUNNED);
    CHECK(wire.statuses[0].remaining_millis == 1500);
}

TEST_CASE("pack_view_wire: events copy 1:1 from the EventInbox", "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];

    InboxEvent ev;
    ev.kind = InboxEventKind::DamageTaken;
    ev.source_slot = 3;
    ev.param = 5.0f;
    push_inbox_event(*g, e, ev);  // stamps at_millis/ttl_millis from game state

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    REQUIRE(wire.event_count == 1);
    CHECK(wire.events[0].kind == BL_EV_DAMAGE_TAKEN);
    CHECK(wire.events[0].source_slot == 3);
    CHECK(wire.events[0].param == Catch::Approx(5.0f));
    CHECK(wire.events[0].at_millis == g->world_millis);
    CHECK(wire.events[0].ttl_millis == kInboxTtlMillis);
}

TEST_CASE("pack_view_wire: self carries the CurrentIntention summary", "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];

    CurrentIntention& ci = g->registry.get<CurrentIntention>(e);
    ci.kind = IntentionKind::Idle;
    ci.wake_at_millis = 4321;

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    CHECK(wire.self.intention_kind == BL_INT_IDLE);
    CHECK(wire.self.intention_wake_at == 4321);
}

// --- Cleanup: BlViewFactors.entrance_radius replaces blocks.nim's
// hand-copied kEntranceRadius constant -- the wire is now the single source
// of truth for it.

TEST_CASE("pack_view_wire: factors carries kEntranceRadius on the wire", "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];

    const ActivityWeights& weights = weights_for(*g, e);
    const WorldView view = observe_hero(*g, slot, e, weights);
    const BlViewWire wire = pack_view_wire(*g, e, view, weights);

    CHECK(wire.factors.entrance_radius == Catch::Approx(kEntranceRadius));
}

// --- decode_suggestion: the suggestion side of the wire trust boundary ------

TEST_CASE("decode_suggestion: each BL_INT_* kind maps onto the matching IntentionKind",
          "[wasm_brain]") {
    struct Case {
        int32_t wire_kind;
        IntentionKind expect;
    };
    const Case cases[] = {
        {BL_INT_NONE, IntentionKind::None},           {BL_INT_MOVE_TO, IntentionKind::MoveTo},
        {BL_INT_ATTACK, IntentionKind::Attack},        {BL_INT_SHOOT, IntentionKind::Shoot},
        {BL_INT_ENTER, IntentionKind::Enter},          {BL_INT_ENTER_HOME, IntentionKind::EnterHome},
        {BL_INT_BUY, IntentionKind::Buy},              {BL_INT_CHAT, IntentionKind::Chat},
        {BL_INT_IDLE, IntentionKind::Idle},
    };
    for (const Case& c : cases) {
        INFO("wire_kind " << c.wire_kind);
        BlSuggestionWire wire{};
        wire.intention_kind = c.wire_kind;
        wire.target_slot = 5;
        wire.arg = 2;
        wire.duration_millis = 100;
        wire.idle_hint_millis = 200;
        wire.point_x = 1.0f;
        wire.point_z = 2.0f;
        wire.activity_label = static_cast<int32_t>(ActivityId::Roam);

        const std::optional<Intention> intent = decode_suggestion(wire, /*slot=*/0);
        REQUIRE(intent.has_value());
        CHECK(intent->kind == c.expect);
        CHECK(intent->target_slot == 5);
        CHECK(intent->arg == 2);
        CHECK(intent->duration_millis == 100);
        CHECK(intent->idle_hint_millis == 200);
        CHECK(intent->point.x == Catch::Approx(1.0f));
        CHECK(intent->point.y == Catch::Approx(2.0f));
        CHECK(intent->activity_label == static_cast<int32_t>(ActivityId::Roam));
    }
}

TEST_CASE("decode_suggestion: BL_INT_USE_SKILL is reserved -- decodes to IntentionKind::None, "
         "not rejected",
          "[wasm_brain]") {
    BlSuggestionWire wire{};
    wire.intention_kind = BL_INT_USE_SKILL;
    const std::optional<Intention> intent = decode_suggestion(wire, 0);
    REQUIRE(intent.has_value());  // well-formed -- not the malformed/FATAL path
    CHECK(intent->kind == IntentionKind::None);
}

TEST_CASE("decode_suggestion: malformed wires are rejected", "[wasm_brain]") {
    // intention_kind out of range and activity_label out of range are NOT
    // here (Fix 5): they are unknown VOCABULARY, not malformed -- see the
    // "forward-compatible vocabulary decode" cases below.
    SECTION("non-finite point_x") {
        BlSuggestionWire wire{};
        wire.point_x = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("non-finite point_z") {
        BlSuggestionWire wire{};
        wire.point_z = std::numeric_limits<float>::infinity();
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("negative duration_millis") {
        BlSuggestionWire wire{};
        wire.duration_millis = -1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("duration_millis beyond INT32_MAX (would truncate through Command::param_b)") {
        BlSuggestionWire wire{};
        wire.intention_kind = BL_INT_IDLE;
        wire.duration_millis = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("negative idle_hint_millis") {
        BlSuggestionWire wire{};
        wire.idle_hint_millis = -1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
    SECTION("idle_hint_millis beyond INT32_MAX") {
        BlSuggestionWire wire{};
        wire.idle_hint_millis = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
        CHECK_FALSE(decode_suggestion(wire, 0).has_value());
    }
}

// --- Fix 5: forward-compatible vocabulary decode ----------------------------
// An intention_kind/activity_label this build doesn't recognize is UNKNOWN
// VOCABULARY (e.g. a newer guest talking to an older host), not corruption --
// decode as None/-1 with a warning, never reject. Only corruption-shaped
// wires (non-finite point floats, negative/overflowing duration/hint) stay
// FATAL, pinned by the "malformed wires are rejected" test above.

TEST_CASE("decode_suggestion: an unrecognized intention_kind decodes as None, not rejected",
          "[wasm_brain]") {
    BlSuggestionWire wire{};
    wire.intention_kind = 99;  // outside [BL_INT_NONE, BL_INT_USE_SKILL]
    wire.point_x = 1.0f;
    wire.point_z = 2.0f;
    wire.target_slot = 5;
    const std::optional<Intention> intent = decode_suggestion(wire, 0);
    REQUIRE(intent.has_value());  // well-formed -- not the malformed/FATAL path
    CHECK(intent->kind == IntentionKind::None);
    // The rest of the wire still decodes normally -- only the kind is unknown.
    CHECK(intent->point.x == Catch::Approx(1.0f));
    CHECK(intent->target_slot == 5);
}

TEST_CASE("decode_suggestion: a negative intention_kind decodes as None, not rejected",
          "[wasm_brain]") {
    BlSuggestionWire wire{};
    wire.intention_kind = -1;
    const std::optional<Intention> intent = decode_suggestion(wire, 0);
    REQUIRE(intent.has_value());
    CHECK(intent->kind == IntentionKind::None);
}

TEST_CASE("decode_suggestion: an out-of-range activity_label clamps to -1, not rejected",
          "[wasm_brain]") {
    SECTION("too high") {
        BlSuggestionWire wire{};
        wire.activity_label = kActivityCount;
        const std::optional<Intention> intent = decode_suggestion(wire, 0);
        REQUIRE(intent.has_value());
        CHECK(intent->activity_label == -1);
    }
    SECTION("below -1") {
        BlSuggestionWire wire{};
        wire.activity_label = -2;
        const std::optional<Intention> intent = decode_suggestion(wire, 0);
        REQUIRE(intent.has_value());
        CHECK(intent->activity_label == -1);
    }
    SECTION("-1 itself is the valid inspection-only sentinel, not clamped/warned") {
        BlSuggestionWire wire{};
        wire.activity_label = -1;
        const std::optional<Intention> intent = decode_suggestion(wire, 0);
        REQUIRE(intent.has_value());
        CHECK(intent->activity_label == -1);
    }
}

TEST_CASE("decode_suggestion + apply_intention: the idle hint plumbs through to "
         "CurrentIntention.wake_at_millis",
          "[wasm_brain]") {
    auto g = make_world(BrainDesc{});
    uint32_t slot = spawn_into(*g, bare_hero(0.0f, 0.0f));
    entt::entity e = g->slots[slot];
    g->world_millis = 1000;

    BlSuggestionWire wire{};
    wire.intention_kind = BL_INT_ATTACK;  // any non-Idle kind: idle_hint_millis (not
                                          // duration_millis) carries the schedule
    wire.idle_hint_millis = 750;

    const std::optional<Intention> intent = decode_suggestion(wire, slot);
    REQUIRE(intent.has_value());
    CHECK(apply_intention(*g, slot, *intent));
    CHECK(g->registry.get<CurrentIntention>(e).wake_at_millis == 1750);
}

// --- integration: a real, shipping wasm brain driving a live sim -----------

TEST_CASE("wasm: a town world ticks, heroes act (MoveTo/SetBehavior reach the log)") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    seed_heroes(g.get(), 3);
    for (int i = 0; i < 150; ++i) {
        tick_world(*g, 1.0f / 30.0f);
    }

    bool saw_move = false;
    bool saw_set_behavior = false;
    for (const Command& c : g->command_log) {
        saw_move = saw_move || c.kind == CommandKind::MoveTo;
        saw_set_behavior = saw_set_behavior || c.kind == CommandKind::SetBehavior;
    }
    CHECK(saw_move);
    CHECK(saw_set_behavior);
}

TEST_CASE("wasm: a sleeping hero (long hint) is woken by damage and re-decides within a tick") {
    std::vector<uint8_t> bytes = read_hero_wasm();
    auto g = make_world(wasm_desc(bytes));
    REQUIRE(g->wasm_brains != nullptr);

    uint32_t slot = spawn_into(*g, MercenaryDesc(0.0f, kCastleSpawnZ));
    entt::entity e = g->slots[slot];

    // One real wake: let the hero actually think (bh_spawn + a genuine
    // suggestion) so CurrentIntention reflects what react() produced.
    tick_world(*g, 1.0f / 30.0f);
    REQUIRE(g->registry.get<CurrentIntention>(e).last_think_millis == g->world_millis);

    // Force it deep asleep: an artificially far wake_at_millis, well beyond
    // any idle hint hero.nim could actually draw -- deterministic,
    // independent of the hint's real (randomized, bounded) value.
    CurrentIntention& ci = g->registry.get<CurrentIntention>(e);
    ci.wake_at_millis = g->world_millis + 60'000;
    REQUIRE_FALSE(should_wake(*g, e));

    const int64_t last_think_before = ci.last_think_millis;
    for (int i = 0; i < 5; ++i) {
        tick_world(*g, 1.0f / 30.0f);  // asleep: no re-think while nothing happens
    }
    CHECK(g->registry.get<CurrentIntention>(e).last_think_millis == last_think_before);

    // Inject a hit -- the same DamageTaken choke point real combat uses
    // (emit_char_hit, game_state.h) -- a guaranteed-wake event
    // (docs/design/intention-contract.html §2).
    emit_char_hit(*g, /*actor_slot=*/UINT32_MAX, slot, 3.0f,
                 g->registry.get<Health>(e).hp - 3.0f, g->registry.get<Position>(e).pos);
    CHECK(should_wake(*g, e));

    tick_world(*g, 1.0f / 30.0f);
    CHECK(g->registry.get<CurrentIntention>(e).last_think_millis == g->world_millis);
}

TEST_CASE("wasm: two identical runs produce identical command logs and character snapshots") {
    std::vector<uint8_t> bytes = read_hero_wasm();

    auto a = make_world(wasm_desc(bytes));
    auto b = make_world(wasm_desc(bytes));
    REQUIRE(a->wasm_brains != nullptr);
    REQUIRE(b->wasm_brains != nullptr);
    seed_heroes(a.get(), 3);
    seed_heroes(b.get(), 3);

    constexpr int kTicks = 150;
    for (int i = 0; i < kTicks; ++i) {
        tick_world(*a, 1.0f / 30.0f);
        tick_world(*b, 1.0f / 30.0f);
    }

    REQUIRE(a->command_log.size() == b->command_log.size());
    REQUIRE(!a->command_log.empty());
    for (size_t i = 0; i < a->command_log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(a->command_log[i], b->command_log[i]));
    }

    const std::vector<CharacterState> ca = characters_of(*a);
    const std::vector<CharacterState> cb = characters_of(*b);
    REQUIRE(ca.size() == cb.size());
    for (size_t i = 0; i < ca.size(); ++i) {
        INFO("character row " << i);
        CHECK(ca[i].pos_x == cb[i].pos_x);
        CHECK(ca[i].pos_z == cb[i].pos_z);
        CHECK(ca[i].behavior == cb[i].behavior);
        CHECK(ca[i].fatigue == cb[i].fatigue);
        CHECK(ca[i].content == cb[i].content);
    }
}
