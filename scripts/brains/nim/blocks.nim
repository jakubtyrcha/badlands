# Port of the hero score_*/act_* pairs (game/src/behaviours/blocks.cpp)
# actually reachable through kHeroActivities (game/src/town_brain.cpp), now
# emitting a v2 Suggestion (BL_INT_*) instead of v1's Decision
# (activityId/goalKind/BL_CMD_*).
#
# A `score_*` returns a CONSIDERATION in [0,1] (0 = veto) -- UNCHANGED from
# v1, byte-for-byte the same comparisons/thresholds. `act_*` differs from v1
# in shape, not spirit: v1's engine (apply_brain_decision) gated a
# "walk-then-fire-follow-up-on-arrival" itself, generically, for every
# activity; the v2 engine (apply_intention, game/src/intention.h) has no such
# combined walk+gate for ENTER/ENTER_HOME/BUY -- each intention kind is
# either a walk (MoveTo) or a bare action, never both. So the three
# door-activities (GoHome/Buy/VisitTavern) now do that arrival check
# THEMSELVES, against BlViewFactors.entrance_radius (mirrors game/src/
# components.h's kEntranceRadius): far -> suggest MoveTo toward the door;
# close enough -> suggest the actual action. Chat/Hunt already carried their
# own precise gate in v1 (chat_radius/self_attack_range, both still on the
# wire) and are unchanged in spirit.

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

# How close to a door an act_* block below must be before firing the actual
# action (Enter/EnterHome/Buy) instead of just walking toward it -- read from
# the wire's own BlViewFactors.entrance_radius (mirrors game/src/components.h's
# kEntranceRadius; was a hand-copied constant here, now the wire is the single
# source of truth -- see brain_abi.h's BlViewFactors comment). v1 had this
# check done GENERICALLY, host-side, by apply_brain_decision's
# follow_up_on_arrival gate; v2's apply_intention has no such combined gate
# (see this file's top comment), so it moves here. Compared as a squared
# distance to avoid pulling in sqrt/libm at all -- one fewer thing for
# scripts/build_brains.sh's WASI-import bisect to worry about.
proc entranceRadiusSq(f: BlViewFactors): float32 =
  f.entrance_radius * f.entrance_radius

# How badly a depleted reserve wants attention: 0 at/above `threshold`, ramping
# linearly to 1 when empty. One shape, used by every need (blocks.cpp: urgency).
proc urgency(reserve, threshold: float32): float32 =
  if threshold <= 0.0'f32 or reserve >= threshold:
    return kNotApplicable
  let x = (threshold - reserve) / threshold
  result = if x < 0.0'f32: 0.0'f32 elif x > 1.0'f32: 1.0'f32 else: x

proc distSq(a, b: Vec2): float32 =
  let dx = a.x - b.x
  let dz = a.z - b.z
  result = dx * dx + dz * dz

# One activity's suggestion: BL_INT_* + the fields it uses (mirrors
# BlSuggestionWire's own shape, minus duration_millis/idle_hint_millis,
# which hero.nim's top-level react() fills in once, not per-activity).
type Suggestion* = object
  kind*: int32              # BL_INT_*
  activityLabel*: int32     # ActivityId, inspection only
  pointX*, pointZ*: float32
  targetSlot*: uint32
  arg*: int32

proc moveTo(activityLabel: int32, goal: Vec2): Suggestion =
  Suggestion(kind: BL_INT_MOVE_TO, activityLabel: activityLabel, pointX: goal.x, pointZ: goal.z)

# --- GoHome (rest) -----------------------------------------------------------
proc scoreGoHome*(v: HeroView, f: BlViewFactors): float32 =
  if not v.hasHome:
    return kNotApplicable
  let bar = if v.night: f.fatigue_seek_night else: f.fatigue_seek
  result = max(urgency(v.fatigue, bar), urgency(v.healthFrac, f.low_health_rest))

proc actGoHome*(v: HeroView, f: BlViewFactors): Suggestion =
  if distSq(v.pos, v.homeDoor) <= entranceRadiusSq(f):
    return Suggestion(kind: BL_INT_ENTER_HOME, activityLabel: ActGoHome)
  result = moveTo(ActGoHome, v.homeDoor)

# --- Buy ----------------------------------------------------------------------
proc scoreBuy*(v: HeroView, f: BlViewFactors): float32 =
  result = if v.hasApothecary and v.inventory < kInventoryCap: kApplies else: kNotApplicable

