# The wasm-side WorldView (game/src/behaviours/world_view.h): the subset of
# fields the hero blocks/selector/deliberation actually read, unpacked from
# BlViewWire once per tick. Field-for-field the inverse of wasm_brain.cpp's
# pack_view_wire -- read that function alongside this one when reviewing.
#
# Two things a C++ WorldView carries that this deliberately drops: the
# townfolk/critter-only fields (tax target, deposit, grazing) never appear on
# the wire (a hero brain never perceives them, brain_abi.h says so), and the
# full 8-deep threat list collapses to the one bit hero blocks/deliberate
# actually consult -- "is anything a threat" (has_threat(view) in C++).

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

    # threats: collapsed to the one bit deliberate() needs
    hasThreat*: bool

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

    # deliberation
    nowMillis*, thinkUntilMillis*: int64
    currentActivity*: int32

proc viewFromWire*(w: BlViewWire): HeroView =
  result.slot = w.self.slot
  result.pos = Vec2(x: w.self.pos_x, z: w.self.pos_z)
  result.fatigue = w.self.fatigue
  result.content = w.self.content
  result.healthFrac = w.self.health_frac
  result.inventory = w.self.inventory
  result.night = w.self.night != 0'u32
  result.nowMillis = w.self.world_millis
  result.thinkUntilMillis = w.self.think_until_millis
  result.currentActivity = w.self.current_activity
  result.selfAttackRange = w.self.attack_range

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
