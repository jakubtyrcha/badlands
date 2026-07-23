// The needs system: per-hero RESERVES that drain over time and are refilled by
// doing something about them.
//
// A reserve is a float in [0,1] where 1 is satisfied and 0 is spent. Every
// hero's `fatigue` and `content` drain each tick at a configurable rate; a hero
// asleep at home refills fatigue, one at the tavern refills content, one
// chatting refills content slowly and only to a ceiling.
//
// This replaces the old "needs are costs that rise, and entering a building
// zeroes them instantly" model. Filling over time is what makes rest a thing a
// hero SPENDS ITS EVENING ON rather than a doorway it touches, and it is what
// lets the leave decision be about the hero's state instead of a timer.
//
// Every rate lives in HeroFactors, in in-game hours, and is live at runtime.

#pragma once

struct BadlandsGame;

namespace badlands {

// Drains both reserves for heroes who are out in the world, and refills the
// relevant one for heroes who are asleep, at the tavern, or in conversation.
// Clamps to [0,1] at both ends. Deterministic, not dt-scaled.
void advance_needs(BadlandsGame& game);

}  // namespace badlands
