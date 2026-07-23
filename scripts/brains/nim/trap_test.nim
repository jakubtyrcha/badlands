# Test-only fixture brain (game/tests/wasm_brain_tests.cpp's BhInstance
# reinstantiation coverage, Task 4's review fix): implements the same ABI
# surface as hero.nim, but bl_tick unconditionally traps the wasm guest with
# a genuine `unreachable` -- NOT a clean nonzero return (BH_ERR_SCRIPT) and
# NOT a fuel-exhausting loop (BH_ERR_FUEL) -- so brainhost reports
# BH_ERR_TRAP and the host's "a trapped BhInstance is not reused" path
# (src/crates/brainhost/include/brainhost.h) has a real module to exercise.
#
# Compiled to wasm32-wasi via scripts/build_brains.sh; must import at most
# env.bl_log (enforced by src/crates/brainhost's bh_instantiate) -- so no
# echo/io/os module usage anywhere in this file or its imports, same
# constraint as hero.nim.

import abi

# Nim's own entry point (see hero.nim's identical comment: --nomain means
# bl_init must call it once itself, exactly as a normal Nim program's
# startup would).
proc NimMain() {.importc, cdecl.}

# The one host import a brain may make (env.bl_log).
proc bl_log(level: int32, msg_ptr: int32, len: int32) {.importc, cdecl.}

var g_view_buf: BlViewWire
var g_out_buf: BlDecisionWire

proc bl_abi_version*(): int32 {.exportc, cdecl.} =
  BL_ABI_VERSION

proc bl_init*(world_seed: int32) {.exportc, cdecl.} =
  NimMain()
  const msg: cstring = "trap brain v1 init"
  bl_log(0'i32, cast[int32](msg), len(msg).int32)

proc bl_spawn*(slot: int32, cls: int32, seed: int32) {.exportc, cdecl.} =
  discard  # no per-hero state; init/spawn never trap, only tick does

proc bl_despawn*(slot: int32) {.exportc, cdecl.} =
  discard

proc bl_view_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_view_buf.addr)

proc bl_out_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_out_buf.addr)

proc bl_tick*(slot: int32): int32 {.exportc, cdecl.} =
  # Deliberate, unconditional wasm trap. __builtin_trap() lowers to wasm's
  # `unreachable` instruction under clang/wasi-sdk -- a genuine guest crash,
  # not a Nim exception/panic (which would raise or exit, not trap) and not
  # an infinite loop (which would exhaust fuel instead, BH_ERR_FUEL).
  {.emit: "__builtin_trap();".}
  0  # unreachable at runtime; satisfies the proc's declared return type
