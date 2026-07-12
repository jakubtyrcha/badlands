#include "brain.h"

#include "game_state.h"

#include <pthread.h>

#include <cstdio>
#include <functional>
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
    bool bound =
        runtime->program->BindCallableByName(
            "perceive_self",
            std::function<glm::vec4(int32_t)>([g](int32_t e) { return perceive_self(*g, e); })) &&
        runtime->program->BindCallableByName(
            "perceive_target",
            std::function<glm::vec4(int32_t)>([g](int32_t e) { return perceive_target(*g, e); })) &&
        runtime->program->BindCallableByName(
            "attack_range",
            std::function<float(int32_t)>([g](int32_t e) { return attack_range(*g, e); })) &&
        runtime->program->BindCallableByName(
            "intent_move", std::function<void(int32_t, float, float)>(
                               [g](int32_t e, float dx, float dz) { intent_move(*g, e, dx, dz); })) &&
        runtime->program->BindCallableByName(
            "intent_attack",
            std::function<void(int32_t)>([g](int32_t e) { intent_attack(*g, e); }));
    if (!bound) {
        out_error = "BindCallableByName failed: script @fn declarations drifted from the host";
        return nullptr;
    }
    runtime->program->FreezeHostThunks();
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
