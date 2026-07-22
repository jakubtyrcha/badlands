// Noiser brain integration: compiles the brain script, binds the host-call
// interface (perception in, intents out — the Weave idiom), and owns the
// per-entity coroutine contexts.

#pragma once

#include <noiser.hpp>

#include <memory>
#include <string>

struct BadlandsGame;

namespace badlands {

// One compiled brain program with its host bindings. Recreated on hot reload.
struct BrainRuntime {
    std::shared_ptr<sampo::noiser::NoiserProgram> program;

    // Compiles `source` (on a big-stack thread) and binds the host functions
    // against `game`. Returns null and fills `out_error` on any failure; the
    // caller records the bug and runs mock-only.
    static std::unique_ptr<BrainRuntime> create(BadlandsGame& game,
                                                const std::string& source,
                                                std::string& out_error);
};

// Per-entity stackless coroutine state, stored in the Brain component.
struct BrainState {
    sampo::noiser::ExecutionContext ctx;
    bool downgraded = false;
};

// Which decision logic drives this entity when no script brain is running.
// Set once by the spawn recipe (heroes.cpp) and dispatched on in tick_world --
// this is how archetype selects a brain WITHOUT anything querying "is this a
// Hero" at think time.
enum class BrainKind : int32_t {
    None = 0,   // no autonomous decisions (combat pre-empt only)
    Town,       // hero errand/needs loop (town_brain.cpp)
    Critter,    // reactive roam/graze/flee
    Townfolk,   // sequential routes
    Monster,    // seek and engage
};

// ECS component. Null state (or downgraded) -> the BrainKind mock drives it.
struct Brain {
    std::unique_ptr<BrainState> state;
    BrainKind kind = BrainKind::None;
};

// Fresh coroutine for the entity slot; the slot id is the generator's
// parameter (delivered via NoiserInput.warp_id).
std::unique_ptr<BrainState> spawn_brain(const BrainRuntime& runtime, uint32_t slot);

// Resumes the entity's coroutine for one tick; intents arrive as host-call
// side effects. Returns false when the brain failed (caller downgrades).
bool resume_brain(BadlandsGame& game, uint32_t slot, BrainState& state);

}  // namespace badlands
