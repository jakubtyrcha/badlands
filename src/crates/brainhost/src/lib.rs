// wasmtime embedding behind the narrow C ABI in include/brainhost.h. This
// crate is the runtime half of the wasm-brain plan (game-AI "brain" modules,
// authored in Nim, later possibly other languages, running inside the
// badlands C++ game): it loads a compiled wasm module, validates it against
// the brain ABI, and ticks it with opaque byte buffers. It knows NOTHING
// about game types or the ViewWire layout — it moves bytes and enforces a
// determinism contract (see `make_config` below), nothing more.
//
// Ownership: `BhProgram` is a compiled Module + the Engine it was compiled
// with (each program gets its own Engine so its Config — the determinism
// contract — travels with it). `BhInstance` is one instantiation of a
// program: its own Store<HostState> plus the resolved, type-checked guest
// exports. Both are handed to C++ as opaque pointers via Box::into_raw /
// Box::from_raw.

use std::cell::RefCell;
use std::ffi::{c_void, CString};
use std::ops::Range;
use std::os::raw::c_char;
use std::panic;
use std::panic::AssertUnwindSafe;
use std::ptr;

use wasmtime::{
    Caller, Config, Engine, Extern, ExternType, Instance, Linker, Memory, Module, Store, Trap,
    TypedFunc, WasmParams, WasmResults,
};

// ---------------------------------------------------------------------------
// Return codes (mirrors the #defines in include/brainhost.h — keep in sync).
// ---------------------------------------------------------------------------

const BH_OK: i32 = 0;
const BH_ERR_TRAP: i32 = -1;
const BH_ERR_FUEL: i32 = -2;
const BH_ERR_SCRIPT: i32 = -3;
const BH_ERR_ARGS: i32 = -4;
const BH_ERR_PANIC: i32 = -5;

/// Fuel granted immediately before EVERY guest call this crate makes —
/// bl_abi_version/bl_init/bl_spawn/bl_despawn/bl_view_buf/bl_out_buf/bl_tick
/// all get their own fresh copy of this budget, not a shared running total.
/// `Config::consume_fuel(true)` makes a Store start with 0 fuel (any guest
/// call would trap instantly), so every one of the calls above resets fuel
/// first. Giving accessor calls (bl_view_buf/bl_out_buf/bl_abi_version) the
/// same full budget as bl_tick is generous but simplest: one rule ("reset to
/// FUEL_PER_TICK, then call") for every guest entry point, rather than a
/// special-cased smaller budget for the trivial accessors.
const FUEL_PER_TICK: u64 = 50_000_000;

/// Mirrors `BH_MAX_BUF_LEN` in include/brainhost.h: the hard cap on a single
/// bh_tick view/out range, independent of (and in addition to) the
/// requirement that the range fit inside the instance's actual linear
/// memory.
const MAX_BUF_LEN: usize = 64 * 1024;

thread_local! {
    // bh_last_error()'s backing storage: a single reusable CString per
    // thread. Holding the CString (not just a String) means bh_last_error
    // can hand back a raw pointer with no per-call allocation-then-leak.
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

fn set_last_error(msg: impl AsRef<str>) {
    // CString::new fails only on interior NUL bytes; a wasmtime error string
    // containing one is implausible but not worth panicking over.
    let sanitized = msg.as_ref().replace('\0', "\u{fffd}");
    let c = CString::new(sanitized).unwrap_or_else(|_| CString::new("<error>").unwrap());
    LAST_ERROR.with(|cell| *cell.borrow_mut() = c);
}

/// bh_last_error(): thread-local, valid until this thread's next brainhost
/// call. Never NULL (empty string when nothing has failed yet).
#[unsafe(no_mangle)]
pub extern "C" fn bh_last_error() -> *const c_char {
    let result = panic::catch_unwind(|| LAST_ERROR.with(|cell| cell.borrow().as_ptr()));
    result.unwrap_or(ptr::null())
}

// ---------------------------------------------------------------------------
// wasmtime Config: the determinism contract. Every BhProgram gets an Engine
// built from this. See the brief this crate implements (docs/superpowers/
// sdd/task-1-brief.md) for the rationale behind each setting; kept together
// here so the contract is one obvious place to audit.
// ---------------------------------------------------------------------------

fn make_config() -> Config {
    let mut config = Config::new();
    // Cranelift folds every NaN produced by a float op to one canonical bit
    // pattern instead of leaving hardware-dependent payload/sign bits, so
    // float results replay identically across the CPUs the game runs on.
    config.cranelift_nan_canonicalization(true);
    // Every guest call is metered; combined with resetting the budget before
    // each call (FUEL_PER_TICK above), a runaway or hostile script traps
    // instead of hanging the game loop's tick.
    config.consume_fuel(true);
    // Wasm threads OFF: this crate depends on wasmtime with its "threads"
    // Cargo feature disabled (default-features are trimmed to just
    // cranelift+runtime), so `Config::wasm_threads` doesn't even exist to
    // call — the threads proposal (shared memories, atomics) is absent at
    // compile time, not merely turned off at runtime. Brains are
    // single-threaded, deterministic guests; there is nothing to opt into.
    //
    // Relaxed-SIMD ops are deliberately non-deterministic (their rounding is
    // hardware-dependent by spec); keep them unavailable rather than pin
    // them down, since determinism is the entire point of this contract.
    config.wasm_relaxed_simd(false);
    config
}

// ---------------------------------------------------------------------------
// BhProgram: a compiled module + the Engine it was compiled with.
// ---------------------------------------------------------------------------

pub struct BhProgram {
    engine: Engine,
    module: Module,
}

/// bh_load: compile `wasm_bytes` (binary wasm, not wat text — this crate
/// depends on wasmtime with its optional "wat" feature disabled, so text
/// input is rejected) into a BhProgram. No ABI validation here; that's
/// bh_instantiate's job, so a mismatched/hostile module still loads and can
/// be inspected/rejected with a precise error at instantiate time.
///
/// # Safety
/// `wasm_bytes` must be NULL (with `len` ignored) or point at `len` readable
/// bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_load(wasm_bytes: *const u8, len: usize) -> *mut BhProgram {
    let result = panic::catch_unwind(|| {
        if wasm_bytes.is_null() {
            set_last_error("bh_load: wasm_bytes is NULL");
            return ptr::null_mut();
        }
        let bytes = unsafe { std::slice::from_raw_parts(wasm_bytes, len) };
        let engine = match Engine::new(&make_config()) {
            Ok(e) => e,
            Err(e) => {
                set_last_error(format!("bh_load: engine init failed: {e:#}"));
                return ptr::null_mut();
            }
        };
        let module = match Module::new(&engine, bytes) {
            Ok(m) => m,
            Err(e) => {
                set_last_error(format!("bh_load: module compile failed: {e:#}"));
                return ptr::null_mut();
            }
        };
        Box::into_raw(Box::new(BhProgram { engine, module }))
    });
    result.unwrap_or_else(|_| {
        set_last_error("bh_load: internal panic");
        ptr::null_mut()
    })
}

