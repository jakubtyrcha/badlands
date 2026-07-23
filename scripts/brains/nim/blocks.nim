# Bit-exact port of the hero score_*/act_* pairs (game/src/behaviours/blocks.cpp)
# actually reachable through kHeroActivities (game/src/town_brain.cpp:224-233):
# Explore, GoHome, Hunt, Buy, VisitTavern, Chat, Roam, Idle. Every threshold and
# comparison mirrors the C++ verbatim -- float32 throughout, C++ operation order.
#
# A `score_*` returns a CONSIDERATION in [0,1] (0 = veto); an `act_*` returns a
# `Decision`, this module's stand-in for BehaviourResult -- the non-pause half
# of BlDecisionWire (activity/goal/command), which the selector/hero.nim then
# either commits as-is or replaces with a Think pause. `goalKind` follows the
# wire's own convention: 0 = "no explicit goal" (the host holds the entity's
# current position, same as C++ decode_decision's self_pos fallback), 1 = an
# explicit (goalX, goalZ) point -- used here exactly where the C++ act_* target
# IS the entity's own current position (act_idle, act_chat while already
# chatting), so no value is round-tripped for nothing.

import abi
import activity_catalog
import hero_view

# Scores are considerations, never priorities/preferences (see blocks.cpp's
# top-of-file comment) -- kApplies/kNotApplicable name the two flat cases.
const
  kApplies: float32 = 1.0'f32
  kNotApplicable: float32 = 0.0'f32

  # components.h: constexpr int kInventoryCap = 2 (elixirs a hero can carry).
  kInventoryCap: int32 = 2

  # badlands_sim.hpp's BuildingKind: Castle=0, FreeCompanyQuarters=1,
  # HuntersCamp=2, ThievesDen=3, Scriptorium=4, Tavern=5.
  kBuildingKindTavern: int32 = 5

# How badly a depleted reserve wants attention: 0 at/above `threshold`, ramping
# linearly to 1 when empty. One shape, used by every need (blocks.cpp: urgency).
proc urgency(reserve, threshold: float32): float32 =
  if threshold <= 0.0'f32 or reserve >= threshold:
    return kNotApplicable
  let x = (threshold - reserve) / threshold
  result = if x < 0.0'f32: 0.0'f32 elif x > 1.0'f32: 1.0'f32 else: x

# The non-pause half of one tick's decision: mirrors BehaviourResult
# (activity id, goal, optional follow-up command, follow_up_on_arrival).
type Decision* = object
  activityId*: int32
  goalKind*: int32          # 0 none (self pos), 1 point
  goalX*, goalZ*: float32
  commandKind*: int32       # BL_CMD_*
  commandArg*: int32
  followUpOnArrival*: bool

# --- GoHome (rest) -----------------------------------------------------------
proc scoreGoHome*(v: HeroView, f: BlViewFactors): float32 =
  if not v.hasHome:
    return kNotApplicable
  let bar = if v.night: f.fatigue_seek_night else: f.fatigue_seek
  result = max(urgency(v.fatigue, bar), urgency(v.healthFrac, f.low_health_rest))

proc actGoHome*(v: HeroView, f: BlViewFactors): Decision =
  Decision(activityId: ActGoHome, goalKind: 1, goalX: v.homeDoor.x, goalZ: v.homeDoor.z,
           commandKind: BL_CMD_ENTER_HOME, commandArg: 0, followUpOnArrival: true)

# --- Buy ----------------------------------------------------------------------
proc scoreBuy*(v: HeroView, f: BlViewFactors): float32 =
  result = if v.hasApothecary and v.inventory < kInventoryCap: kApplies else: kNotApplicable

proc actBuy*(v: HeroView, f: BlViewFactors): Decision =
  Decision(activityId: ActBuy, goalKind: 1, goalX: v.apothecaryDoor.x, goalZ: v.apothecaryDoor.z,
           commandKind: BL_CMD_BUY, commandArg: 0, followUpOnArrival: true)

