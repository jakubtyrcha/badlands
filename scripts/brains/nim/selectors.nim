# Port of select_banded (game/src/behaviours/selectors.cpp) -- the only
# selector a hero needs (select_argmax/select_priority drive other archetypes,
# never a hero, so they are not ported). Band loop first (Danger, then
# Normal), then per-band argmax over weight*score with a strict `>` so ties
# keep the earliest list entry -- verbatim, including the early-continue/skip
# order.

import abi
import activity_catalog
import hero_view
import blocks

type
  ScoreFn* = proc(v: HeroView, f: BlViewFactors): float32 {.nimcall.}
  ActFn* = proc(v: HeroView, f: BlViewFactors): Decision {.nimcall.}

  # Mirrors ActivityDef (behaviours/blocks.h): one row of the activity table.
  ActivityEntry* = object
    id*: int32
    band*: Band
    score*: ScoreFn
    act*: ActFn

proc selectBanded*(activities: openArray[ActivityEntry], weights: array[BL_MAX_ACTIVITIES, float32],
                   v: HeroView, f: BlViewFactors): Decision =
  # Bands are tried strictly in order (Danger=0, Normal=1) -- the hierarchy is
  # enforced by control flow, not by the numbers.
  for bandIdx in 0 ..< 2:
    let band = Band(bandIdx)
    var bestIdx = -1
    var bestUtility: float32 = 0.0'f32
    for i in 0 ..< activities.len:
      if activities[i].band != band:
        continue
      let weight = weights[int(activities[i].id)]
      if weight <= 0.0'f32:
        continue  # this actor does not have this activity at all
      let score = activities[i].score(v, f)
      if score <= 0.0'f32:
        continue  # vetoed by its own considerations
      let utility = weight * score
      if utility > bestUtility:  # strictly greater -> ties keep the earliest
        bestUtility = utility
        bestIdx = i
    if bestIdx >= 0:
      return activities[bestIdx].act(v, f)
  # Nothing applicable anywhere -> Idle in place. Matches C++'s default
  # BehaviourResult{} (id=Idle, no follow-up) with TWO deliberate wire-only
  # differences, neither of which is observable host-side (no follow_up means
  # apply_brain_decision never reads goal_kind's self-position choice nor
  # follow_up_on_arrival at all -- see decode_decision/apply_brain_decision):
  # goalKind=0 ("no goal", host holds current position) rather than an
  # explicit (0,0) point, and followUpOnArrival=false rather than C++'s
  # struct-default true. This branch is unreachable for a hero with real
  # factors (Idle's weight is always > 0, so the Normal band always finds at
  # least Idle) -- it fires only when every weight on the wire is exactly 0,
  # which is what src/crates/brainhost's real_hero_wasm_conforms test feeds
  # (an all-zeroed view) and asserts an all-zero-bytes DecisionWire back, so
  # every field here must be the wire's zero value to keep that acceptance
  # test green.
  result = Decision(activityId: ActIdle, goalKind: 0, followUpOnArrival: false)