/// bh_drop_program. Safe to call with NULL.
///
/// # Safety
/// `program` must be NULL or a live pointer previously returned by `bh_load`
/// and not yet freed, with no live `BhInstance` created from it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_drop_program(program: *mut BhProgram) {
    if program.is_null() {
        return;
    }
    let _ = panic::catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(program) });
    }));
}

// ---------------------------------------------------------------------------
// BhInstance: one instantiation — Store + resolved, type-checked exports.
// ---------------------------------------------------------------------------

/// C function-pointer type matching `BhLogFn` in include/brainhost.h.
/// `Option<...>` rather than a bare fn pointer so a NULL `log_fn` from C
/// maps to `None` instead of being an invalid, unrepresentable value (the
/// null-pointer optimization makes `Option<extern "C" fn(...)>` and a raw
/// nullable function pointer the same ABI shape).
type BhLogFn = extern "C" fn(i32, *const u8, usize, *mut c_void);

/// Per-Store data: just the registered log sink. Deliberately data-only —
/// see CLAUDE.md's "FFI is data-only and mockable".
struct HostState {
    log_fn: Option<BhLogFn>,
    log_user: *mut c_void,
}

pub struct BhInstance {
    store: Store<HostState>,
    memory: Memory,
    // bl_init/bl_view_buf/bl_out_buf are intentionally NOT kept as TypedFuncs
    // here: bh_instantiate calls each exactly once (bl_init as a local
    // TypedFunc; bl_view_buf/bl_out_buf likewise, immediately turned into the
    // cached addresses below) and the ABI never calls any of the three again
    // afterward -- see view_ptr/out_ptr's own comment for why that is safe.
    bl_spawn: TypedFunc<(i32, i32, i32), ()>,
    bl_despawn: TypedFunc<i32, ()>,
    // Buffer addresses, resolved ONCE at instantiation (see include/
    // brainhost.h's contract note): a conforming brain's view/out buffers are
    // ordinary global variables, so their address is fixed for the whole
    // instance lifetime and re-querying it every tick is two wasted fueled
    // guest calls. bh_tick still bounds-checks these against the instance's
    // CURRENT linear memory size every call -- memory can grow even though an
    // address baked into a global can't move.
    view_ptr: i32,
    out_ptr: i32,
    bl_tick: TypedFunc<i32, i32>,
}

/// The env.bl_log host function: reads `len` bytes at `ptr` from the calling
/// instance's own memory and forwards them to the registered `BhLogFn`. A
/// request whose [ptr, ptr+len) range is not FULLY within the instance's
/// memory (negative ptr/len, or the range starting or ending outside it) is
/// replaced with a short marker instead of trapping the guest or silently
/// truncating to whatever fits — a script logging a bad pointer shouldn't
/// crash the tick, and a partially-out-of-range request should be visibly
/// flagged rather than quietly clipped.
fn host_bl_log(mut caller: Caller<'_, HostState>, level: i32, ptr: i32, len: i32) {
    let (log_fn, log_user) = {
        let state = caller.data();
        (state.log_fn, state.log_user)
    };
    let Some(log_fn) = log_fn else {
        return;
    };
    let Some(memory) = caller
        .get_export("memory")
        .and_then(Extern::into_memory)
    else {
        return;
    };

    const OOB_MARKER: &[u8] = b"<bl_log: out-of-bounds ptr/len>";
    let data = memory.data(&caller);
    // Full-range check, not a clamp-and-take: a range that starts in bounds
    // but runs past the end must be marked, not silently truncated (the
    // marker is the only signal the host log ever gets that the guest's
    // ptr/len disagreed with its own memory).
    let bytes: &[u8] = if ptr < 0 || len < 0 {
        OOB_MARKER
    } else {
        let p = ptr as usize;
        let l = len as usize;
        match p.checked_add(l) {
            Some(end) if end <= data.len() => &data[p..end],
            _ => OOB_MARKER,
        }
    };
    log_fn(level, bytes.as_ptr(), bytes.len(), log_user);
}

