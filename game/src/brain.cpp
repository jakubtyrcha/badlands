#include "brain.h"

#include "game_state.h"
#include "heroes.h"
#include "placement.h"

#include <pthread.h>

#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace badlands {

namespace {

using sampo::noiser::CompileError;
using sampo::noiser::NoiserInput;
using sampo::noiser::NoiserProgram;

// The noiser compiler recurses deeply and assumes RUST_MIN_STACK-sized stacks
// (the noiser repo pins 64 MiB); the caller's thread stack is not enough, so
// compilation runs on a dedicated big-stack thread.
std::expected<std::shared_ptr<NoiserProgram>, CompileError> compile_big_stack(
    const std::string& source) {
    using Result = std::expected<std::shared_ptr<NoiserProgram>, CompileError>;
    struct Job {
        const std::string* source;
        std::optional<Result> result;
    } job{&source, std::nullopt};

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread;
    auto entry = [](void* raw) -> void* {
        auto* j = static_cast<Job*>(raw);
        j->result = NoiserProgram::Compile(*j->source);
        return nullptr;
    };
    if (pthread_create(&thread, &attr, entry, &job) != 0) {
        pthread_attr_destroy(&attr);
        return std::unexpected(CompileError{.message = "failed to spawn compile thread"});
    }
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    return std::move(*job.result);
}

std::string format_error(const CompileError& err) {
    std::string out = err.message;
    if (err.line > 0) {
        out += " at " + (err.module.empty() ? std::string("main") : err.module) + ":" +
               std::to_string(err.line) + ":" + std::to_string(err.column);
    }
    return out;
}

glm::vec4 perceive_self(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "perceive_self", "invalid entity slot " + std::to_string(slot));
        return glm::vec4(0.0f);
    }
    const auto& pos = game.registry.get<Position>(self);
    const auto& health = game.registry.get<Health>(self);
    const auto& cooldown = game.registry.get<CooldownTimer>(self);
    return {pos.pos.x, pos.pos.y, health.hp, cooldown.remaining};
}

glm::vec4 perceive_target(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "perceive_target", "invalid entity slot " + std::to_string(slot));
        return glm::vec4(0.0f);
    }
    entt::entity target = nearest_enemy(game, self);
    if (target == entt::null) {
        return glm::vec4(0.0f);
    }
    const auto& pos = game.registry.get<Position>(target);
    const auto& health = game.registry.get<Health>(target);
    return {pos.pos.x, pos.pos.y, health.hp, 1.0f};
}

float attack_range(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "attack_range", "invalid entity slot " + std::to_string(slot));
        return 0.0f;
    }
    return game.registry.get<Stats>(self).attack_range;
}

void intent_move(BadlandsGame& game, int32_t slot, float dx, float dz) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "intent_move", "invalid entity slot " + std::to_string(slot));
        return;
    }
    game.registry.get<Intent>(self) = {.kind = 1, .dir = {dx, dz}};
    ++game.script_intents;
}

void intent_attack(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "intent_attack", "invalid entity slot " + std::to_string(slot));
        return;
    }
    game.registry.get<Intent>(self) = {.kind = 2, .dir = {0.0f, 0.0f}};
    ++game.script_intents;
}

// --- v0.3 town host calls ---------------------------------------------------
// Perception returns are flat f32 tuples (bound as glm::vec4); the building id
// is never returned to the brain -- intent_enter(kind) re-derives nearest-of-kind
// engine-side, so the hero standing at that door is unambiguous.

// (door_x, door_z, exists, _) for the nearest alive building of `kind`.
glm::vec4 perceive_building(BadlandsGame& game, int32_t slot, int32_t kind) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "perceive_building", "invalid entity slot " + std::to_string(slot));
        return glm::vec4(0.0f);
    }
    glm::vec2 p = game.registry.get<Position>(self).pos;
    uint32_t bid = nearest_building_of(game.placement, kind, p);
    if (bid == std::numeric_limits<uint32_t>::max()) {
        return glm::vec4(0.0f);
    }
    glm::vec2 tile;
    if (!building_approach_tile(game.placement, game.placement.buildings[bid], tile)) {
        return glm::vec4(0.0f);
    }
    return {tile.x, tile.y, 1.0f, 0.0f};
}

// (door_x, door_z, exists, _) for the hero's home guild.
glm::vec4 perceive_home(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "perceive_home", "invalid entity slot " + std::to_string(slot));
        return glm::vec4(0.0f);
    }
    int32_t home = game.registry.get<Home>(self).building_id;
    auto& bs = game.placement.buildings;
    if (home < 0 || static_cast<size_t>(home) >= bs.size() || !bs[home].alive) {
        return glm::vec4(0.0f);
    }
    glm::vec2 tile;
    if (!building_approach_tile(game.placement, bs[home], tile)) {
        return glm::vec4(0.0f);
    }
    return {tile.x, tile.y, 1.0f, 0.0f};
}

float inventory_count(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "inventory_count", "invalid entity slot " + std::to_string(slot));
        return 0.0f;
    }
    return static_cast<float>(game.registry.get<Inventory>(self).count);
}

// The hero's class (HeroClassId), so the brain can pick its per-class behaviour
// profile. -1 for a home-less/non-hero entity (still runs the shared brain).
int32_t perceive_class(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "perceive_class", "invalid entity slot " + std::to_string(slot));
        return -1;
    }
    if (!game.registry.all_of<HeroClass>(self)) {
        return -1;
    }
    return game.registry.get<HeroClass>(self).value;
}

