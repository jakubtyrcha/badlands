# Test-only fixture brain (v2, the intention contract): implements the same
# ABI surface as hero.nim, but bl_tick always suggests BL_INT_IDLE (activity
# label ActivityId::Idle) with duration_millis 0 -- "idle until woken"
# (apply_intention's own sentinel: 0 is "no deadline", so this intention
# never self-completes and never spuriously re-wakes on a timer; see
# game/src/intention.h's CurrentIntention doc). Every existing test that
# asserts all-Idle behaviour targets this fixture instead of the shipping
# assets/brains/hero.wasm, which makes real decisions.
#
# ABI boilerplate (buffers, bl_abi_version/bl_spawn/bl_despawn/bl_view_buf/
# bl_out_buf/bl_tick, NimMain/bl_log imports) lives in brain_scaffold.nim --
# see its CONTRACT comment. This file is just the two hooks: brainInit (the
# init log line) and brainTick (the fixed all-Idle suggestion).
#
# Compiled to wasm32-wasi via scripts/build_brains.sh (build_one) ->
# game/tests/fixtures/idle_brain.wasm; must import at most env.bl_log
# (enforced by src/crates/brainhost's bh_instantiate) -- so no echo/io/os
# module usage anywhere in this file or its imports, same constraint as
# hero.nim.

import abi

include brain_scaffold

proc brainInit() =
  const msg: cstring = "idle test brain v2 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

proc brainTick(slot: int32): int32 =
  if g_view_buf.version != BL_ABI_VERSION.uint32:
    return 1
  g_out_buf = BlSuggestionWire(
    idle_hint_millis: 0,
    duration_millis: 0,       # 0 = idle until woken, never self-completes
    intention_kind: BL_INT_IDLE,
    activity_label: 0,        # ActivityId::Idle
    point_x: 0.0,
    point_z: 0.0,
    target_slot: 0,
    arg: 0,
  )
  0