/// Resolve+typecheck one required export. A missing export or a signature
/// mismatch both surface here as one clear error (wasmtime's own message
/// distinguishes the two).
fn typed_export<Params, Results>(
    instance: &Instance,
    store: &mut Store<HostState>,
    name: &str,
) -> Result<TypedFunc<Params, Results>, String>
where
    Params: WasmParams,
    Results: WasmResults,
{
    instance
        .get_typed_func(&mut *store, name)
        .map_err(|e| format!("export `{name}` missing or has the wrong signature: {e:#}"))
}

/// Reset the fuel budget then call `func`, tagging any failure's
/// bh_last_error message with `context` (e.g. "bl_tick"). Every guest entry
/// point in this crate goes through here (see FUEL_PER_TICK's doc comment
/// for why every call — not just bl_tick — gets a fresh budget).
/// Distinguishes an out-of-fuel trap from every other trap kind via
/// wasmtime's trap code.
fn call_fueled<Params, Results>(
    store: &mut Store<HostState>,
    func: &TypedFunc<Params, Results>,
    args: Params,
    context: &str,
) -> Result<Results, i32>
where
    Params: WasmParams,
    Results: WasmResults,
{
    // consume_fuel(true) is set unconditionally in make_config(), so this
    // can only fail if that invariant is broken; let the outer catch_unwind
    // turn a broken invariant into BH_ERR_PANIC rather than silently running
    // unfueled.
    store
        .set_fuel(FUEL_PER_TICK)
        .expect("consume_fuel is enabled; set_fuel must succeed");
    func.call(&mut *store, args).map_err(|e| {
        let code = match e.downcast_ref::<Trap>() {
            Some(Trap::OutOfFuel) => BH_ERR_FUEL,
            _ => BH_ERR_TRAP,
        };
        set_last_error(format!("{context}: {e:#}"));
        code
    })
}

fn instantiate_inner(
    program: *const BhProgram,
    expected_abi_version: i32,
    world_seed: i32,
    log_fn: Option<BhLogFn>,
    log_user: *mut c_void,
) -> *mut BhInstance {
    if program.is_null() {
        set_last_error("bh_instantiate: program is NULL");
        return ptr::null_mut();
    }
    // Safety: caller contract (see the pub fn's doc comment) requires a live
    // pointer previously returned by bh_load.
    let program = unsafe { &*program };

    // ---- import validation: the no-WASI guarantee -------------------------
    for import in program.module.imports() {
        if import.module() != "env" || import.name() != "bl_log" {
            set_last_error(format!(
                "bh_instantiate: unexpected import `{}.{}` (a brain module may import at most env.bl_log)",
                import.module(),
                import.name()
            ));
            return ptr::null_mut();
        }
    }

    // ---- export validation: `memory` must be present -----------------------
    let has_memory = program
        .module
        .exports()
        .any(|e| e.name() == "memory" && matches!(e.ty(), ExternType::Memory(_)));
    if !has_memory {
        set_last_error("bh_instantiate: module does not export a memory named `memory`");
        return ptr::null_mut();
    }

    let mut store = Store::new(&program.engine, HostState { log_fn, log_user });
    // Instantiation itself runs guest-declared constant expressions (data/
    // element segment offsets, global initializers) through the same
    // fuel-metered execution path as any other guest call, so — same as
    // every call_fueled call below — it needs a budget up front or it traps
    // instantly with "all fuel consumed" before a single guest function has
    // even been reached.
    store
        .set_fuel(FUEL_PER_TICK)
        .expect("consume_fuel is enabled; set_fuel must succeed");

    let mut linker: Linker<HostState> = Linker::new(&program.engine);
    if let Err(e) = linker.func_wrap("env", "bl_log", host_bl_log) {
        set_last_error(format!("bh_instantiate: failed to define env.bl_log: {e:#}"));
        return ptr::null_mut();
    }

    let instance = match linker.instantiate(&mut store, &program.module) {
        Ok(i) => i,
        Err(e) => {
            set_last_error(format!("bh_instantiate: instantiation failed: {e:#}"));
            return ptr::null_mut();
        }
    };

    let memory = match instance.get_memory(&mut store, "memory") {
        Some(m) => m,
        None => {
            set_last_error("bh_instantiate: module does not export a memory named `memory`");
            return ptr::null_mut();
        }
    };

    macro_rules! resolve {
        ($name:expr, $params:ty, $results:ty) => {
            match typed_export::<$params, $results>(&instance, &mut store, $name) {
                Ok(f) => f,
                Err(msg) => {
                    set_last_error(format!("bh_instantiate: {msg}"));
                    return ptr::null_mut();
                }
            }
        };
    }
    let bl_abi_version = resolve!("bl_abi_version", (), i32);
    let bl_init = resolve!("bl_init", i32, ());
    let bl_spawn = resolve!("bl_spawn", (i32, i32, i32), ());
    let bl_despawn = resolve!("bl_despawn", i32, ());
    let bl_view_buf = resolve!("bl_view_buf", (), i32);
    let bl_out_buf = resolve!("bl_out_buf", (), i32);
    let bl_tick = resolve!("bl_tick", i32, i32);

    let version = match call_fueled(&mut store, &bl_abi_version, (), "bl_abi_version") {
        Ok(v) => v,
        Err(_) => return ptr::null_mut(),
    };
    if version != expected_abi_version {
        set_last_error(format!(
            "bh_instantiate: abi version mismatch: module reports bl_abi_version()={version}, expected {expected_abi_version}"
        ));
        return ptr::null_mut();
    }

    if call_fueled(&mut store, &bl_init, world_seed, "bl_init").is_err() {
        return ptr::null_mut();
    }

    // Cache the buffer addresses NOW, once, rather than on every bh_tick (see
    // BhInstance::view_ptr/out_ptr's doc comment for the contract this
    // relies on: a conforming brain's buffers live at a fixed address for its
    // whole lifetime, e.g. ordinary global variables).
    let view_ptr = match call_fueled(&mut store, &bl_view_buf, (), "bl_view_buf") {
        Ok(p) => p,
        Err(_) => return ptr::null_mut(),
    };
    let out_ptr = match call_fueled(&mut store, &bl_out_buf, (), "bl_out_buf") {
        Ok(p) => p,
        Err(_) => return ptr::null_mut(),
    };

    Box::into_raw(Box::new(BhInstance {
        store,
        memory,
        bl_spawn,
        bl_despawn,
        view_ptr,
        out_ptr,
        bl_tick,
    }))
}

