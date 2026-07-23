# Port of deliberate/is_discretionary (game/src/behaviours/deliberation.cpp):
# the pause a hero takes when it changes its mind. Pure function of the view;
# the pause rules and duration draw are ported verbatim, same order, same
# early-return shape as the C++ (each `if` below is one C++ `if`, in the same
# sequence, so a reviewer can diff line-for-line against deliberation.cpp).

import abi
import activity_catalog
import hero_view
import rng

# Mirrors ThinkDecision (behaviours/deliberation.h).
type ThinkDecision* = object
  pause*: bool
  durationMillis*: int64

# Anything that is not an immediate-danger response (a hero deliberates over
# which errand to run; it does not deliberate over whether to flee).
proc isDiscretionary*(id: int32): bool =
  if id < 0 or id >= BL_MAX_ACTIVITIES:
    return false  # nothing decided yet -- there is no mind to change
  result = bandOf(id) != bDanger

proc deliberate*(chosen: int32, v: HeroView, f: BlViewFactors): ThinkDecision =
  # Danger ends deliberation, and cancels one in progress. Checked first so no
  # combination of the rules below can keep a character standing still while
  # something is bearing down on it.
  if v.hasThreat or bandOf(chosen) == bDanger:
    return ThinkDecision(pause: false, durationMillis: 0)

  # A pause already running just continues -- no new decision, nothing logged.
  if v.nowMillis < v.thinkUntilMillis:
    return ThinkDecision(pause: true, durationMillis: 0)

  # Only a genuine change of mind is worth mulling over.
  if chosen == v.currentActivity:
    return ThinkDecision(pause: false, durationMillis: 0)
  # The pause that just ended must not immediately start another.
  if v.currentActivity == ActThink:
    return ThinkDecision(pause: false, durationMillis: 0)
  # Idle is the ABSENCE of a goal, not a goal: treated like "no decision yet".
  if chosen == ActIdle or v.currentActivity == ActIdle:
    return ThinkDecision(pause: false, durationMillis: 0)
  if not isDiscretionary(v.currentActivity) or not isDiscretionary(chosen):
    return ThinkDecision(pause: false, durationMillis: 0)

  var s = seedOf(v.slot, v.nowMillis)
  let duration = rangeI64(s, f.think_min_millis, f.think_max_millis)
  # A zero draw is not a pause: it would log a Think the character leaves in
  # the same tick (and is what makes think_max_millis = 0 a clean "off" switch).
  result = if duration > 0: ThinkDecision(pause: true, durationMillis: duration)
           else: ThinkDecision(pause: false, durationMillis: 0)
