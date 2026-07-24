# Test-only fixture brain: implements the same ABI surface as hero.nim, but
# bl_tick always decides Idle/no-goal/no-command/no-pause, unconditionally.
# This USED to be hero.nim itself (the skeleton before Task 5's real decision
# port); every existing test that asserted all-Idle behaviour now targets this
# fixture instead of the shipping assets/brains/hero.wasm, which makes real
# decisions.
#
# ABI boilerplate (buffers, bl_abi_version/bl_spawn/bl_despawn/bl_view_buf/
# bl_out_buf/bl_tick, NimMain/bl_log imports) lives in brain_scaffold.nim --
# see its CONTRACT comment. This file is just the two hooks: brainInit (the
# init log line) and brainTick (the fixed all-Idle decision).
#
# Compiled to wasm32-wasi via scripts/build_brains.sh (build_one) ->
# game/tests/fixtures/idle_brain.wasm; must import at most env.bl_log
# (enforced by src/crates/brainhost's bh_instantiate) -- so no echo/io/os
# module usage anywhere in this file or its imports, same constraint as
# hero.nim.

import abi

include brain_scaffold

proc brainInit() =
  const msg: cstring = "idle test brain v1 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

proc brainTick(slot: int32): int32 =
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