/// bh_instantiate. See include/brainhost.h for the full contract.
///
/// # Safety
/// `p` must be NULL or a live pointer previously returned by `bh_load`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_instantiate(
    p: *const BhProgram,
    expected_abi_version: i32,
    world_seed: i32,
    log_fn: Option<BhLogFn>,
    log_user: *mut c_void,
) -> *mut BhInstance {
    let result = panic::catch_unwind(AssertUnwindSafe(|| {
        instantiate_inner(p, expected_abi_version, world_seed, log_fn, log_user)
    }));
    result.unwrap_or_else(|_| {
        set_last_error("bh_instantiate: internal panic");
        ptr::null_mut()
    })
}

/// bh_drop_instance. Safe to call with NULL.
///
/// # Safety
/// `instance` must be NULL or a live pointer previously returned by
/// `bh_instantiate` and not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_drop_instance(instance: *mut BhInstance) {
    if instance.is_null() {
        return;
    }
    let _ = panic::catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(instance) });
    }));
}

/// bh_spawn. See include/brainhost.h.
///
/// # Safety
/// `instance` must be NULL or a live pointer previously returned by
/// `bh_instantiate`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_spawn(instance: *mut BhInstance, slot: i32, cls: i32, seed: i32) -> i32 {
    let result = panic::catch_unwind(AssertUnwindSafe(|| {
        let Some(inst) = (unsafe { instance.as_mut() }) else {
            set_last_error("bh_spawn: instance is NULL");
            return BH_ERR_ARGS;
        };
        let bl_spawn = inst.bl_spawn.clone();
        match call_fueled(&mut inst.store, &bl_spawn, (slot, cls, seed), "bl_spawn") {
            Ok(()) => BH_OK,
            Err(code) => code,
        }
    }));
    result.unwrap_or_else(|_| {
        set_last_error("bh_spawn: internal panic");
        BH_ERR_PANIC
    })
}

/// bh_despawn. See include/brainhost.h.
///
/// # Safety
/// `instance` must be NULL or a live pointer previously returned by
/// `bh_instantiate`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_despawn(instance: *mut BhInstance, slot: i32) -> i32 {
    let result = panic::catch_unwind(AssertUnwindSafe(|| {
        let Some(inst) = (unsafe { instance.as_mut() }) else {
            set_last_error("bh_despawn: instance is NULL");
            return BH_ERR_ARGS;
        };
        let bl_despawn = inst.bl_despawn.clone();
        match call_fueled(&mut inst.store, &bl_despawn, slot, "bl_despawn") {
            Ok(()) => BH_OK,
            Err(code) => code,
        }
    }));
    result.unwrap_or_else(|_| {
        set_last_error("bh_despawn: internal panic");
        BH_ERR_PANIC
    })
}

/// Bounds-check rule: `ptr` must be non-negative, `len` must fit under
/// MAX_BUF_LEN, and `[ptr, ptr+len)` must fit inside `mem_len` bytes of
/// linear memory. Mirrors BH_MAX_BUF_LEN's doc comment in
/// include/brainhost.h.
fn checked_range(ptr: i32, len: usize, mem_len: usize) -> Option<Range<usize>> {
    if ptr < 0 || len > MAX_BUF_LEN {
        return None;
    }
    let start = ptr as usize;
    let end = start.checked_add(len)?;
    if end > mem_len {
        return None;
    }
    Some(start..end)
}

