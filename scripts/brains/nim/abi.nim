# hand-mirrored from game/src/brain_abi.h -- keep in sync; both sides
# static-assert sizes.
#
# `{.packed.}` on every object below is deliberate, not decorative: the C
# header has ZERO implicit padding by construction (every gap the natural C
# alignment rules would otherwise insert is filled by an explicit `_pad*`
# field already present in the field list below, in the same position as in
# the header) -- so packing changes nothing versus Nim's own natural
# alignment EXCEPT that it removes any risk of this compiler's alignment
# rules disagreeing with C's. Field order and sizes below must match
# brain_abi.h exactly, field for field; see that header's "LAYOUT RULES"
# comment for the reasoning behind each `_pad*`.
#
# v2 (the intention contract): BL_CMD_*/BlDecisionWire are gone, replaced by
# BL_INT_*/BlSuggestionWire; BlViewWire gains statuses/events blocks and
# BlViewSelf gains a current-intention summary; BlViewFactors drops
# think_min_millis/think_max_millis (deliberation is gone).
#
# v3 (contract-v3-alignment): BlViewWire gains the attack-loadout block
# (BlViewAttack, after statuses) -- a brain cannot pick an attack it cannot
# see -- and a new write-only host import, bl_enqueue_action, for firing
# instant actions (BL_ACT_*) independently of the one suggestion a wake still
# returns.
#
# v4 (skills execution): BlViewWire gains the skills block (BlViewSkill, after
# attacks) -- same reasoning as the attacks block: a brain cannot use a skill
# it cannot see, and the WIRE INDEX of a skill is what it hands back as
# BL_ACT_USE_SKILL's `arg`. BL_ACT_USE_SKILL stops being reserved, and
# BL_ST_STUNNED joins the status vocabulary; v5 adds BL_ST_DISENGAGED plus the
# threat/standoff block on BlThreat and BlViewSelf.

