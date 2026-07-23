#pragma once

// Deliberation: the pause a character takes when it changes its mind.
//
// Selection alone produces a character that pivots mid-stride the instant its
// needs cross a threshold -- correct, and unmistakably mechanical. Real people
// stop, stand for a moment, and then set off. This layer sits BETWEEN selection
// and commitment and does exactly that, and nothing else.
//
// It is deliberately not a behaviour block. A block competes for selection;
// this modifies what happens to the thing already selected, so folding it into
// the table would mean an activity whose score depended on which activity won
// -- a circularity the band model exists to avoid.
//
// Pure function of the WorldView: no registry, no sim, no clock of its own.

#include <cstdint>

#include "badlands_sim.hpp"
#include "behaviours/world_view.h"

namespace badlands {

// What to do with the activity selection just made.
struct ThinkDecision {
    // true  -> hold position and deliberate instead of committing.
    bool pause = false;
    // >0 -> START a pause of this many sim milliseconds (worth recording).
    //  0 with pause=true -> a pause already in progress simply continues, which
    //    is NOT a new decision and must not enter the command log.
    int64_t duration_millis = 0;
};

// Should the character pause before committing to `chosen`?
//
// Pauses only on a genuine change of mind between two DISCRETIONARY goals while
// nothing threatens it. Specifically it does NOT pause when:
//
//   * anything the observer counts as a threat is in proximity, or the chosen
//     goal is itself a Danger-band one -- you do not stand and think about a
//     wolf. This also CANCELS a pause already running.
//   * the choice is unchanged (no decision was made to deliberate over).
//   * it was already deliberating (current == Think): the pause just ended, and
//     pausing again would loop forever.
//   * either the old or the new goal is non-discretionary (Danger/Fallback, or
//     the -1 "nothing yet" of a freshly spawned character).
//
// Duration is drawn deterministically from (slot, now) in
// [think_min_millis, think_max_millis], so a run and its replay pause
// identically -- and setting think_max_millis to 0 disables deliberation
// entirely, which is how timing-sensitive tests opt out.
ThinkDecision deliberate(ActivityId chosen, const WorldView& view, const SimFactors& factors);

// Is `id` a goal worth deliberating over -- i.e. a Productive or Filler one?
// Out-of-range ids (a character that has not decided anything yet) are not.
bool is_discretionary(int32_t id);

}  // namespace badlands
