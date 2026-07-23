# Test-only fixture brain: implements the same ABI surface as hero.nim, but
# bl_tick always decides Idle/no-goal/no-command/no-pause, unconditionally.
# This USED to be hero.nim itself (the skeleton before Task 5's real decision
# port); every existing test that asserted all-Idle behaviour now targets this
# fixture instead of the shipping assets/brains/hero.wasm, which makes real
# decisions.
#
# Compiled to wasm32-wasi via scripts/build_brains.sh (build_one) ->
# game/tests/fixtures/idle_brain.wasm; must import at most env.bl_log
# (enforced by src/crates/brainhost's bh_instantiate) -- so no echo/io/os
# module usage anywhere in this file or its imports, same constraint as
# hero.nim.

import abi

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

proc bl_abi_version*(): int32 {.exportc, cdecl.} =
  BL_ABI_VERSION

proc bl_init*(world_seed: int32) {.exportc, cdecl.} =
  NimMain()
  const msg: cstring = "idle test brain v1 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

proc bl_spawn*(slot: int32, cls: int32, seed: int32) {.exportc, cdecl.} =
  discard  # no per-hero state needed

proc bl_despawn*(slot: int32) {.exportc, cdecl.} =
  discard

proc bl_view_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_view_buf.addr)

proc bl_out_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_out_buf.addr)

proc bl_tick*(slot: int32): int32 {.exportc, cdecl.} =
  if g_view_buf.version != BL_ABI_VERSION.uint32:
    return 1
  g_out_buf = BlDecisionWire(
    pause_duration_millis: 0,
    activity_id: 0,  # ActivityId::Idle
    goal_kind: 0,    # none
    goal_x: 0.0,
    goal_z: 0.0,
    command_kind: BL_CMD_NONE,
    command_arg: 0,
    follow_up_on_arrival: 0,
    pause_kind: 0,
  )
  0