const
  BL_ABI_VERSION* = 5'i32
  BL_MAX_THREATS* = 8
  BL_MAX_CHARS* = 16
  BL_MAX_EVENTS* = 8
  BL_MAX_STATUSES* = 8
  BL_MAX_ACTIVITIES* = 14
  BL_MAX_ATTACKS* = 3
  BL_MAX_SKILLS* = 8

  # BL_ACT_*: action kinds (append-only) -- bl_enqueue_action's `kind`
  # argument. Fire-and-forget: no return, validated by the engine at resolve
  # time, independently of the wake's own suggestion. Only BL_ACT_ATTACK is
  # live this slice.
  BL_ACT_NONE* = 0'i32
  BL_ACT_USE_SKILL* = 1'i32   # arg = SKILL SLOT index (into BlViewWire.skills),
                              # target = victim slot (or the caster for a self skill)
  BL_ACT_USE_POTION* = 2'i32  # reserved
  BL_ACT_ATTACK* = 3'i32      # arg = attack index, target = victim slot
                               # (UINT32_MAX = the current Attack intention's target)

  # BL_INT_*: intention kinds (append-only) -- the suggestion's `kind`.
  # Mirrors badlands::IntentionKind (game/src/components.h) 1:1 for 0..8;
  # BL_INT_USE_SKILL(9) has no IntentionKind counterpart yet (reserved,
  # rejected warn+ignore by the host until the skills slice). Never
  # renumber/reuse a shipped value.
  BL_INT_NONE* = 0'i32
  BL_INT_MOVE_TO* = 1'i32
  BL_INT_ATTACK* = 2'i32
  BL_INT_SHOOT* = 3'i32
  BL_INT_ENTER* = 4'i32       # arg = BuildingKind
  BL_INT_ENTER_HOME* = 5'i32
  BL_INT_BUY* = 6'i32
  BL_INT_CHAT* = 7'i32        # target_slot = chat partner slot
  BL_INT_IDLE* = 8'i32        # duration_millis = explicit idle-for-X
  BL_INT_USE_SKILL* = 9'i32   # reserved

  # BL_EV_*: event-inbox kinds (append-only), mirroring
  # badlands::InboxEventKind (game/src/components.h) 1:1. Every kind is also
  # a guaranteed-wake reason (docs/design/intention-contract.html §2).
  BL_EV_NONE* = 0'i32
  BL_EV_DAMAGE_TAKEN* = 1'i32
  BL_EV_THREAT_SIGHTED* = 2'i32
  BL_EV_MOVE_BLOCKED* = 3'i32
  BL_EV_INTENTION_ENDED* = 4'i32

  # BL_ST_*: status kinds (append-only). All three are advisory only this
  # slice -- none bypasses the think.
  BL_ST_NONE* = 0'i32
  BL_ST_CHATTING* = 1'i32
  BL_ST_MELEE_LOCKED* = 2'i32
  BL_ST_INSIDE_BUILDING* = 3'i32
  # Not advisory: a stunned entity is not consulted at all, so a brain only
  # sees this for a stun that ended before its wake.
  BL_ST_STUNNED* = 4'i32
  # Walked out of melee contact: no actions at all for a few seconds.
  BL_ST_DISENGAGED* = 5'i32
  BL_ST_CURSED* = 6'i32
  BL_ST_SNEAKING* = 7'i32
  BL_ST_CALCIFIED* = 8'i32

  # badlands::SkillTrigger (game/include/badlands_sim.hpp), mirrored here
  # because brain_abi.h deliberately excludes that header -- the same
  # hand-sync discipline every other vocabulary in this file follows. Only an
  # Action skill can be fired through bl_enqueue_action.
  BL_SKILL_TRIGGER_ACTION* = 0'i32
  BL_SKILL_TRIGGER_PASSIVE* = 1'i32
  BL_SKILL_TRIGGER_INTENTION* = 2'i32

  # badlands::SkillId, same discipline. Only the ids a shipping brain names.
  BL_SKILL_CALCIFY* = 0'i32
  BL_SKILL_SHIELD_BASH* = 1'i32
  BL_SKILL_CURSE* = 2'i32
  BL_SKILL_DRESS_WOUNDS* = 3'i32
  BL_SKILL_BACKSTAB* = 4'i32
  BL_SKILL_SNEAK* = 5'i32

