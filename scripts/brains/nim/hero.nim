# The shipping hero brain (v2, the intention contract): react-on-wake. This
# module is called ONLY on a real wake (should_wake gates it, host-side,
# game/src/sim.cpp) -- never per-tick -- and returns exactly one suggestion:
# an intention, an activity label (inspection only), and an idle hint.
#
# Keeps the SAME scoring/selection this brain always had (blocks.nim/
# selectors.nim, byte-for-byte the same considerations/thresholds as the C++
# reference town_brain.cpp once was) -- what changed is the OUTPUT shape
# (BlSuggestionWire's BL_INT_* vocabulary replaces v1's BL_CMD_*/goal_kind
# pair) and that deliberation.nim (the old "stand and think it over" pause)
# is gone outright: the idle hint IS that pause now, drawn unconditionally
# every wake rather than only on a discretionary activity change.
#
# New this wake, versus v1: threats in view -> BL_INT_ATTACK; a recent
# DamageTaken/ThreatSighted event in the inbox biases toward retreating home
# over the normal selection. MoveBlocked needs no NEW handling here:
# scoreExplore's existing veto (BlViewSuggest.move_blocked, unchanged from
# v1) already covers "avoid re-suggesting a blocked point" -- the inbox
# event's own role is purely to be a guaranteed-wake trigger (see InboxEvent's
# own doc comment, game/src/components.h: the sibling component already
# carries the position). Every OTHER wake, including a spurious one where
# nothing changed, simply re-runs selectBanded -- re-suggesting the same
# intention is a valid, idempotent answer (docs/design/intention-contract.html
# §2): apply_intention's v3 restate-resume (game/src/intention.cpp) diffs it
# against what's already running and, for an identical restatement, adopts it
# as a no-op refresh (only the wake schedule moves) rather than re-running the
# producer -- so this brain always restates, unconditionally, and leaves the
# dedup to the engine.
#
# v3 (contract-v3-alignment, docs/superpowers/specs/2026-07-25-contract-v3-
# alignment-design.md): the threat branch below is LIVE (sim.cpp's think
# dispatch consults this brain during combat too, not just in an enemy's
# absence -- should_wake's own high-stakes clause already demands it every
# tick); it also fires one bl_enqueue_action(BL_ACT_ATTACK, ...) per wake when
# an attack is ready (pickBestAttack, below). Single-gateway combat (V5):
# combat_preempt is GONE outright -- engagement (the Attack intention's
# approach) and the swing (this brain's own BL_ACT_ATTACK) both arrive
# through the exact same apply_intention/resolve_action seams a monster's
# simple brain uses (monster_brain.cpp); there is no second, host-level
# combat path running alongside this one anymore.
#
# V6 (Shoot-restate livelock fix): the SAME bl_enqueue_action channel also
# fires from a hunt wake now (actHunt's BL_INT_SHOOT suggestion, below) --
# apply_intention's Shoot case (intention.cpp) stopped pushing its own
# one-shot swing at adoption, so a prey that survived the opening arrow
# needs this brain to keep firing at it every wake it restates Shoot,
# exactly like the combat branch already does for Attack. Separately (mid-
# chat wake fix): actChat (blocks.nim) now restates BL_INT_CHAT at the live
# partner while a session is running, instead of substituting BL_INT_IDLE --
# Idle is never an identical restatement (is_identical_restatement,
# intention.cpp), so it used to overwrite CurrentIntention mid-conversation
# and lose the session's completion tracking.
#
# WIRE-STABILITY (restate-resume is EXACT field equality, intention.cpp's
# is_identical_restatement -- no epsilon): every point this file echoes back
# on a MoveTo suggestion is read STRAIGHT off the wire (roamGoal/exploreGoal,
# hero_view.nim), never recomputed here, so restating is naturally safe AS
# LONG AS the host hands back the identical float it did last wake for an
# unchanged goal. That holds for roamGoal (hero_perception.cpp's roam_point is
# a pure function of (slot, roam_epoch, anchor, radius) -- no dependence on
# this hero's current, possibly-mid-walk position) but NOT for exploreGoal
# (pick_exploration_target, exploration.cpp, filters candidate frontier
# texels by distance from the hero's CURRENT position -- which drifts every
# wake while walking toward a still-open goal within the same explore_epoch,
# so a later wake's reservoir sample can legitimately land on a different
# texel even with the epoch-derived seed unchanged). This file has no fix
# available within its own scope (it cannot recompute what it never computed,
# and the host recomputation lives in hero_perception.cpp/exploration.cpp,
# outside this task's file list) -- flagged here, and in the task report, per
# the brief's instruction to note rather than silently patch the engine's
# comparison to an epsilon. In practice Explore's restate can occasionally
# fail to dedupe (one extra MoveTo where a truly-identical goal would have
# resumed) -- never a correctness bug (the new goal is still valid), just
# occasional avoidable log/replay churn.
#
# ABI boilerplate (buffers, bl_abi_version/bl_spawn/bl_despawn/bl_view_buf/
# bl_out_buf/bl_tick, NimMain/bl_log imports) lives in brain_scaffold.nim --
# see its CONTRACT comment.
#
# Compiled to wasm32-wasi via scripts/build_brains.sh; must import at most
# env.bl_log (enforced by src/crates/brainhost's bh_instantiate) -- so no
# echo/io/os module usage anywhere in this file or its imports.