fn tick_inner(
    instance: *mut BhInstance,
    slot: i32,
    view: *const u8,
    view_len: usize,
    out: *mut u8,
    out_len: usize,
) -> i32 {
    let Some(inst) = (unsafe { instance.as_mut() }) else {
        set_last_error("bh_tick: instance is NULL");
        return BH_ERR_ARGS;
    };
    if view.is_null() && view_len > 0 {
        set_last_error("bh_tick: view is NULL but view_len > 0");
        return BH_ERR_ARGS;
    }
    if out.is_null() && out_len > 0 {
        set_last_error("bh_tick: out is NULL but out_len > 0");
        return BH_ERR_ARGS;
    }

    let bl_tick = inst.bl_tick.clone();

    // Cached at bh_instantiate time (BhInstance::view_ptr/out_ptr's doc
    // comment) -- no bl_view_buf/bl_out_buf guest call here anymore. The
    // bounds check right below still runs fresh every tick against the
    // instance's CURRENT memory size, since memory can grow.
    let view_ptr = inst.view_ptr;
    let out_ptr = inst.out_ptr;

    let mem_len = inst.memory.data(&inst.store).len();
    let Some(view_range) = checked_range(view_ptr, view_len, mem_len) else {
        set_last_error(format!(
            "bh_tick: view range [{view_ptr}, +{view_len}) doesn't fit (cap {MAX_BUF_LEN} bytes, memory is {mem_len} bytes)"
        ));
        return BH_ERR_ARGS;
    };
    let Some(out_range) = checked_range(out_ptr, out_len, mem_len) else {
        set_last_error(format!(
            "bh_tick: out range [{out_ptr}, +{out_len}) doesn't fit (cap {MAX_BUF_LEN} bytes, memory is {mem_len} bytes)"
        ));
        return BH_ERR_ARGS;
    };

    if view_len > 0 {
        // Safety: caller contract requires `view` valid for `view_len` bytes
        // when view_len > 0 (checked above).
        let src = unsafe { std::slice::from_raw_parts(view, view_len) };
        inst.memory.data_mut(&mut inst.store)[view_range].copy_from_slice(src);
    }

    let rc = match call_fueled(&mut inst.store, &bl_tick, slot, "bl_tick") {
        Ok(rc) => rc,
        Err(code) => return code,
    };
    if rc != 0 {
        set_last_error(format!("bh_tick: bl_tick(slot={slot}) returned nonzero: {rc}"));
        return BH_ERR_SCRIPT;
    }

    if out_len > 0 {
        let data = inst.memory.data(&inst.store);
        // Safety: caller contract requires `out` valid for `out_len` bytes
        // when out_len > 0 (checked above).
        let dst = unsafe { std::slice::from_raw_parts_mut(out, out_len) };
        dst.copy_from_slice(&data[out_range]);
    }

    BH_OK
}

/// bh_tick. See include/brainhost.h for the full contract.
///
/// # Safety
/// `instance` must be NULL or a live pointer previously returned by
/// `bh_instantiate`. `view` must be valid for `view_len` bytes (or NULL with
/// `view_len == 0`); `out` must be valid for `out_len` writable bytes (or
/// NULL with `out_len == 0`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bh_tick(
    instance: *mut BhInstance,
    slot: i32,
    view: *const u8,
    view_len: usize,
    out: *mut u8,
    out_len: usize,
) -> i32 {
    let result = panic::catch_unwind(AssertUnwindSafe(|| {
        tick_inner(instance, slot, view, view_len, out, out_len)
    }));
    result.unwrap_or_else(|_| {
        set_last_error("bh_tick: internal panic");
        BH_ERR_PANIC
    })
}