type
  BlViewSelf* {.packed.} = object
    world_millis*: int64
    think_until_millis*: int64
    roam_epoch*: int64
    intention_wake_at*: int64
    slot*: uint32
    class_id*: int32
    tod*: float32
    night*: uint32
    pos_x*: float32
    pos_z*: float32
    health_frac*: float32
    fatigue*: float32
    content*: float32
    inventory*: int32
    attack_range*: float32
    current_activity*: int32
    intention_kind*: int32
    # v5 (was pad2, so the size is unchanged): this hero's own combat
    # potential. Compare against BlThreat.threat to decide whether a fight is
    # worth taking -- see game/src/threat_table.h.
    threat*: float32

  BlThreat* {.packed.} = object
    pos_x*: float32
    pos_z*: float32
    dist*: float32
    slot*: uint32
    # v5 standoff block: what this hostile can reach, how fast it closes, and
    # what it is worth. Hand-synced with game/src/brain_abi.h.
    reach*: float32
    ranged_reach*: float32
    move_speed*: float32
    threat*: float32

  BlViewSuggest* {.packed.} = object
    roam_goal_x*: float32
    roam_goal_z*: float32
    explore_goal_x*: float32
    explore_goal_z*: float32
    has_explore_goal*: uint32
    move_blocked*: uint32
    blocked_x*: float32
    blocked_z*: float32
    partner_x*: float32
    partner_z*: float32
    partner_dist*: float32
    partner_slot*: uint32
    has_chat_partner*: uint32
    chatting*: uint32
    prey_x*: float32
    prey_z*: float32
    prey_dist*: float32
    prey_slot*: uint32
    has_prey*: uint32
    home_x*: float32
    home_z*: float32
    has_home*: uint32
    apothecary_x*: float32
    apothecary_z*: float32
    has_apothecary*: uint32
    tavern_x*: float32
    tavern_z*: float32
    has_tavern*: uint32
    threat_count*: int32
    pad0*: uint32  # see brain_abi.h: keeps `threats`/the struct size 8-aligned
    threats*: array[BL_MAX_THREATS, BlThreat]

  BlViewFactors* {.packed.} = object
    weights*: array[BL_MAX_ACTIVITIES, float32]
    fatigue_seek*: float32          # read by score_go_home
    fatigue_seek_night*: float32    # read by score_go_home
    low_health_rest*: float32       # read by score_go_home
    content_seek*: float32          # read by score_visit_tavern
    chat_content_seek*: float32     # read by score_chat
    chat_radius*: float32           # read by act_chat
    explore_min_fatigue*: float32   # read by score_explore
    entrance_radius*: float32       # mirrors components.h's kEntranceRadius;
                                     # see brain_abi.h's BlViewFactors comment

  BlViewChar* {.packed.} = object
    last_seen_millis*: int64
    slot*: uint32
    archetype*: int32
    team*: int32
    last_x*: float32
    last_z*: float32
    last_hp*: float32
    visible_now*: uint32
    pad0*: uint32

  BlStatus* {.packed.} = object
    remaining_millis*: int64   # 0 = indefinite
    kind*: uint32
    pad0*: uint32

  BlViewAttack* {.packed.} = object
    category*: int32       # badlands::AttackCategory
    damage_type*: int32    # badlands::DamageType
    base_damage*: float32
    range*: float32
    cooldown_remaining*: float32  # seconds; 0 = ready
    pad0*: uint32

  BlViewSkill* {.packed.} = object
    skill_id*: int32            # badlands::SkillId
    cooldown_remaining*: float32  # seconds; 0 = ready
    ready*: uint32              # bool: off cooldown
    recommended*: uint32        # bool: the host's trigger advice
    trigger*: int32             # badlands::SkillTrigger
    target_mode*: int32         # badlands::SkillTargetMode

  BlEvent* {.packed.} = object
    at_millis*: int64
    ttl_millis*: int64
    kind*: uint32
    source_slot*: uint32
    param*: float32
    pad0*: uint32

  BlViewWire* {.packed.} = object
    version*: uint32
    pad0*: uint32
    self*: BlViewSelf
    suggest*: BlViewSuggest
    factors*: BlViewFactors
    status_count*: int32
    pad1*: uint32
    statuses*: array[BL_MAX_STATUSES, BlStatus]
    attack_count*: int32
    pad4*: uint32
    attacks*: array[BL_MAX_ATTACKS, BlViewAttack]
    skill_count*: int32
    pad5*: uint32
    skills*: array[BL_MAX_SKILLS, BlViewSkill]
    event_count*: int32
    pad2*: uint32
    events*: array[BL_MAX_EVENTS, BlEvent]
    char_count*: int32
    pad3*: uint32
    chars*: array[BL_MAX_CHARS, BlViewChar]

  BlSuggestionWire* {.packed.} = object
    idle_hint_millis*: int64    # 0 = none
    duration_millis*: int64     # BL_INT_IDLE only
    intention_kind*: int32      # BL_INT_*
    activity_label*: int32      # ActivityId, inspection only
    point_x*, point_z*: float32
    target_slot*: uint32
    arg*: int32                 # building kind for ENTER, etc.

static: doAssert sizeof(BlViewSelf) == 88
static: doAssert sizeof(BlThreat) == 32
static: doAssert sizeof(BlViewSuggest) == 376
static: doAssert sizeof(BlViewFactors) == 88
static: doAssert sizeof(BlViewChar) == 40
static: doAssert sizeof(BlStatus) == 16
static: doAssert sizeof(BlViewAttack) == 24
static: doAssert sizeof(BlViewSkill) == 24
static: doAssert sizeof(BlEvent) == 32
static: doAssert sizeof(BlViewWire) == 1888
static: doAssert sizeof(BlSuggestionWire) == 40