import abi
import activity_catalog
import hero_view
import blocks
import selectors
import rng

include brain_scaffold

# bl_enqueue_action: the v3 action channel (brainhost.h's BhActionFn side;
# brain_abi.h's BL_ACT_* vocabulary). Same "env" import-module convention as
# brain_scaffold's bl_log. Declared here (not brain_scaffold.nim) because only
# the hero brain will ever call it -- idle_test.nim/trap_test.nim stay fixed-
# decision fixtures with no reason to. Called from a combat wake below
# (pickBestAttack + brainTick): at most ONCE per wake (the soft one-action
# convention resolve_action, game/src/intention.h, documents but does not
# itself enforce).
proc bl_enqueue_action(kind: int32; target: uint32; arg: int32) {.importc, cdecl.}

# badlands::AttackCategory (game/include/badlands_sim.hpp): Melee=0, Ranged=1.
# Not part of the wire vocabulary proper -- brain_abi.h deliberately excludes
# badlands_sim.hpp (its own file comment explains why) -- so mirrored here,
# locally, the one value this file's picker needs, the same discipline abi.nim
# uses for the wire structs themselves: keep in sync by hand, comment says so.
const kAttackCategoryRanged: int32 = 1

# BL_ACT_ATTACK picker (docs/superpowers/specs/2026-07-25-contract-v3-
# alignment-design.md §4): highest base_damage among this hero's attacks that
# are off cooldown, legal under the current melee lock (no Ranged while
# locked), and -- when the caller knows a distance to gate on -- within reach
# of it. Returns -1 if none qualifies (the caller enqueues nothing that wake).
# `hasDist=false` (melee-locked with no threat actually in view, an edge case
# BL_ST_MELEE_LOCKED alone can produce) skips the range gate rather than
# guessing: resolve_action (game/src/intention.h) re-validates range against
# the live, authoritatively-resolved target anyway, so a locally-unknown
# distance is never a reason to refuse trying.
proc pickBestAttack(v: HeroView, hasDist: bool, dist: float32): int32 =
  result = -1
  var bestDamage = -1.0'f32
  for i in 0 ..< v.attackCount:
    let a = v.attacks[i]
    if a.cooldown_remaining > 0.0'f32:
      continue
    if v.meleeLocked and a.category == kAttackCategoryRanged:
      continue
    if hasDist and a.range < dist:
      continue
    if a.base_damage > bestDamage:
      bestDamage = a.base_damage
      result = i.int32

# The skill picker's twin of pickBestAttack: the SLOT INDEX of `wantedId` in
# this hero's own skill list when it is ready to fire right now, or -1. Slot
# index, not id: that index is exactly what bl_enqueue_action names as its
# `arg` (brain_abi.h's BlViewSkill doc), and the two must not be confused.
# Trigger is checked here as well as host-side, so a class that is later
# authored a passive of the same name never gets it fired as an action.
proc readySkillSlot(v: HeroView, wantedId: int32): int32 =
  result = -1
  for i in 0 ..< v.skillCount:
    let s = v.skills[i]
    if s.skill_id == wantedId and s.ready != 0'u32 and
       s.trigger == BL_SKILL_TRIGGER_ACTION:
      return i.int32

# Longest reach among this hero's melee attacks -- the reach a melee-tested
# skill (ShieldBash) is gated on host-side (skill_cast.h's skill_cast_range),
# mirrored here so the brain does not spend a wake asking for a cast the
# engine will refuse.
proc meleeReach(v: HeroView): float32 =
  result = 0.0'f32
  for i in 0 ..< v.attackCount:
    let a = v.attacks[i]
    if a.category != kAttackCategoryRanged and a.range > result:
      result = a.range

