// C ABI for the `brainhost` Rust crate (src/crates/brainhost): a wasmtime
// embedding that loads a "brain" wasm module (authored in Nim, later possibly
// other languages) and ticks it with opaque byte buffers. This crate knows
// NOTHING about game types or the ViewWire layout — it moves bytes. Linked
// into the badlands C++ game via Corrosion (import-only for now; a later
// task wires a caller through CMake).
//
// A conforming brain module exports `memory` plus:
//   bl_abi_version() -> i32
//   bl_init(i32 world_seed)
//   bl_spawn(i32 slot, i32 cls, i32 seed)
//   bl_despawn(i32 slot)
//   bl_view_buf() -> i32          (pointer into the module's own memory)
//   bl_out_buf()  -> i32          (pointer into the module's own memory)
//   bl_tick(i32 slot) -> i32      (0 == ok, nonzero == script-reported error)
// and imports AT MOST `env.bl_log(i32 level, i32 ptr, i32 len)` — any other
// import makes bh_instantiate fail (the no-WASI guarantee: brains cannot
// touch the filesystem, clock, env, or network).
//
// Determinism contract enforced by this crate for every instantiated module
// (see src/crates/brainhost/src/lib.rs for the wasmtime Config that
// implements it): Cranelift NaN canonicalization on, fuel-metered execution
// (a fixed budget reset before every guest call — a runaway script traps
// instead of hanging the game loop), wasm threads and relaxed-SIMD off.
#ifndef BADLANDS_BRAINHOST_H
#define BADLANDS_BRAINHOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque: a loaded (compiled, not-yet-instantiated) brain module + its own
// wasmtime Engine. Create with bh_load, free with bh_drop_program.
typedef struct BhProgram BhProgram;

// Opaque: one live instantiation of a BhProgram — its own Store + resolved
// exports. Create with bh_instantiate, free with bh_drop_instance. Design
// choice: a BhInstance that has trapped (BH_ERR_TRAP/BH_ERR_FUEL) is not
// reused — drop it with bh_drop_instance and bh_instantiate a fresh one from
// the same BhProgram (which is unaffected by any of its instances trapping).
typedef struct BhInstance BhInstance;

// Host log sink registered at bh_instantiate time, called synchronously from
// inside a brainhost call whenever the guest invokes env.bl_log. `msg` points
// at a copy of the guest's bytes living in host memory (bounds-checked
// against the module's linear memory; a request that doesn't fit is replaced
// with a short marker instead of trapping the guest) and is valid only for
// the duration of this call — copy it if you need it afterward.
typedef void (*BhLogFn)(int32_t level, const uint8_t* msg, size_t len, void* user);

// Return codes shared by every brainhost entry point below that reports
// success/failure as an int32_t. 0 is the only success value.
#define BH_OK 0
// The wasm guest trapped for a reason other than fuel exhaustion (e.g. an
// `unreachable` instruction, an out-of-bounds guest memory access, integer
// div-by-zero, indirect-call type mismatch, ...).
#define BH_ERR_TRAP (-1)
// The guest call's fuel budget (see FUEL_PER_TICK in the .rs source) was
// exhausted before the call completed — distinguished from BH_ERR_TRAP via
// wasmtime's trap code so callers can tell "buggy/hostile forever-loop" apart
// from "crashed".
#define BH_ERR_FUEL (-2)
// bl_tick returned a nonzero value: the brain script itself reported an
// error (this is not a host/runtime failure).
#define BH_ERR_SCRIPT (-3)
// Bad arguments: a NULL pointer paired with a nonzero length, or a bh_tick
// buffer range that fails the bounds check below.
#define BH_ERR_ARGS (-4)
// A Rust panic was caught at the FFI boundary (a bug in this crate, not in
// the guest module). See bh_last_error() for whatever context survived the
// unwind.
#define BH_ERR_PANIC (-5)

