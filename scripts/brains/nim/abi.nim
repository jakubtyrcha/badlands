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

const
  BL_ABI_VERSION* = 1'i32
  BL_MAX_THREATS* = 8
  BL_MAX_CHARS* = 16
  BL_MAX_ACTIVITIES* = 14

  # BL_CMD_*: the command kinds a brain's decision can request, ABI version 1
  # (BL_ABI_VERSION above). This list -- values, meanings, and the
  # command_arg convention each one carries -- is kept in sync BY HAND across
  # this file and game/src/brain_abi.h's own "#define BL_CMD_*" block; the
  # wire STRUCT layout is what's frozen/asserted by sizeof (see the header's
  # own "LAYOUT RULES" comment), not this enum-like list, which may grow
  # (never renumber/reuse, same append-only discipline as badlands::ActivityId).
  BL_CMD_NONE* = 0'i32
  BL_CMD_ATTACK* = 1'i32
  BL_CMD_BUY* = 2'i32
  BL_CMD_ENTER* = 3'i32       # arg = BuildingKind
  BL_CMD_ENTER_HOME* = 4'i32
  # Task 5 extensions (game/src/brain_abi.h): act_hunt/act_chat's follow-ups
  # (CommandKind::Shoot/Chat) have no wire representation among the original
  # four -- both need a TARGET slot, which BL_CMD_ATTACK/ENTER_HOME/BUY do
  # not carry. arg = target slot (prey for SHOOT, chat partner for CHAT).
  # Accepted by controller adjudication as compatible with "abi.nim unchanged"
  # (task-5-brief.md): that constraint is about the wire STRUCT layout, not
  # this command-kind list -- decode_command's extension was explicitly
  # authorized, and command_arg was already a generic int32 slot, so this is
  # a new MEANING for an existing field, not a layout change.
  BL_CMD_SHOOT* = 5'i32
  BL_CMD_CHAT* = 6'i32

type
  BlViewSelf* {.packed.} = object
    world_millis*: int64
    think_until_millis*: int64
    roam_epoch*: int64
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
    think_min_millis*: int64   # read by deliberate
    think_max_millis*: int64   # read by deliberate
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

  BlViewWire* {.packed.} = object
    version*: uint32
    pad0*: uint32
    self*: BlViewSelf
    suggest*: BlViewSuggest
    factors*: BlViewFactors
    char_count*: int32
    pad1*: uint32
    chars*: array[BL_MAX_CHARS, BlViewChar]

  BlDecisionWire* {.packed.} = object
    pause_duration_millis*: int64
    activity_id*: int32
    goal_kind*: int32   # 0 none, 1 point
    goal_x*: float32
    goal_z*: float32
    command_kind*: int32
    command_arg*: int32
    follow_up_on_arrival*: uint32
    pause_kind*: uint32  # 0 none, 1 start, 2 continue

static: doAssert sizeof(BlViewSelf) == 72
static: doAssert sizeof(BlThreat) == 16
static: doAssert sizeof(BlViewSuggest) == 248
static: doAssert sizeof(BlViewFactors) == 104
static: doAssert sizeof(BlViewChar) == 40
static: doAssert sizeof(BlViewWire) == 1080
static: doAssert sizeof(BlDecisionWire) == 40
