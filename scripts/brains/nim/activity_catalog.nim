# The shared goal vocabulary (badlands::ActivityId, game/include/badlands_sim.hpp)
# and its band mapping (badlands::ActivityBand, filled by
# game/src/activity_catalog.cpp's kCatalog) -- copied here because the wire
# does not carry bands (see brain_abi.h: BlViewFactors ships weights only).
#
# KEEP IN SYNC with badlands_sim.hpp's `enum class ActivityId` and
# activity_catalog.cpp's `kCatalog`: the id values are the shared, APPEND-ONLY
# id space (command log / snapshot / histogram / this brain all speak it), and
# the band of each id below must match kCatalog's row for the same id exactly.
# BL_MAX_ACTIVITIES (abi.nim) already pins the id space's size.

# ActivityId values, as plain int32 (not a Nim enum) so they compare directly
# against BlViewWire/BlDecisionWire's int32 fields with no conversion.
const
  ActIdle*: int32 = 0
  ActRoam*: int32 = 1
  ActBuy*: int32 = 2
  ActGoHome*: int32 = 3
  ActVisitTavern*: int32 = 4
  ActCombat*: int32 = 5
  ActGraze*: int32 = 6
  ActVisitTax*: int32 = 7
  ActDeposit*: int32 = 8
  ActHunt*: int32 = 9
  ActFlee*: int32 = 10
  ActThink*: int32 = 11
  ActExplore*: int32 = 12
  ActChat*: int32 = 13
  ActCount*: int32 = 14

type Band* = enum
  bDanger = 0
  bNormal = 1

# Indexed by ActivityId, exactly like kCatalog. Only Combat/Flee are Danger;
# everything else -- including every activity a hero brain can pick -- is
# Normal. (See activity_catalog.cpp's own comment on why there are only two
# bands, and why sorting further would be a mistake.)
const kBandOf*: array[ActCount, Band] = [
  bNormal,  # Idle
  bNormal,  # Roam
  bNormal,  # Buy
  bNormal,  # GoHome
  bNormal,  # VisitTavern
  bDanger,  # Combat
  bNormal,  # Graze
  bNormal,  # VisitTax
  bNormal,  # Deposit
  bNormal,  # Hunt
  bDanger,  # Flee
  bNormal,  # Think
  bNormal,  # Explore
  bNormal,  # Chat
]

# Mirrors ActivityInfoOf's out-of-range fallback: resolves to the Idle row
# (Normal), never an out-of-bounds read.
proc bandOf*(id: int32): Band =
  if id < 0 or id >= ActCount:
    bNormal
  else:
    kBandOf[id]
