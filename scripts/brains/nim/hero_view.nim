# The wasm-side view (game/src/behaviours/world_view.h's WorldView, plus the
# v2 current-intention summary): the subset of fields the hero blocks/
# selector actually read, unpacked from BlViewWire once per wake. Field-for-
# field the inverse of wasm_brain.cpp's pack_view_wire -- read that function
# alongside this one when reviewing.
#
# Two things a C++ WorldView carries that this deliberately drops: the
# townfolk/critter-only fields (tax target, deposit, grazing) never appear on
# the wire (a hero brain never perceives them, brain_abi.h says so), and the
# full 8-deep threat list collapses to the nearest one -- the slot/dist react()
# actually consults (nearest-first, per BlThreat's own doc, brain_abi.h). Also
# dropped versus v1: thinkUntilMillis (HeroSimulationState's own deliberation
# pause -- unrelated to the intention contract, and nothing on this path reads
# it anymore now that deliberation.nim is gone).
#
# v3 (contract-v3-alignment): gains meleeLocked (BL_ST_MELEE_LOCKED, scanned
# out of the wire's statuses block), the attack loadout (attackCount/
# attacks, copied straight off BlViewWire.attacks -- a brain cannot pick an
# attack it cannot see, brain_abi.h's BlViewAttack doc) -- both read by
# hero.nim's own BL_ACT_ATTACK picker, not by any blocks.nim score_*/act_*.
#
# v4 (skills execution): gains the skill loadout (skillCount/skills), copied
# in WIRE ORDER for the same reason -- the index is the handle
# (BL_ACT_USE_SKILL's arg), so it is never re-sorted or filtered here.

import abi

type
  Vec2* = object
    x*, z*: float32

  HeroView* = object
    slot*: uint32
    pos*: Vec2

    # needs
    fatigue*, content*, healthFrac*: float32
    inventory*: int32

    # clock
    nowMillis*: int64
    night*: bool

    # wander goal (drawn host-side; the block just walks to it)
    roamGoal*: Vec2

    # buildings
    hasHome*: bool
    homeDoor*: Vec2
    hasApothecary*: bool
    apothecaryDoor*: Vec2
    hasTavern*: bool
    tavernDoor*: Vec2

    # threats: collapsed to the nearest one -- the bit react() needs (is
    # there one at all) plus its slot/dist, for BL_INT_ATTACK's target and
    # the attack-range gate (v3).
    hasThreat*: bool
    threatSlot*: uint32
    threatDist*: float32
    threatPos*: Vec2
    # v5: what the nearest threat can reach, how fast it closes, and what it is
    # worth -- everything a standoff decision needs. Choosing a distance is
    # TACTICS, so it happens here in the brain; the engine grows no policy.
    threatReach*: float32
    threatRangedReach*: float32
    threatSpeed*: float32
    threatThreat*: float32
    # This hero's own combat potential, the other half of that comparison.
    selfThreat*: float32

    # combat (v3): advisory melee-lock status + this entity's own attack
    # loadout (a brain cannot pick an attack it cannot see).
    meleeLocked*: bool
    attackCount*: int32
    attacks*: array[BL_MAX_ATTACKS, BlViewAttack]

    # skills (v4): this hero's learned skills, in WIRE ORDER -- the index into
    # this array is what bl_enqueue_action(BL_ACT_USE_SKILL, ...) names as its
    # `arg`, so it must never be re-sorted or filtered on the way in.
    skillCount*: int32
    skills*: array[BL_MAX_SKILLS, BlViewSkill]

    # exploration
    hasExploreGoal*: bool
    exploreGoal*: Vec2
    moveBlocked*: bool

    # chat
    chatting*: bool
    hasChatPartner*: bool
    partnerPos*: Vec2
    partnerSlot*: uint32
    partnerDist*: float32

    # hunt
    hasPrey*: bool
    preyPos*: Vec2
    preySlot*: uint32
    preyDist*: float32
    selfAttackRange*: float32

    # current-intention summary (v2): what the engine is executing for this
    # hero right now, if anything -- read by react() to know whether it is
    # already mid-MoveTo/etc. on a spurious wake.
    currentActivity*: int32       # ActivityId this entity is doing now; -1 = none yet
    intentionKind*: int32         # BL_INT_*; BL_INT_NONE = nothing running
    intentionWakeAt*: int64       # CurrentIntention.wake_at_millis; 0 = no deadline

proc viewFromWire*(w: BlViewWire): HeroView =
  result.slot = w.self.slot
  result.pos = Vec2(x: w.self.pos_x, z: w.self.pos_z)
  result.fatigue = w.self.fatigue
  result.content = w.self.content
  result.healthFrac = w.self.health_frac
  result.inventory = w.self.inventory
  result.night = w.self.night != 0'u32
  result.nowMillis = w.self.world_millis
  result.currentActivity = w.self.current_activity
  result.intentionKind = w.self.intention_kind
  result.intentionWakeAt = w.self.intention_wake_at
  result.selfAttackRange = w.self.attack_range
  result.selfThreat = w.self.threat

  result.roamGoal = Vec2(x: w.suggest.roam_goal_x, z: w.suggest.roam_goal_z)
  result.hasExploreGoal = w.suggest.has_explore_goal != 0'u32
  result.exploreGoal = Vec2(x: w.suggest.explore_goal_x, z: w.suggest.explore_goal_z)
  result.moveBlocked = w.suggest.move_blocked != 0'u32
  result.hasChatPartner = w.suggest.has_chat_partner != 0'u32
  result.chatting = w.suggest.chatting != 0'u32
  result.partnerPos = Vec2(x: w.suggest.partner_x, z: w.suggest.partner_z)
  result.partnerDist = w.suggest.partner_dist
  result.partnerSlot = w.suggest.partner_slot
  result.hasPrey = w.suggest.has_prey != 0'u32
  result.preyPos = Vec2(x: w.suggest.prey_x, z: w.suggest.prey_z)
  result.preyDist = w.suggest.prey_dist
  result.preySlot = w.suggest.prey_slot
  result.hasHome = w.suggest.has_home != 0'u32
  result.homeDoor = Vec2(x: w.suggest.home_x, z: w.suggest.home_z)
  result.hasApothecary = w.suggest.has_apothecary != 0'u32
  result.apothecaryDoor = Vec2(x: w.suggest.apothecary_x, z: w.suggest.apothecary_z)
  result.hasTavern = w.suggest.has_tavern != 0'u32
  result.tavernDoor = Vec2(x: w.suggest.tavern_x, z: w.suggest.tavern_z)
  result.hasThreat = w.suggest.threat_count > 0'i32
  if result.hasThreat:
    result.threatSlot = w.suggest.threats[0].slot
    result.threatDist = w.suggest.threats[0].dist
    result.threatPos = Vec2(x: w.suggest.threats[0].pos_x, z: w.suggest.threats[0].pos_z)
    result.threatReach = w.suggest.threats[0].reach
    result.threatRangedReach = w.suggest.threats[0].ranged_reach
    result.threatSpeed = w.suggest.threats[0].move_speed
    result.threatThreat = w.suggest.threats[0].threat

  for i in 0 ..< w.status_count:
    if w.statuses[i].kind == BL_ST_MELEE_LOCKED.uint32:
      result.meleeLocked = true
      break

  result.attackCount = w.attack_count
  for i in 0 ..< w.attack_count:
    result.attacks[i] = w.attacks[i]

  result.skillCount = w.skill_count
  for i in 0 ..< w.skill_count:
    result.skills[i] = w.skills[i]