proc actBuy*(v: HeroView, f: BlViewFactors): Suggestion =
  if distSq(v.pos, v.apothecaryDoor) <= entranceRadiusSq(f):
    return Suggestion(kind: BL_INT_BUY, activityLabel: ActBuy)
  result = moveTo(ActBuy, v.apothecaryDoor)

# --- VisitTavern ---------------------------------------------------------------
proc scoreVisitTavern*(v: HeroView, f: BlViewFactors): float32 =
  if not v.hasTavern or v.night:
    return kNotApplicable  # shut after dark
  result = urgency(v.content, f.content_seek)

proc actVisitTavern*(v: HeroView, f: BlViewFactors): Suggestion =
  if distSq(v.pos, v.tavernDoor) <= entranceRadiusSq(f):
    return Suggestion(kind: BL_INT_ENTER, activityLabel: ActVisitTavern, arg: kBuildingKindTavern)
  result = moveTo(ActVisitTavern, v.tavernDoor)

# --- Chat -----------------------------------------------------------------------
proc scoreChat*(v: HeroView, f: BlViewFactors): float32 =
  if v.chatting:
    return kApplies  # mid-conversation: see it through
  if not v.hasChatPartner:
    return kNotApplicable
  result = urgency(v.content, f.chat_content_seek)

proc actChat*(v: HeroView, f: BlViewFactors): Suggestion =
  if v.chatting:
    # Nothing NEW to decide: the conversation runs to its own clock
    # (ChattingState + advance_chats, engine-side), and observe_hero never
    # populates partner_slot while already chatting (v.chatting excludes the
    # nearest_companion scan), so there is no valid target to re-suggest Chat
    # with even if we wanted to. BL_INT_IDLE here (not BL_INT_NONE) is
    # deliberate, not a mistake: apply_intention's BL_INT_NONE path leaves
    # CurrentIntention untouched entirely, including its wake_at_millis --
    # which would leave a chatting hero's wake schedule stuck at whatever it
    # was BEFORE the conversation started, so once that deadline passed it
    # would re-wake the brain every single tick for as long as the chat
    # continues. Idle's own duration_millis (the idle hint) keeps a fresh,
    # bounded re-check window instead -- activityLabel still reports Chat for
    # inspection, so the histogram/HeroSimulationState.behavior are unaffected.
    return Suggestion(kind: BL_INT_IDLE, activityLabel: ActChat)
  # Walk over, and strike it up once close enough -- the engine still gates
  # on chat_radius itself (command.cpp's Chat handler), this just avoids
  # suggesting Chat before it could possibly be valid.
  if v.partnerDist <= f.chat_radius:
    return Suggestion(kind: BL_INT_CHAT, activityLabel: ActChat, targetSlot: v.partnerSlot)
  result = moveTo(ActChat, v.partnerPos)

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

proc actExplore*(v: HeroView, f: BlViewFactors): Suggestion =
  result = moveTo(ActExplore, v.exploreGoal)

# --- Hunt (hunter) -----------------------------------------------------------------
proc scoreHunt*(v: HeroView, f: BlViewFactors): float32 =
  result = if v.hasPrey: kApplies else: kNotApplicable

proc actHunt*(v: HeroView, f: BlViewFactors): Suggestion =
  # Chase to prey_pos; once within the hunter's own reach, take the shot (the
  # handler re-checks range + cooldown, so the log gets one entry per shot).
  if v.preyDist <= v.selfAttackRange:
    return Suggestion(kind: BL_INT_SHOOT, activityLabel: ActHunt, targetSlot: v.preySlot)
  result = moveTo(ActHunt, v.preyPos)

# --- Roam (shared) -------------------------------------------------------------------
# Unlike blocks.cpp, this does NOT re-derive the wander point: observe_hero
# already drew it host-side (roam_point) and shipped it as suggest.roam_goal_*.
proc scoreRoam*(v: HeroView, f: BlViewFactors): float32 = kApplies

proc actRoam*(v: HeroView, f: BlViewFactors): Suggestion =
  result = moveTo(ActRoam, v.roamGoal)

# --- Idle -----------------------------------------------------------------------------
proc scoreIdle*(v: HeroView, f: BlViewFactors): float32 = kApplies

proc actIdle*(v: HeroView, f: BlViewFactors): Suggestion =
  # duration_millis is filled in by hero.nim's top-level react() (the idle
  # hint IS the duration now -- see its own comment).
  Suggestion(kind: BL_INT_IDLE, activityLabel: ActIdle)
