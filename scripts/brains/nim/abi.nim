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

const
  BL_ABI_VERSION* = 2'i32
  BL_MAX_THREATS* = 8
  BL_MAX_CHARS* = 16
  BL_MAX_EVENTS* = 8
  BL_MAX_STATUSES* = 8
  BL_MAX_ACTIVITIES* = 14

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
    pad2*: uint32

  BlThreat* {.packed.} = object
    pos_x*: float32
    pos_z*: float32
    dist*: float32
    slot*: uint32

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
    pad0*: uint32  # see brain_abi.h: rounds the struct to a multiple of 8

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
static: doAssert sizeof(BlThreat) == 16
static: doAssert sizeof(BlViewSuggest) == 248
static: doAssert sizeof(BlViewFactors) == 88
static: doAssert sizeof(BlViewChar) == 40
static: doAssert sizeof(BlStatus) == 16
static: doAssert sizeof(BlEvent) == 32
static: doAssert sizeof(BlViewWire) == 1480
static: doAssert sizeof(BlSuggestionWire) == 40