// ---------------------------------------------------------------------------
// Tests: exercise the extern "C" thunks (not just Rust-level helpers), per
// the brief — the catch_unwind wrappers are exactly what's under test.
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    const ABI_VERSION: i32 = 1;

    /// A conforming brain module's fixed layout: view/out buffers at 1024 /
    /// 2048 in a single 64 KiB memory page, so tests can reason about exact
    /// addresses without querying anything.
    const VIEW_BUF_ADDR: i32 = 1024;
    const OUT_BUF_ADDR: i32 = 2048;

    /// Builds a brain module's wat text. `extra_import` is spliced in right
    /// after the mandatory `env.bl_log` import (use it to add an
    /// unsupported import for the no-WASI test). `init_body`/`tick_body` are
    /// spliced into bl_init/bl_tick respectively.
    fn build_wasm(abi_version: i32, extra_import: &str, init_body: &str, tick_body: &str) -> Vec<u8> {
        let text = format!(
            r#"
            (module
                (import "env" "bl_log" (func $bl_log (param i32 i32 i32)))
                {extra_import}
                (memory (export "memory") 1)
                (data (i32.const 100) "hello brain")
                (func (export "bl_abi_version") (result i32) i32.const {abi_version})
                (func (export "bl_init") (param i32)
                    {init_body}
                )
                (func (export "bl_spawn") (param i32 i32 i32))
                (func (export "bl_despawn") (param i32))
                (func (export "bl_view_buf") (result i32) i32.const {VIEW_BUF_ADDR})
                (func (export "bl_out_buf") (result i32) i32.const {OUT_BUF_ADDR})
                (func (export "bl_tick") (param i32) (result i32)
                    {tick_body}
                )
            )
            "#
        );
        wat::parse_str(&text).expect("valid wat fixture")
    }

    /// bl_tick body that copies the first 8 bytes of the view buffer to the
    /// out buffer and returns 0, per the brief's fixture spec.
    const COPY_8_BYTES: &str = "(i64.store (i32.const 2048) (i64.load (i32.const 1024))) i32.const 0";

    fn conforming_wasm() -> Vec<u8> {
        build_wasm(ABI_VERSION, "", "", COPY_8_BYTES)
    }

    extern "C" fn noop_log(_level: i32, _msg: *const u8, _len: usize, _user: *mut c_void) {}

    fn load_ok(wasm: &[u8]) -> *mut BhProgram {
        let p = unsafe { bh_load(wasm.as_ptr(), wasm.len()) };
        assert!(!p.is_null(), "bh_load failed: {:?}", last_error());
        p
    }

    fn last_error() -> String {
        let ptr = bh_last_error();
        assert!(!ptr.is_null());
        unsafe { std::ffi::CStr::from_ptr(ptr) }
            .to_string_lossy()
            .into_owned()
    }

    // 1. happy path: load, instantiate, spawn, tick — out buffer holds the
    // copied bytes.
    #[test]
    fn happy_path_tick_copies_view_into_out() {
        let wasm = conforming_wasm();
        let program = load_ok(&wasm);

        let instance = unsafe {
            bh_instantiate(program, ABI_VERSION, 42, Some(noop_log), ptr::null_mut())
        };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        assert_eq!(unsafe { bh_spawn(instance, 0, 7, 123) }, BH_OK);

        let view: [u8; 8] = [1, 2, 3, 4, 5, 6, 7, 8];
        let mut out = [0u8; 8];
        let rc = unsafe {
            bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(rc, BH_OK, "bh_tick failed: {}", last_error());
        assert_eq!(out, view);

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }

    // 2. trap: bl_tick body is `unreachable` => BH_ERR_TRAP, bh_last_error
    // non-empty; a subsequent tick on a fresh instance still works. Traps
    // only when slot==0 so "still works" can mean what it says: the fresh
    // instance actually completes a real tick (BH_OK, bytes copied), not
    // just "traps again without crashing".
    #[test]
    fn trap_reports_bh_err_trap_and_fresh_instance_still_works() {
        let tick_body = "(if (i32.eqz (local.get 0)) (then unreachable)) \
                          (i64.store (i32.const 2048) (i64.load (i32.const 1024))) i32.const 0";
        let wasm = build_wasm(ABI_VERSION, "", "", tick_body);
        let program = load_ok(&wasm);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        let view = [0u8; 8];
        let mut out = [0u8; 8];
        let rc = unsafe {
            bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(rc, BH_ERR_TRAP);
        assert!(!last_error().is_empty());

        // This crate's design choice: a trapped instance is simply dropped
        // and never reused; the caller instantiates a fresh BhInstance from
        // the same (still-valid) BhProgram for the next attempt.
        unsafe { bh_drop_instance(instance) };
        let instance2 =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(!instance2.is_null(), "re-instantiate failed: {}", last_error());

        // A real tick (slot=1, no trap) on the fresh instance succeeds,
        // proving the program/engine survive a prior instance's trap
        // untouched — not just that a second trap doesn't crash.
        let view2: [u8; 8] = [9, 8, 7, 6, 5, 4, 3, 2];
        let mut out2 = [0u8; 8];
        let rc2 = unsafe {
            bh_tick(instance2, 1, view2.as_ptr(), view2.len(), out2.as_mut_ptr(), out2.len())
        };
        assert_eq!(rc2, BH_OK, "bh_tick on fresh instance failed: {}", last_error());
        assert_eq!(out2, view2);

        unsafe {
            bh_drop_instance(instance2);
            bh_drop_program(program);
        }
    }

    // 3. fuel: bl_tick infinite `loop` => BH_ERR_FUEL, distinguished from
    // BH_ERR_TRAP.
    #[test]
    fn infinite_loop_exhausts_fuel() {
        let wasm = build_wasm(ABI_VERSION, "", "", "(loop $l (result i32) (br $l))");
        let program = load_ok(&wasm);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        let view = [0u8; 8];
        let mut out = [0u8; 8];
        let rc = unsafe {
            bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(rc, BH_ERR_FUEL);
        assert!(!last_error().is_empty());

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }

    // 4. version mismatch: module reports 999 => bh_instantiate NULL, error
    // mentions both versions.
    #[test]
    fn abi_version_mismatch_rejected() {
        let wasm = build_wasm(999, "", "", COPY_8_BYTES);
        let program = load_ok(&wasm);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(instance.is_null());
        let err = last_error();
        assert!(err.contains("999"), "error should mention module version: {err}");
        assert!(
            err.contains(&ABI_VERSION.to_string()),
            "error should mention expected version: {err}"
        );

        unsafe { bh_drop_program(program) };
    }

    // 5. unknown import: module imports env.evil => instantiate NULL (the
    // no-WASI test).
    #[test]
    fn unknown_import_rejected() {
        let wasm = build_wasm(
            ABI_VERSION,
            r#"(import "env" "evil" (func $evil))"#,
            "",
            COPY_8_BYTES,
        );
        let program = load_ok(&wasm);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(instance.is_null());
        let err = last_error();
        assert!(err.contains("evil"), "error should name the bad import: {err}");

        unsafe { bh_drop_program(program) };
    }

    // 6. bl_log: module calls bl_log with a static string => callback
    // receives exact bytes.
    #[test]
    fn bl_log_forwards_exact_bytes() {
        static CAPTURED: Mutex<Vec<(i32, Vec<u8>)>> = Mutex::new(Vec::new());
        extern "C" fn capture(level: i32, msg: *const u8, len: usize, _user: *mut c_void) {
            let bytes = unsafe { std::slice::from_raw_parts(msg, len) }.to_vec();
            CAPTURED.lock().unwrap().push((level, bytes));
        }
        CAPTURED.lock().unwrap().clear();

        // "hello brain" lives at offset 100 (see build_wasm's data segment),
        // 11 bytes long; bl_init logs it with level 7.
        let wasm = build_wasm(
            ABI_VERSION,
            "",
            "i32.const 7 i32.const 100 i32.const 11 call $bl_log",
            COPY_8_BYTES,
        );
        let program = load_ok(&wasm);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(capture), ptr::null_mut()) };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        let captured = CAPTURED.lock().unwrap();
        assert_eq!(captured.len(), 1);
        assert_eq!(captured[0].0, 7);
        assert_eq!(captured[0].1, b"hello brain");
        drop(captured);

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }

    // 7. bounds: view_len larger than the module's declared buffer region
    // => BH_ERR_ARGS.
    #[test]
    fn view_len_beyond_memory_is_bh_err_args() {
        let wasm = conforming_wasm();
        let program = load_ok(&wasm);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        // Memory is 1 page = 65536 bytes; view buf starts at 1024, so
        // 1024 + 65000 > 65536 overflows the module's actual linear memory
        // (while still under the separate 64 KiB BH_MAX_BUF_LEN cap).
        let view = vec![0u8; 65000];
        let mut out = [0u8; 8];
        let rc = unsafe {
            bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(rc, BH_ERR_ARGS);
        assert!(!last_error().is_empty());

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }

    // 8. catch_unwind: passing NULL program to bh_instantiate returns NULL,
    // no abort.
    #[test]
    fn null_program_returns_null_not_abort() {
        let instance = unsafe {
            bh_instantiate(ptr::null(), ABI_VERSION, 0, Some(noop_log), ptr::null_mut())
        };
        assert!(instance.is_null());
        assert!(!last_error().is_empty());
    }

    // 9. acceptance: the real, shipped assets/brains/hero.wasm (built by
    // scripts/build_brains.sh from scripts/brains/nim/hero.nim) conforms to
    // the full brain ABI end to end, not just a wat fixture. Size constants
    // below are duplicated as literals rather than computed, on purpose --
    // see the comment on each: they must match game/src/brain_abi.h by hand,
    // the same way scripts/brains/nim/abi.nim does on the Nim side.
    #[test]
    fn real_hero_wasm_conforms() {
        // sizeof(BlViewWire) per game/src/brain_abi.h's static_assert.
        const VIEW_WIRE_LEN: usize = 1080;
        // sizeof(BlDecisionWire) per game/src/brain_abi.h's static_assert.
        const DECISION_WIRE_LEN: usize = 40;
        // offsetof(BlViewWire, version) is 0; BL_ABI_VERSION is 1 (both in
        // game/src/brain_abi.h). A zeroed buffer with just the version
        // stamped is exactly what a host that hasn't populated the rest of
        // the view yet would send on a first tick.
        const VERSION_OFFSET: usize = 0;

        let wasm_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../assets/brains/hero.wasm");
        let wasm_bytes = std::fs::read(&wasm_path)
            .unwrap_or_else(|e| panic!("failed to read {}: {e}", wasm_path.display()));

        let program = load_ok(&wasm_bytes);

        static LOGGED: Mutex<Vec<String>> = Mutex::new(Vec::new());
        extern "C" fn capture_log(_level: i32, msg: *const u8, len: usize, _user: *mut c_void) {
            let text = unsafe { std::slice::from_raw_parts(msg, len) };
            LOGGED
                .lock()
                .unwrap()
                .push(String::from_utf8_lossy(text).into_owned());
        }
        LOGGED.lock().unwrap().clear();

        let instance = unsafe {
            bh_instantiate(program, ABI_VERSION, 42, Some(capture_log), ptr::null_mut())
        };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        {
            let logged = LOGGED.lock().unwrap();
            assert!(
                logged.iter().any(|l| l.contains("init")),
                "expected an init log line from bl_init, got: {logged:?}"
            );
        }

        assert_eq!(unsafe { bh_spawn(instance, 0, 0, 1) }, BH_OK);

        let mut view = vec![0u8; VIEW_WIRE_LEN];
        view[VERSION_OFFSET..VERSION_OFFSET + 4].copy_from_slice(&(ABI_VERSION as u32).to_le_bytes());
        let mut out = vec![0u8; DECISION_WIRE_LEN];
        let rc = unsafe {
            bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(rc, BH_OK, "bh_tick failed: {}", last_error());

        // An all-Idle DecisionWire: pause_duration_millis=0 (i64, offset 0),
        // activity_id=0/goal_kind=0 (offset 8/12), goal_x=goal_z=0.0 (offset
        // 16/20), command_kind=0/command_arg=0 (offset 24/28),
        // follow_up_on_arrival=0/pause_kind=0 (offset 32/36) -- i.e. every
        // byte zero.
        assert!(
            out.iter().all(|&b| b == 0),
            "expected an all-zero (all-Idle) DecisionWire, got: {out:?}"
        );

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }

    // 10. fail-fast fixture: game/tests/fixtures/trap_brain.wasm
    // (scripts/brains/nim/trap_test.nim) unconditionally traps bl_tick with a
    // real `unreachable` (__builtin_trap()) -- pins the Nim -> wasm-trap ->
    // BH_ERR_TRAP chain end to end against the actual compiled artifact, not
    // a wat fixture. This coverage used to live at the sim level
    // (game/tests/wasm_brain_tests.cpp's BhInstance reinstantiation tests,
    // pre-Task-7); the C++ game now treats ANY wasm-brain failure as fatal
    // (brain_fatal, game/src/wasm_brain.cpp) rather than reinstantiating, so
    // there is nothing left to exercise about trapping at that layer -- this
    // crate's own error-code contract is unchanged by that policy (fatality
    // is game policy, not a brainhost concern), so trapping still belongs
    // here.
    #[test]
    fn real_trap_wasm_traps() {
        // trap_test.nim shares hero.nim's BlViewWire/BlDecisionWire var
        // buffers (see scripts/brains/nim/trap_test.nim), so the same
        // static_assert'd sizes from game/src/brain_abi.h apply as
        // real_hero_wasm_conforms uses above.
        const VIEW_WIRE_LEN: usize = 1080;
        const DECISION_WIRE_LEN: usize = 40;

        let wasm_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../game/tests/fixtures/trap_brain.wasm");
        let wasm_bytes = std::fs::read(&wasm_path)
            .unwrap_or_else(|e| panic!("failed to read {}: {e}", wasm_path.display()));

        let program = load_ok(&wasm_bytes);
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        // bl_spawn never traps in this fixture (only bl_tick does).
        assert_eq!(unsafe { bh_spawn(instance, 0, 0, 1) }, BH_OK);

        let view = vec![0u8; VIEW_WIRE_LEN];
        let mut out = vec![0u8; DECISION_WIRE_LEN];
        let rc = unsafe {
            bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(rc, BH_ERR_TRAP, "expected BH_ERR_TRAP, got {rc}");
        assert!(!last_error().is_empty());

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }

    // 11. buffer-address caching (Task 8, finding C): a module whose
    // bl_view_buf() returns X (a mutable global counter's initial reading) on
    // the FIRST call and Y on every call after. bh_instantiate resolves
    // bl_view_buf/bl_out_buf exactly ONCE and bh_tick reuses that cached
    // address on every subsequent tick (include/brainhost.h's "buffer
    // addresses are fixed for the instance's lifetime" contract) -- so this
    // module's ONE honest answer (X) is what bh_instantiate consumes, and
    // bh_tick never asks again. bl_tick here always reads its view from the
    // real, fixed address X, exactly like a conforming brain's own
    // global-variable buffer would; before the caching fix, a second bh_tick
    // would re-invoke bl_view_buf(), honor the lying Y address for the WRITE,
    // and this fixed-address READ would see stale bytes instead of the
    // freshly-copied view.
    #[test]
    fn bh_tick_uses_the_buffer_address_cached_at_instantiate_not_a_later_call() {
        const REAL_ADDR: i32 = 1024; // X: where the "real" buffer lives
        const LYING_ADDR: i32 = 4096; // Y: what a re-queried bl_view_buf would claim later
        const OUT_ADDR: i32 = 2048;

        let text = format!(
            r#"
            (module
                (import "env" "bl_log" (func $bl_log (param i32 i32 i32)))
                (memory (export "memory") 1)
                (global $calls (mut i32) (i32.const 0))
                (func (export "bl_abi_version") (result i32) i32.const {ABI_VERSION})
                (func (export "bl_init") (param i32))
                (func (export "bl_spawn") (param i32 i32 i32))
                (func (export "bl_despawn") (param i32))
                (func (export "bl_view_buf") (result i32)
                    (local $addr i32)
                    (local.set $addr
                        (if (result i32) (i32.eqz (global.get $calls))
                            (then (i32.const {REAL_ADDR}))
                            (else (i32.const {LYING_ADDR}))))
                    (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
                    (local.get $addr))
                (func (export "bl_out_buf") (result i32) i32.const {OUT_ADDR})
                (func (export "bl_tick") (param i32) (result i32)
                    ;; ALWAYS reads from the real, fixed address -- exactly what a
                    ;; conforming brain's own global-variable buffer would do;
                    ;; only this fixture's bl_view_buf itself lies about a
                    ;; changing address, to prove the host doesn't re-ask it.
                    (i64.store (i32.const {OUT_ADDR}) (i64.load (i32.const {REAL_ADDR})))
                    i32.const 0)
            )
            "#
        );
        let wasm = wat::parse_str(&text).expect("valid wat fixture");
        let program = load_ok(&wasm);

        // bh_instantiate consumes the module's ONE honest (X) answer here.
        let instance =
            unsafe { bh_instantiate(program, ABI_VERSION, 0, Some(noop_log), ptr::null_mut()) };
        assert!(!instance.is_null(), "bh_instantiate failed: {}", last_error());

        assert_eq!(unsafe { bh_spawn(instance, 0, 0, 1) }, BH_OK);

        // Several ticks, distinct payloads each time: if bh_tick ever
        // re-queried bl_view_buf, every tick from the SECOND on would write
        // its view bytes to the lying Y address instead, and bl_tick's
        // fixed-address (X) read would keep echoing back tick 1's stale
        // bytes rather than the current tick's.
        for view_byte in [0xAAu8, 0xBBu8, 0xCCu8] {
            let view = [view_byte; 8];
            let mut out = [0u8; 8];
            let rc = unsafe {
                bh_tick(instance, 0, view.as_ptr(), view.len(), out.as_mut_ptr(), out.len())
            };
            assert_eq!(rc, BH_OK, "bh_tick failed: {}", last_error());
            assert_eq!(out, view, "view bytes did not land at the cached address X");
        }

        unsafe {
            bh_drop_instance(instance);
            bh_drop_program(program);
        }
    }
}