# --- VisitTavern ---------------------------------------------------------------
proc scoreVisitTavern*(v: HeroView, f: BlViewFactors): float32 =
  if not v.hasTavern or v.night:
    return kNotApplicable  # shut after dark
  result = urgency(v.content, f.content_seek)

proc actVisitTavern*(v: HeroView, f: BlViewFactors): Decision =
  Decision(activityId: ActVisitTavern, goalKind: 1, goalX: v.tavernDoor.x, goalZ: v.tavernDoor.z,
           commandKind: BL_CMD_ENTER, commandArg: kBuildingKindTavern, followUpOnArrival: true)

# --- Chat -----------------------------------------------------------------------
proc scoreChat*(v: HeroView, f: BlViewFactors): float32 =
  if v.chatting:
    return kApplies  # mid-conversation: see it through
  if not v.hasChatPartner:
    return kNotApplicable
  result = urgency(v.content, f.chat_content_seek)

proc actChat*(v: HeroView, f: BlViewFactors): Decision =
  if v.chatting:
    return Decision(activityId: ActChat, goalKind: 0, followUpOnArrival: false)  # stand and talk
  # Walk over, and strike it up once close enough -- emitting on arrival keeps
  # the approach itself out of the command log (mirrors act_chat's comment).
  result = Decision(activityId: ActChat, goalKind: 1, goalX: v.partnerPos.x, goalZ: v.partnerPos.z,
                     followUpOnArrival: false)
  if v.partnerDist <= f.chat_radius:
    result.commandKind = BL_CMD_CHAT
    result.commandArg = int32(v.partnerSlot)

# --- Explore ---------------------------------------------------------------------
proc scoreExplore*(v: HeroView, f: BlViewFactors): float32 =
  if not v.hasExploreGoal:
    return kNotApplicable  # nowhere unknown within reach, or not in the mood
  if v.moveBlocked:
    return kNotApplicable  # the world said no; try elsewhere next window
  if v.fatigue <= f.explore_min_fatigue:
    return kNotApplicable  # not enough in the tank -- stay near home
  if v.hasPrey:
    return kNotApplicable  # something worth stopping for is right here
  result = kApplies

proc actExplore*(v: HeroView, f: BlViewFactors): Decision =
  Decision(activityId: ActExplore, goalKind: 1, goalX: v.exploreGoal.x, goalZ: v.exploreGoal.z,
           followUpOnArrival: false)

# --- Hunt (hunter) -----------------------------------------------------------------
proc scoreHunt*(v: HeroView, f: BlViewFactors): float32 =
  result = if v.hasPrey: kApplies else: kNotApplicable

proc actHunt*(v: HeroView, f: BlViewFactors): Decision =
  # Chase to prey_pos; once within the hunter's own reach, take the shot (the
  # handler re-checks range + cooldown, so the log gets one entry per shot).
  result = Decision(activityId: ActHunt, goalKind: 1, goalX: v.preyPos.x, goalZ: v.preyPos.z,
                     followUpOnArrival: false)
  if v.preyDist <= v.selfAttackRange:
    result.commandKind = BL_CMD_SHOOT
    result.commandArg = int32(v.preySlot)

# --- Roam (shared) -------------------------------------------------------------------
# Unlike blocks.cpp, this does NOT re-derive the wander point: observe_hero
# already drew it host-side (roam_point) and shipped it as suggest.roam_goal_*
# -- act_roam on the C++ side simply reads v.roam_goal too, so there is nothing
# left for this block to compute.
proc scoreRoam*(v: HeroView, f: BlViewFactors): float32 = kApplies

proc actRoam*(v: HeroView, f: BlViewFactors): Decision =
  Decision(activityId: ActRoam, goalKind: 1, goalX: v.roamGoal.x, goalZ: v.roamGoal.z,
           followUpOnArrival: false)

# --- Idle -----------------------------------------------------------------------------
proc scoreIdle*(v: HeroView, f: BlViewFactors): float32 = kApplies

proc actIdle*(v: HeroView, f: BlViewFactors): Decision =
  Decision(activityId: ActIdle, goalKind: 0, followUpOnArrival: false)  # hold position