# Floor for the re-fire hint below: never ask for a re-consult sooner than
# this, even if an attack's own cooldown_remaining is shorter (a wake still
# costs a host round-trip).
const kMinShootRefireMillis: int64 = 100

# Smallest cooldown_remaining (ms) among this hero's attacks that are LEGAL
# under the current melee lock (the same category gate pickBestAttack uses
# above) -- how soon a hunt-wake retry could actually land a shot, floored at
# kMinShootRefireMillis. -1 when no attack qualifies (nothing legal to wait
# on; the caller falls back to the ordinary idle hint instead).
proc minLegalCooldownMillis(v: HeroView): int64 =
  result = -1'i64
  for i in 0 ..< v.attackCount:
    let a = v.attacks[i]
    if v.meleeLocked and a.category == kAttackCategoryRanged:
      continue
    let ms = (a.cooldown_remaining * 1000.0'f32).int64
    if result < 0 or ms < result:
      result = ms
  if result >= 0 and result < kMinShootRefireMillis:
    result = kMinShootRefireMillis

# EVERY hero class runs this one table (the now-deleted town_brain.cpp's own
# comment: "there is no per-class list" -- what a class does, how eagerly,
# and whether it has an activity at all is entirely the weight table). List
# order matched town_brain.cpp's kHeroActivities exactly when this table was
# ported from it -- it is the tie-break.
const kHeroActivities = [
  ActivityEntry(id: ActExplore, band: bNormal, score: scoreExplore, act: actExplore),
  ActivityEntry(id: ActGoHome, band: bNormal, score: scoreGoHome, act: actGoHome),
  ActivityEntry(id: ActHunt, band: bNormal, score: scoreHunt, act: actHunt),
  ActivityEntry(id: ActBuy, band: bNormal, score: scoreBuy, act: actBuy),
  ActivityEntry(id: ActVisitTavern, band: bNormal, score: scoreVisitTavern, act: actVisitTavern),
  ActivityEntry(id: ActChat, band: bNormal, score: scoreChat, act: actChat),
  ActivityEntry(id: ActRoam, band: bNormal, score: scoreRoam, act: actRoam),
  ActivityEntry(id: ActIdle, band: bNormal, score: scoreIdle, act: actIdle),
]

# The idle hint's bounds (ms): replaces the wire's v1 think_min_millis/
# think_max_millis (BlViewFactors) -- deliberation is gone, so this is a
# compiled constant now, not a tunable factor (CLAUDE.md: fixed constants
# until a knob is asked for). Same order of magnitude as the shipped v1
# defaults (0..833ms). Scheduling advice only ("you don't need me for X
# ms"), never a promise -- a spurious wake is always tolerated.
const
  kIdleHintMinMillis: int64 = 500
  kIdleHintMaxMillis: int64 = 2000

proc brainInit() =
  const msg: cstring = "hero brain v3 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

# True if the wire's inbox carries a guaranteed-wake danger signal
# (DamageTaken or ThreatSighted), checked regardless of age/TTL -- the inbox
# is sticky by design (a hero waking late still sees what it missed).
proc hasDangerEvent(w: BlViewWire): bool =
  for i in 0 ..< w.event_count:
    let kind = w.events[i].kind
    if kind == BL_EV_DAMAGE_TAKEN.uint32 or kind == BL_EV_THREAT_SIGHTED.uint32:
      return true
  false

proc brainTick(slot: int32): int32 =
  if g_view_buf.version != BL_ABI_VERSION.uint32:
    return 1

  let v = viewFromWire(g_view_buf)

  # Combat wake: a threat in view OR an active melee lock (BL_ST_MELEE_LOCKED
  # -- the lock can outlast the attacker briefly stepping out of view). Keep/
  # restate BL_INT_ATTACK (actor-only: melee whatever this hero is already
  # engaged with, apply_intention's own doc comment, game/src/intention.cpp)
  # and fire at most ONE BL_ACT_ATTACK this wake via pickBestAttack (above) --
  # the soft one-action convention: exactly one bl_enqueue_action call when an
  # attack qualifies, none when it doesn't (resolve_action, game/src/
  # intention.h, tolerates more per wake but nothing here ever sends more).
  let inCombat = v.hasThreat or v.meleeLocked
  let chosen =
    if inCombat:
      # target_slot on the SUGGESTION itself is inspection-only for Attack --
      # apply_intention discards it (no distinguishing field,
      # is_identical_restatement, game/src/intention.cpp) -- so UINT32_MAX
      # when there is no directly-visible threat to name (locked-only wake)
      # is fine; it mirrors bl_enqueue_action's own "let the engine infer it"
      # sentinel below.
      let targetSlot = if v.hasThreat: v.threatSlot else: high(uint32)
      let best = pickBestAttack(v, v.hasThreat, v.threatDist)
      if best >= 0:
        bl_enqueue_action(BL_ACT_ATTACK, targetSlot, best)
      # Shield-bash (v4): a SECOND action this wake, not a replacement for the
      # swing -- the bash stamps only its own cooldown host-side, so a
      # mercenary that opens with it still swings the same tick. Only fired
      # at a threat actually in view (a locked-but-unseen opponent gives no
      # distance to gate on) and only inside melee reach, which is the same
      # gate validate_cast applies host-side.
      let bash = readySkillSlot(v, BL_SKILL_SHIELD_BASH)
      if bash >= 0 and v.hasThreat and v.threatDist <= meleeReach(v):
        bl_enqueue_action(BL_ACT_USE_SKILL, targetSlot, bash)
      Suggestion(kind: BL_INT_ATTACK, activityLabel: ActCombat, targetSlot: targetSlot)
    elif hasDangerEvent(g_view_buf) and v.hasHome:
      # A recent hit or sighting biases toward retreating home over the
      # normal selection -- but only when there IS a home to retreat to;
      # otherwise fall through to the table below, which still gives
      # GoHome/etc. a fair shot on their own merits.
      actGoHome(v, g_view_buf.factors)
    else:
      selectBanded(kHeroActivities, g_view_buf.factors.weights, v, g_view_buf.factors)

  # Hunt wake (Finding A: the Shoot-restate livelock fix, V6):
  # apply_intention's Shoot case (intention.cpp) went engagement/tracking-
  # only, so a Shoot suggestion no longer carries a swing of its own -- fire
  # at most ONE BL_ACT_ATTACK here, every wake this brain suggests Shoot,
  # mirroring the combat branch's own picker above. hasDist=true
  # unconditionally: actHunt (blocks.nim) only ever suggests Shoot once
  # preyDist is already within selfAttackRange, so the distance is always
  # known and meaningful here. When nothing qualifies (every legal attack
  # still cooling down), no action fires this wake, and refireHintMillis
  # asks for a re-consult closer to the actual cooldown (floored at
  # kMinShootRefireMillis) instead of the ordinary 0.5-2s idle hint below --
  # so the next shot does not wait out the full window.
  var refireHintMillis = -1'i64  # -1 = no override; the ordinary hint stands
  if chosen.kind == BL_INT_SHOOT:
    let best = pickBestAttack(v, true, v.preyDist)
    if best >= 0:
      bl_enqueue_action(BL_ACT_ATTACK, v.preySlot, best)
    else:
      refireHintMillis = minLegalCooldownMillis(v)

  # The idle hint: "you don't need me for X ms" -- the SAME draw doubles as
  # BL_INT_IDLE's own duration_millis (the pause IS the idle hint now) and as
  # idle_hint_millis for every other kind. BL_INT_NONE gets neither: it is
  # apply_intention's own "nothing suggested this wake" no-op (discarded
  # wholesale, CurrentIntention untouched) -- reachable today only via
  # selectBanded's vacuous all-weights-zero fallback (src/crates/brainhost's
  # real_hero_wasm_conforms test), which also asserts an all-zero-bytes
  # SuggestionWire back, so a hint here would break that acceptance test for
  # no behavioural gain (apply_intention never reads it for None anyway).
  var s = seedOf(v.slot, v.nowMillis)
  let drawnHint = rangeI64(s, kIdleHintMinMillis, kIdleHintMaxMillis)
  let hint = if refireHintMillis >= 0: refireHintMillis else: drawnHint
  let isIdle = chosen.kind == BL_INT_IDLE
  let isNone = chosen.kind == BL_INT_NONE

  g_out_buf = BlSuggestionWire(
    idle_hint_millis: (if isIdle or isNone: 0'i64 else: hint),
    duration_millis: (if isIdle: hint else: 0'i64),
    intention_kind: chosen.kind,
    activity_label: chosen.activityLabel,
    point_x: chosen.pointX,
    point_z: chosen.pointZ,
    target_slot: chosen.targetSlot,
    arg: chosen.arg,
  )
  0