// Durable walk goal (engine navmesh-paths); replaces intent_move for town heroes.
void intent_move_to(BadlandsGame& game, int32_t slot, float x, float z) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "intent_move_to", "invalid entity slot " + std::to_string(slot));
        return;
    }
    MoveTarget& mt = game.registry.get<MoveTarget>(self);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {x, z};
    mt.entity = entt::null;
    mt.building = UINT32_MAX;
    mt.stop_distance = 0.0f;
    ++game.script_intents;
}

void intent_enter(BadlandsGame& game, int32_t slot, int32_t kind) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "intent_enter", "invalid entity slot " + std::to_string(slot));
        return;
    }
    hero_enter(game, self, kind);
    ++game.script_intents;
}

void intent_enter_home(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "intent_enter_home", "invalid entity slot " + std::to_string(slot));
        return;
    }
    hero_enter_home(game, self);
    ++game.script_intents;
}

void intent_buy(BadlandsGame& game, int32_t slot) {
    entt::entity self = entity_for_slot(game, slot);
    if (self == entt::null) {
        report_bug(game, "intent_buy", "invalid entity slot " + std::to_string(slot));
        return;
    }
    hero_buy(game, self);
    ++game.script_intents;
}

}  // namespace

std::unique_ptr<BrainRuntime> BrainRuntime::create(BadlandsGame& game,
                                                   const std::string& source,
                                                   std::string& out_error) {
    auto compiled = compile_big_stack(source);
    if (!compiled) {
        out_error = format_error(compiled.error());
        return nullptr;
    }
    auto runtime = std::make_unique<BrainRuntime>();
    runtime->program = std::move(*compiled);

    BadlandsGame* g = &game;  // outlives the program: game owns the runtime
    NoiserProgram& prog = *runtime->program;

    // Resilient binding: bind only the host functions the script actually
    // declares (probed via GetCallableLocation), and fail only if a *declared*
    // @fn cannot be bound (real drift). This lets the town brain declare the
    // fuller interface while the duel/downgrade fixtures declare a minimal set,
    // without a "declared but unbound" failure. A script that declares a @fn the
    // host does not implement is left unbound and fails loudly at resume.
    //
    // Tradeoff: because create() no longer fails when a script simply omits a
    // declaration, a hot-reloaded brain that drops a needed @fn binds fine and
    // just stops performing that action (visible in dev). game_reload_script
    // still keeps-last-good on a genuine build failure (compile error or a
    // declared-but-unbound @fn). The undeclared-bind-fail smoke test pins the
    // failure half of this contract.
    bool ok = true;
    auto bind = [&](const char* name, auto fn) {
        if (!prog.GetCallableLocation(name).has_value()) {
            return;  // script does not declare it; nothing to bind
        }
        if (!prog.BindCallableByName(name, std::function(std::move(fn)))) {
            ok = false;
        }
    };

    bind("perceive_self", [g](int32_t e) { return perceive_self(*g, e); });
    bind("perceive_target", [g](int32_t e) { return perceive_target(*g, e); });
    bind("attack_range", [g](int32_t e) { return attack_range(*g, e); });
    bind("perceive_building",
         [g](int32_t e, int32_t kind) { return perceive_building(*g, e, kind); });
    bind("perceive_home", [g](int32_t e) { return perceive_home(*g, e); });
    bind("inventory_count", [g](int32_t e) { return inventory_count(*g, e); });
    bind("perceive_class", [g](int32_t e) { return perceive_class(*g, e); });
    bind("intent_move", [g](int32_t e, float dx, float dz) { intent_move(*g, e, dx, dz); });
    bind("intent_move_to", [g](int32_t e, float x, float z) { intent_move_to(*g, e, x, z); });
    bind("intent_attack", [g](int32_t e) { intent_attack(*g, e); });
    bind("intent_enter", [g](int32_t e, int32_t kind) { intent_enter(*g, e, kind); });
    bind("intent_enter_home", [g](int32_t e) { intent_enter_home(*g, e); });
    bind("intent_buy", [g](int32_t e) { intent_buy(*g, e); });

    if (!ok) {
        out_error = "BindCallableByName failed: a declared @fn has no host implementation";
        return nullptr;
    }
    prog.FreezeHostThunks();
    return runtime;
}

std::unique_ptr<BrainState> spawn_brain(const BrainRuntime& runtime, uint32_t slot) {
    auto state = std::make_unique<BrainState>();
    state->ctx = runtime.program->Prepare(
        NoiserInput{.warp_id = {static_cast<int32_t>(slot), 0, 0}});
    return state;
}

bool resume_brain(BadlandsGame& game, uint32_t slot, BrainState& state) {
    CompileError err;
    auto result = game.brains->program->Resume(state.ctx, &err);
    if (result.has_value()) {
        return true;  // yielded its heartbeat; intents arrived via host calls
    }
    if (!err.message.empty()) {
        report_bug(game, "resume", "entity " + std::to_string(slot) + ": " + format_error(err));
    } else {
        // The brain script loops forever; completing means it fell out of it.
        report_bug(game, "resume",
                   "entity " + std::to_string(slot) + ": brain generator completed unexpectedly");
    }
    return false;
}

}  // namespace badlands
