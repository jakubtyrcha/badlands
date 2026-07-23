// The needs system: raises per-hero fatigue/boredom over time (the day/night
// loop's dynamic state). A deterministic per-tick pass; satisfaction happens
// on entering home/tavern (reset-on-enter, in heroes.cpp). Growth rates are
// policy placeholders (components.h) — the architecture, not the mechanism, is
// what this slice pins.

#pragma once

struct BadlandsGame;

namespace badlands {

// Raises fatigue/boredom by fixed per-tick deltas (kFatiguePerTick/
// kBoredomPerTick) for heroes NOT hidden inside a building; clamps to [0,1].
void advance_needs(BadlandsGame& game);

}  // namespace badlands
