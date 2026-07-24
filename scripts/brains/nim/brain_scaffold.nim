# The ~30 lines every brain (hero.nim / idle_test.nim / trap_test.nim) hand-
# synced identically before this file existed: the view/out buffers, the
# NimMain/bl_log imports, and every bl_* export except the two hooks a brain
# actually differs on. Factored out so the three brain files shrink to pure
# decision logic -- imports + their two hooks (+ hero's own supporting
# modules) -- with zero chance of the boilerplate drifting between them.
#
# CONTRACT (read this before editing a brain file):
#   1. `import abi` (this scaffold uses BlViewWire/BlDecisionWire/
#      BL_ABI_VERSION and assumes the including file already has them).
#   2. `include brain_scaffold` -- this is what declares g_view_buf/
#      g_out_buf and every bl_* export below, PLUS forward declarations for
#      brainInit()/brainTick(slot: int32): int32 (Nim requires forward decls
#      across an `include` boundary the same way C requires them across a
#      translation unit -- see bl_init/bl_tick's own comments below).
#   3. AFTER the include, define both hooks:
#        proc brainInit() = ...
#          Called once from bl_init, after NimMain() -- do whatever
#          one-time setup/logging this brain needs (a real body is
#          required; every shipping brain logs its own distinct init
#          line, so there is no useful "do nothing" default).
#        proc brainTick(slot: int32): int32 = ...
#          THIS BRAIN'S ENTIRE PER-TICK DECISION LOGIC, and the only part
#          that should ever differ between brains apart from brainInit's
#          message: read g_view_buf, write g_out_buf, return 0 (ok) or a
#          nonzero script error code -- exactly what bl_tick used to be,
#          body for body, before this scaffold existed.
#
# Compiled to wasm32-wasi via scripts/build_brains.sh; must import at most
# env.bl_log (enforced by src/crates/brainhost's bh_instantiate) -- so no
# echo/io/os module usage anywhere in this file, the brain files that include
# it, or their imports.

# Nim's own entry point, normally called by a generated C main() -- our build
# is --nomain (no host to call it for us), so bl_init calls it once, exactly
# as a normal Nim program's startup would, before any other exported proc
# runs.
proc NimMain() {.importc, cdecl.}

# The one host import a brain may make (env.bl_log; the "env" module name is
# wasm-ld's default for an undefined symbol with --allow-undefined and no
# explicit import-module attribute -- see brainhost.h's contract).
proc bl_log(level: int32, msg_ptr: int32, len: int32) {.importc, cdecl.}

# Buffer addresses are fixed for the instance's lifetime (src/crates/
# brainhost/include/brainhost.h, game/src/brain_abi.h): the host queries
# bl_view_buf()/bl_out_buf() ONCE, at instantiation, and reuses those
# addresses for every later tick -- ordinary global variables, exactly like
# this, satisfy that contract trivially.
var g_view_buf: BlViewWire
var g_out_buf: BlDecisionWire

proc brainInit()  # forward decl: the including file defines this, below its
                   # own `include brain_scaffold` line (see the CONTRACT above)
proc brainTick(slot: int32): int32  # forward decl: ditto -- this brain's decision logic

proc bl_abi_version*(): int32 {.exportc, cdecl.} =
  BL_ABI_VERSION

proc bl_init*(world_seed: int32) {.exportc, cdecl.} =
  NimMain()
  brainInit()

proc bl_spawn*(slot: int32, cls: int32, seed: int32) {.exportc, cdecl.} =
  discard  # no per-hero state today: every decision is a pure function of the view

proc bl_despawn*(slot: int32) {.exportc, cdecl.} =
  discard

proc bl_view_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_view_buf.addr)

proc bl_out_buf*(): int32 {.exportc, cdecl.} =
  cast[int32](g_out_buf.addr)

proc bl_tick*(slot: int32): int32 {.exportc, cdecl.} =
  brainTick(slot)
