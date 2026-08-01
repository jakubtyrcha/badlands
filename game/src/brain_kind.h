// Brain routing: which decision logic drives an entity. This header keeps only
// the identity bit every archetype spawn recipe and think dispatch still needs.

#pragma once

#include <cstdint>

namespace badlands {

// Which decision logic drives this entity when no script brain is running.
// Set once by the spawn recipe (heroes.cpp) and dispatched on in step_world --
// this is how archetype selects a brain WITHOUT anything querying "is this a
// Hero" at think time.
enum class BrainKind : int32_t {
    None = 0,   // no autonomous decisions (combat pre-empt only)
    Town,       // hero: the wasm brain decides (game/src/wasm_brain.h); no
                // wasm loaded -> idles, same as None
    Critter,    // reactive roam/graze/flee
    Townfolk,   // sequential routes
    Monster,    // seek and engage
};

// ECS component: routing only.
struct Brain {
    BrainKind kind = BrainKind::None;
};

}  // namespace badlands
