# The shipping hero brain: a faithful port of the C++ reference decision layer
# (game/src/town_brain.cpp's town_think, game/src/behaviours/{blocks,selectors,
# deliberation}.cpp) to Nim/wasm. This file wires the ported pieces together
# exactly the way town_think does: unpack the view, select_banded over the
# hero activity table, deliberate on the choice, write the decision. The
# per-tick view/decision plumbing (perception, and applying what comes back)
# stays entirely host-side (game/src/wasm_brain.cpp's pack_view_wire /
# decode_decision / apply_brain_decision) -- this module only ever sees a
# BlViewWire and only ever produces a BlDecisionWire.
#
# Compiled to wasm32-wasi via scripts/build_brains.sh; must import at most
# env.bl_log (enforced by src/crates/brainhost's bh_instantiate) -- so no
# echo/io/os module usage anywhere in this file or its imports.

import abi
import activity_catalog
import hero_view
import blocks
import selectors
import deliberation

# Nim's own entry point, normally called by a generated C main() -- our build
# is --nomain (no host to call it for us), so bl_init calls it once, exactly
# as a normal Nim program's startup would, before any other exported proc
# runs.
proc NimMain() {.importc, cdecl.}

# The one host import a brain may make (env.bl_log; the "env" module name is
# wasm-ld's default for an undefined symbol with --allow-undefined and no
# explicit import-module attribute -- see brainhost.h's contract).
proc bl_log(level: int32, msg_ptr: int32, len: int32) {.importc, cdecl.}

var g_view_buf: BlViewWire
var g_out_buf: BlDecisionWire

# EVERY hero class runs this one table (town_brain.cpp's own comment: "there
# is no per-class list" -- what a class does, how eagerly, and whether it has
# an activity at all is entirely the weight table). List order matches
# kHeroActivities (town_brain.cpp:224-233) exactly -- it is the tie-break.
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

proc bl_abi_version*(): int32 {.exportc, cdecl.} =
  BL_ABI_VERSION

proc bl_init*(world_seed: int32) {.exportc, cdecl.} =
  NimMain()
  const msg: cstring = "hero brain v1 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

proc bl_spawn*(slot: int32, cls: int32, seed: int32) {.exportc, cdecl.} =
  discard  # no per-hero state: every decision is a pure function of the view

proc bl_despawn*(slot: int32) {.exportc, cdecl.} =
  discard

proc bl_view_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_view_buf.addr)

proc bl_out_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_out_buf.addr)

proc bl_tick*(slot: int32): int32 {.exportc, cdecl.} =
  if g_view_buf.version != BL_ABI_VERSION.uint32:
    return 1

  let v = viewFromWire(g_view_buf)
  let chosen = selectBanded(kHeroActivities, g_view_buf.factors.weights, v, g_view_buf.factors)
  let think = deliberate(chosen.activityId, v, g_view_buf.factors)

  # Mirrors town_think's tail: the commit fields (activity/goal/command) are
  # always filled from the selection, exactly as BrainDecision always carries
  # decision.activity = r.id regardless of think.pause; apply_brain_decision
  # (host side) only reads them when pause_kind == 0, so overlaying the pause
  # fields on top is equivalent to C++'s separate `decision.pause = think.pause`
  # assignment.
  g_out_buf = BlDecisionWire(
    pause_duration_millis: think.durationMillis,
    activity_id: chosen.activityId,
    goal_kind: chosen.goalKind,
    goal_x: chosen.goalX,
    goal_z: chosen.goalZ,
    command_kind: chosen.commandKind,
    command_arg: chosen.commandArg,
    follow_up_on_arrival: (if chosen.followUpOnArrival: 1'u32 else: 0'u32),
    pause_kind: (if not think.pause: 0'u32 elif think.durationMillis > 0: 1'u32 else: 2'u32),
  )
  0
