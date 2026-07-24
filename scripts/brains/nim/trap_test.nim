# Test-only fixture brain (game/tests/wasm_brain_tests.cpp's BhInstance
# reinstantiation coverage, Task 4's review fix): implements the same ABI
# surface as hero.nim, but bl_tick unconditionally traps the wasm guest with
# a genuine `unreachable` -- NOT a clean nonzero return (BH_ERR_SCRIPT) and
# NOT a fuel-exhausting loop (BH_ERR_FUEL) -- so brainhost reports
# BH_ERR_TRAP and the host's "a trapped BhInstance is not reused" path
# (src/crates/brainhost/include/brainhost.h) has a real module to exercise.
#
# ABI boilerplate (buffers, bl_abi_version/bl_spawn/bl_despawn/bl_view_buf/
# bl_out_buf/bl_tick, NimMain/bl_log imports) lives in brain_scaffold.nim --
# see its CONTRACT comment. This file is just the two hooks: brainInit (the
# init log line) and brainTick (the unconditional trap).
#
# Compiled to wasm32-wasi via scripts/build_brains.sh; must import at most
# env.bl_log (enforced by src/crates/brainhost's bh_instantiate) -- so no
# echo/io/os module usage anywhere in this file or its imports, same
# constraint as hero.nim.

import abi

include brain_scaffold

proc brainInit() =
  const msg: cstring = "trap brain v1 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

proc brainTick(slot: int32): int32 =
  # Deliberate, unconditional wasm trap. __builtin_trap() lowers to wasm's
  # `unreachable` instruction under clang/wasi-sdk -- a genuine guest crash,
  # not a Nim exception/panic (which would raise or exit, not trap) and not
  # an infinite loop (which would exhaust fuel instead, BH_ERR_FUEL).
  {.emit: "__builtin_trap();".}
  0  # unreachable at runtime; satisfies the proc's declared return type