// bh_tick buffer-bounds rule: for both the view range
// [bl_view_buf(), bl_view_buf()+view_len) and the out range
// [bl_out_buf(), bl_out_buf()+out_len), the requested length must be no
// larger than BH_MAX_BUF_LEN AND the whole range must fit inside the
// instance's actual linear memory (as reported by wasmtime), or bh_tick
// fails with BH_ERR_ARGS without invoking bl_tick. This is a static cap
// unrelated to any size the module itself may claim to reserve — brainhost
// never asks the guest how big its buffers are.
#define BH_MAX_BUF_LEN ((size_t)(64 * 1024))

// Compile `wasm_bytes` (a binary wasm module — NOT wat text) into a
// BhProgram. Does not validate the brain ABI (imports/exports/version) — see
// bh_instantiate for that. Returns NULL on a compile error or a NULL/invalid
// input; bh_last_error() carries the detail. Free with bh_drop_program.
BhProgram* bh_load(const uint8_t* wasm_bytes, size_t len);

// Instantiate `p`: checks the module imports at most env.bl_log (any other
// import is rejected — the no-WASI guarantee), resolves+typechecks the
// required exports (memory + the bl_* functions above), calls
// bl_abi_version() and rejects a mismatch against `expected_abi_version`,
// then calls bl_init(world_seed) with a fresh fuel budget. `log_fn` may be
// NULL to discard bl_log calls; `log_user` is passed through unexamined.
// Returns NULL on any failure (bh_last_error() carries the detail); the
// program `p` is left unaffected either way and may be instantiated again.
// Free the result with bh_drop_instance.
BhInstance* bh_instantiate(const BhProgram* p, int32_t expected_abi_version, int32_t world_seed,
                           BhLogFn log_fn, void* log_user);

// Calls bl_spawn(slot, cls, seed) with a fresh fuel budget. Returns BH_OK,
// or BH_ERR_TRAP/BH_ERR_FUEL/BH_ERR_ARGS (NULL instance)/BH_ERR_PANIC.
int32_t bh_spawn(BhInstance* instance, int32_t slot, int32_t cls, int32_t seed);

// Calls bl_despawn(slot) with a fresh fuel budget. Same return codes as
// bh_spawn.
int32_t bh_despawn(BhInstance* instance, int32_t slot);

// One sim tick for `slot`: copies `view_len` bytes from `view` into the
// instance's linear memory starting at bl_view_buf() (both bounds-checked
// per BH_MAX_BUF_LEN above — see the rule above), calls bl_tick(slot) with a
// fresh fuel budget, then copies `out_len` bytes back from bl_out_buf() into
// `out`. `view`/`out` may be NULL only if the matching `_len` is 0. Returns:
//   BH_OK           bl_tick returned 0; `out` holds the copied bytes
//   BH_ERR_TRAP      the guest trapped (not fuel)
//   BH_ERR_FUEL      fuel exhausted mid-tick
//   BH_ERR_SCRIPT    bl_tick returned nonzero (out was NOT written)
//   BH_ERR_ARGS      NULL instance, a NULL buffer with nonzero length, or a
//                    view/out range that fails the bounds check (bl_tick was
//                    NOT called; memory was NOT modified)
//   BH_ERR_PANIC     caught Rust panic
int32_t bh_tick(BhInstance* instance, int32_t slot, const uint8_t* view, size_t view_len,
                uint8_t* out, size_t out_len);

// The human-readable detail for the most recent brainhost failure on the
// calling thread (empty string if none, or if the last call succeeded).
// Thread-local; the returned pointer is only valid until the next brainhost
// call made by this thread. Never NULL.
const char* bh_last_error(void);

// Free a BhInstance previously returned by bh_instantiate. Safe to call with
// NULL.
void bh_drop_instance(BhInstance* instance);

// Free a BhProgram previously returned by bh_load. Safe to call with NULL.
// Any BhInstance created from it must already be dropped.
void bh_drop_program(BhProgram* program);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BADLANDS_BRAINHOST_H
