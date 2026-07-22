# Structured generator parameters — hand a typed data block to a generator at `Prepare` time

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Kind**: feature request
**Status**: new

**Discovered in**: badlands — a colony/hero game running one noiser generator
per entity as its game-AI brain (`scripts/brains/hero.noiser`,
`game/src/brain.cpp`), designing four creature archetypes (heroes, townfolk,
critters, monsters) whose brains are composed from reusable blocks and tuned by
per-creature-type **data**, not per-creature-type code.
**Backend observed**: VM (`NoiserBackend::kVM`). WASM/JIT not tested.

---

## 1. Summary

A generator entry point can be handed exactly **three `i32`s** — the components
of `NoiserInput::warp_id`, positionally filled into its first three parameters.
Everything else a brain needs at spawn (a per-species tuning block:
`flee_threshold`, `roam_radius`, `sleep_hours`, `engage_ratio`; class weights; a
schedule table) has to come back one scalar at a time through `@fn.` host calls,
or be baked into the script as `const`. Declaring a struct or tuple parameter on
the entry generator **compiles without a diagnostic and delivers garbage**: the
first field receives `warp_id.y`'s integer bits reinterpreted as `f32`, the rest
receive zero. We would like the generator's signature to be able to say what data
it needs — `pub gen fn deer(entity: i32, t: CritterTuning) -> i32` — and
`Prepare` to be able to supply it as a typed block, validated against the
script's declared type.

Two partial channels already exist and are worth stating precisely, because they
change what is actually missing: **per-context uniforms work with generators**
(`ctx.SetUniform(loc, v)` after `Prepare` is visible to the brain), and
**per-context buffer views work with generators**
(`ctx.RegisterBufferByName("factors", …)` then `@buf.factors[i]`). Both are
verified below. What neither provides is a *named, typed, per-instance argument
to the entry point*: uniforms are script-global flat scalars addressed by
hand-counted slot arithmetic with no whole-aggregate setter (and array uniforms
silently read `0`); buffer views are script-global integer-indexed arrays whose
row index has to be smuggled in through the same three `i32`s. This request is
for the argument-shaped channel, plus the small fixes that would make the
uniform channel safe in the meantime.

---

## 2. Current behaviour

Harness as in the companion report: compile on a 64 MiB-stack thread, bind the
`@fn.`s the script declares, `FreezeHostThunks()`, `Prepare`, `Resume` ×N.
`--warp x,y,z` sets `NoiserInput::warp_id`, `--warpsize` sets `warp_size`.

### 2a. A struct parameter on the entry generator compiles and yields garbage

```noiser
@fn.report: fn(x: f32) -> void;

struct CritterTuning { flee_threshold: f32, roam_radius: f32, sleep_hours: f32, engage_ratio: f32 }

pub gen fn brain(entity: i32, tuning: CritterTuning) -> i32 {
    loop {
        @fn.report(tuning.flee_threshold);
        @fn.report(tuning.roam_radius);
        yield 0;
    }
}
0.0
```

`Prepare(NoiserInput{.warp_id = {5,6,7}})`:

```
COMPILE OK
    report(8.40779e-45)
    report(0)
  tick 0: yield: i32=0 f32=0 (4 bytes)
    report(8.40779e-45)
    report(0)
  tick 1: yield: i32=0 f32=0 (4 bytes)
DONE (4 host calls over 2 ticks)
```

`8.40779e-45` is the `f32` reinterpretation of the integer `6` — i.e.
`warp_id.y`'s raw bits landed in `tuning.flee_threshold`. There is no compile
error, no warning, no runtime error. A tuple parameter `tuning: (f32, f32)`
behaves identically.

### 2b. Only three values arrive; the rest are silently zero

```noiser
@fn.report_i: fn(x: i32) -> void;
pub gen fn brain(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32) -> i32 {
    loop { @fn.report_i(a); @fn.report_i(b); @fn.report_i(c);
           @fn.report_i(d); @fn.report_i(e); @fn.report_i(f); yield 0; }
}
0.0
```

`--warp 5,6,7 --warpsize 8,9,10`:

```
COMPILE OK
    report_i(5)
    report_i(6)
    report_i(7)
    report_i(0)
    report_i(0)
    report_i(0)
  tick 0: yield: i32=0 f32=0 (4 bytes)
DONE (6 host calls over 1 ticks)
```

`warp_id.xyz` fills parameters 1–3. `warp_size` does **not** continue the fill —
it is reachable only as the `@warpSize` built-in inside the body (verified: a
brain reading `let (sx,sy,sz) = @warpSize;` correctly saw `8 9 10`). Parameters
4+ are zero-filled with no diagnostic. An `f32` parameter in slot 2 receives the
`i32` bit pattern, as in §2a.

So the entire host→generator setup channel is: 3 `i32`s as parameters, plus 3
more `i32`s readable as `@warpSize`. badlands already spends one of them on the
entity id (`spawn_brain` puts the entity slot in `warp_id.x`,
`game/src/brain.cpp:320-326`).

### 2c. What *does* work today — uniforms, per context

```noiser
@fn.report: fn(x: f32) -> void;
@uni.flee_threshold: f32 = 0.25;
@uni.roam_radius:    f32 = 4.0;
@uni.kind:           i32 = 0;
pub gen fn brain(entity: i32) -> i32 {
    loop { @fn.report(@uni.flee_threshold); @fn.report(@uni.roam_radius);
           @fn.report(@uni.kind as f32); yield 0; }
}
0.0
```

```
COMPILE OK
  uniform 'flee_threshold' loc=0
  uniform 'roam_radius' loc=1
  uniform 'kind' loc=2
  SetUniform f32 flee_threshold@0 = 0.9 -> 1
  SetUniform f32 roam_radius@1 = 12 -> 1
  SetUniform i32 kind@2 = 3 -> 1
    report(0.9)
    report(12)
    report(3)
DONE (3 host calls over 1 ticks)
```

This is a real per-instance channel — `ExecutionContext::SetUniform` after
`Prepare` reaches the generator, and contexts have independent uniform state.
Its limits, all measured:

- **Structs work, but only by slot arithmetic.** `@uni.tuning: Tuning` with
  `Tuning { flee, roam, sleep, engage }` reads its compile-time default
  correctly, and the host can overwrite `flee` with `SetUniform(loc + 0, 0.9)` —
  but `GetUniformLocationI32("tuning.flee")` returns `-1`, so field offsets are
  hand-counted against the script's declaration order. Reordering two fields in
  the script silently swaps two tuning values. There is no whole-struct setter.
- **Array uniforms silently read zero.** `@uni.factors: [f32; 4] = [1.0, 2.0,
  3.0, 4.0];` compiles; `@uni.factors[1]` reports `0`, both with and without a
  host `SetUniform`. So the natural "table of per-kind factors" encoding is out.
- **`@uni.x: vec4 = vec4(...)` ICEs the compiler** — `ICE:
  type_descriptor_to_abi_layout: Unknown has no physical layout`
  (`noiser-compiler/src/bytecode/abi_layout.rs:282`). The capitalised `Vec4`
  spelling works. (Separable; noted here because it is on the path anyone takes
  when trying to pack tuning into a uniform.)
- **Uniforms are script-global**, one namespace shared by every generator in the
  file — which collides directly with the companion request's `critters.noiser`
  holding `deer`, `wolf` and `boar`: they would share one `@uni.flee_threshold`
  rather than each declaring their own parameter block.

### 2d. What also works today — buffer views, per context

```noiser
@fn.report: fn(x: f32) -> void;
@buf.factors: BufferView<f32>;
pub gen fn brain(entity: i32) -> i32 {
    loop { @fn.report(@buf.factors[0]); @fn.report(@buf.factors[3]); yield 0; }
}
0.0
```

with the host registering `{10,20,30,40,50,60,70,80}`:

```
COMPILE OK
  RegisterBufferByName('factors') -> 0
    report(10)
    report(40)
DONE (2 host calls over 1 ticks)
```

`ExecutionContext::RegisterBufferByName` works on a generator context. Limits:
the element type is a scalar or tuple (a struct element type is untested/not
documented for `BufferView`), access is by integer index with no named fields,
the host memory must outlive the context, and pointing each entity at its own
row still requires telling the brain its row index — through the same three
`i32`s.

### 2e. What badlands does today

`scripts/brains/hero.noiser` bakes every tunable as a script `const`:

```noiser
const ROAM_RADIUS: f32 = 6.0;
const FATIGUE_GO_HOME: f32 = 0.6;
const FATIGUE_NIGHT: f32 = 0.2;
const BOREDOM_TAVERN: f32 = 0.5;
```

and encodes per-class variation as a hardcoded lookup returning a struct:

```noiser
struct Profile { roam_weight: f32, tavern_weight: f32 }
fn profile_for(class: i32) -> Profile {
    if class == CLASS_HUNTER { Profile { roam_weight: 9.0, tavern_weight: 5.0 } }
    else if class == CLASS_GRAVE_ROBBER { Profile { roam_weight: 4.0, tavern_weight: 7.0 } }
    ...
}
```

with `class` itself pulled per tick via `@fn.perceive_class(e)`. Every knob a
designer wants is either a script edit or a new host function.

---

## 3. Proposed behaviour

### 3.1 Proposed syntax

The entry generator declares its parameter block as an ordinary typed parameter:

```noiser
struct CritterTuning {
    flee_threshold: f32,
    roam_radius:    f32,
    sleep_hours:    f32,
    engage_ratio:   f32,
    pack_size:      i32,
}

pub gen fn deer(entity: i32, t: CritterTuning) -> i32 {
    loop {
        let (px, pz, _, _) = @fn.perceive_self(entity);
        let (tx, tz, _, seen) = @fn.perceive_threat(entity);
        if seen > 0.0 && dist2(px, pz, tx, tz) < t.flee_threshold * t.flee_threshold {
            @fn.intent_move_to(entity, px - (tx - px), pz - (tz - pz));
        } else {
            @fn.intent_move_to(entity, wander_x(px, t.roam_radius), wander_z(pz, t.roam_radius));
        }
        yield 0;
    }
}
```

Rules:

- The **leading `i32` parameters keep today's meaning** and are still filled from
  `warp_id.xyz` (so `entity: i32` above is unchanged from what badlands does).
- **At most one non-scalar parameter** is permitted, and it must be last. It is
  the *params block*, supplied by the host at `Prepare`.
- Its type must be a `struct` (or tuple) of scalars, fixed-size arrays and nested
  structs — i.e. anything the existing `SerType` wire format
  (`docs/NOISER_DATA_EXCHANGE.md`) can already encode. No heap types
  (`Vec`, `String`) in v1.
- The block is **immutable inside the generator** and lives for the context's
  lifetime, so it survives across yields with no per-tick cost.
- If a generator declares a params block and the host does not supply one, that
  is an error at `Prepare` — not a zero-fill (§3.4).

### 3.2 Proposed C++ API

```cpp
/// Reflection for one entry point's parameter list.
struct GeneratorParamInfo {
  std::string_view name;          // "entity", "t"
  const TypeReflection* type;     // I32 for the warp-filled leaders; struct for the block
  bool from_warp_id;              // true for the leading scalars
};

class NoiserProgram {
  /// Parameters of a generator entry (see the named-entry-points request for
  /// GeneratorHandle; the default entry is GeneratorHandle{0}).
  [[nodiscard]] std::span<const GeneratorParamInfo> GetGeneratorParams(
      GeneratorHandle entry) const;

  /// Reflection for the trailing params block, or nullptr if the entry has none.
  /// Use with TypeReflection::ByteOffsetOf() to build the blob field by field,
  /// or to validate a host struct's layout once at load.
  [[nodiscard]] const TypeReflection* GetGeneratorParamsType(
      GeneratorHandle entry) const;

  /// Prepare a context, supplying the params block as wire-format bytes
  /// (NOISER_DATA_EXCHANGE.md encoding — LE, declaration order, no padding).
  [[nodiscard]] std::expected<ExecutionContext, CompileError> PrepareWithParams(
      GeneratorHandle entry, const NoiserInput& input,
      std::span<const uint8_t> params_bytes) const;

  /// Typed convenience for a host POD whose layout has been validated once
  /// against GetGeneratorParamsType().
  template <typename T>
  [[nodiscard]] std::expected<ExecutionContext, CompileError> PrepareWithParams(
      GeneratorHandle entry, const NoiserInput& input, const T& params) const;
};
```

Host usage in badlands terms:

```cpp
// once, at load: resolve + validate against the C++ struct
const auto* ty = prog.GetGeneratorParamsType(*prog.GetGeneratorLocation("deer"));
assert(ty && ty->ByteSize() == sizeof(CritterTuning));
assert(ty->ByteOffsetOf("roam_radius") == offsetof(CritterTuning, roam_radius));

// per spawn: the species' tuning row, loaded from a designer-owned data file
auto ctx = prog.PrepareWithParams(deer_entry,
                                  NoiserInput{.warp_id = {slot, 0, 0}},
                                  tuning_table[CreatureKind::Deer]);
```

### 3.3 Proposed VM / FFI plumbing

The mechanism already exists on the resume path and is reused wholesale:

- The compiler emits a **params type descriptor** per generator-table entry
  (same encoding as the existing `yield_type` / `resume_type` descriptors that
  `ParseGeneratorMetadata` already parses, `noiser-vm/src/noiser.cpp:842-863`),
  plus `param_count` split into `warp_param_count` + `has_params_block`.
- `deserialize_typed_value` + `resume_abi_layout` (`type_utils.rs`) already turn
  a wire blob into stack slots for `ResumeWithTypedValues`. `PrepareWithParams`
  runs the same decode once, at context creation, and writes the slots into the
  generator's initial locals exactly where `warp_id` values are written today.
- New FFI entry point, additive:

```c
/// Create a generator context, seeding the trailing params block from
/// TypeDescriptor-encoded bytes. params == NULL is equivalent to
/// noiser_vm_prepare_context() and is an error if the entry declares a block.
NoiserVmContext* noiser_vm_prepare_context_with_params(
    NoiserVmProgram* program, NoiserVmInput input,
    const uint8_t* params, uint32_t params_len);
```

### 3.4 Validation and error semantics

Unlike the resume path — which is documented as accepting adversarial buffers
and therefore zero-fills on truncation (`NOISER_DATA_EXCHANGE.md`,
"Untrusted-input contract") — the params path is **host setup code and must fail
loudly**:

| Situation | Result |
|---|---|
| `params_bytes.size()` != the declared block's wire size | `std::unexpected(CompileError{ .message = "params block for 'deer' is 24 bytes, expected 20" })`; no context created |
| Entry declares a block, host called plain `Prepare` | `std::unexpected(... "generator 'deer' requires a params block; use PrepareWithParams")` |
| Entry declares no block, host passed params | `std::unexpected(... "generator 'deer' takes no params block")` |
| Params type contains a heap type (`Vec`, `String`) | **Compile error** at the declaration, not a runtime surprise |
| More than one non-scalar parameter, or a non-scalar not last | **Compile error** naming the offending parameter |
| Declared `f32`/struct parameter with no params support (today's §2a) | Becomes the compile error above instead of silent bit-reinterpretation |

The last row is the single most valuable part of this proposal even if nothing
else lands: **today's silence is the bug**.

### 3.5 Alternatives considered

**(A) Typed params argument on the generator — RECOMMENDED.** As specified above.

**(B) Host-provided read-only buffer** — `@buf.tuning: BufferView<CritterTuning>`
registered per context, extending `BufferView`'s element type to structs and
adding `GetBufferElementLayout(name)` so the host can lay out rows correctly.
This is the smallest delta from what exists (§2d already works for scalar
elements). Drawbacks: the table is a script *global*, not the entry's argument,
so nothing in a brain's signature says what data it needs and two entries in one
file cannot declare different blocks; the brain must be told its row index,
which puts us straight back to smuggling integers through `warp_id`; the host
must keep the backing memory alive for the context's lifetime and must not
reallocate the table while any brain is suspended (badlands spawns and despawns
creatures continuously — a `std::vector` growth would dangle every registered
pointer); and there is no per-*entity* (as opposed to per-*type*) variation
without a row per entity.

**(C) `Prepare` takes an opaque byte span the script views as a declared
struct** — e.g. `@params.tuning: CritterTuning;` at file scope, `Prepare(input,
bytes)`. Mechanically almost identical to (A) and it would work. Drawbacks: it
adds a fourth `@`-namespace to a language that already has `@fn.`/`@uni.`/`@buf.`
for host data; it is file-global, so it does not compose with named entry points
(each entry wants its own block); and it is invisible at the call site, where a
reader of `pub gen fn deer(entity: i32)` gets no hint that the brain is
parameterised.

**(D) Extend `NoiserInput` with more integer/float vectors.** Rejected: it scales
by four values at a time, is positional and untyped, and institutionalises
exactly the `warp_id.x`-means-entity-id smuggling this request exists to
retire.

**(E) Uniform-block improvements** — `ctx.SetUniformStruct(handle, span)`,
`GetUniformLocation("tuning.flee")` for named field addressing, fix array
uniforms, fix the lowercase-`vec4` ICE. Not an alternative so much as a
**complementary small step**: it makes the channel that already works safe, and
it is worth landing on its own. It does not remove the need for (A), because
uniforms remain script-global and therefore cannot give each entry point in a
multi-brain file its own parameter block.

**Recommendation: (A)**, with (E) as the cheap interim fix.

Reasoning:

1. **It is an argument, so it is where the reader is.** `pub gen fn deer(entity:
   i32, t: CritterTuning)` states the brain's data contract in its signature.
   Every other option hides it in a file-global declaration.
2. **Per-instance by construction.** Each `ExecutionContext` gets its own copy —
   one brain per entity is exactly noiser's generator model, and per-entity
   variation ("this deer is bold") costs nothing extra over per-species.
3. **Composes with named entry points.** `deer`, `wolf` and `boar` in one
   `critters.noiser` can each declare a different params type, checked
   independently. Globals cannot do this.
4. **Reuses machinery that already exists and is already tested.** The
   `SerType`/`TypeDescriptor` wire format, `deserialize_typed_value`, the
   physical-vs-wire slot-count invariant, and `TypeReflection::ByteOffsetOf` all
   ship today for yields and typed resumes. Landing (A) is mostly plumbing a
   fourth descriptor through the generator table.
5. **It converts today's silent corruption into a diagnostic.** A struct
   parameter on a generator is currently accepted and filled with reinterpreted
   integers; under (A) it is either correct or a named error.
6. **It is checkable at load.** `GetGeneratorParamsType` + `ByteOffsetOf` lets
   the host assert the script's block matches the C++ struct once, at startup —
   the same bind-once/read-many discipline the rest of the API already
   encourages — instead of discovering a field-order mismatch as subtly wrong
   creature behaviour.

---

## 4. Use cases (badlands)

**Per-creature-type factor blocks as data.** The four archetypes share one
script per archetype (see the companion request) and differ only by a tuning row
that designers own:

```noiser
// scripts/brains/critters.noiser
struct CritterTuning {
    flee_threshold: f32,   // distance at which the animal breaks and runs
    roam_radius:    f32,   // wander radius around its home range
    sleep_hours:    f32,   // hours of the day-night cycle spent bedded down
    engage_ratio:   f32,   // hp ratio above which it will fight instead of flee
}

pub gen fn deer(e: i32, t: CritterTuning) -> i32 { ... }
pub gen fn wolf(e: i32, t: CritterTuning) -> i32 { ... }
pub gen fn boar(e: i32, t: CritterTuning) -> i32 { ... }
0.0
```

```
# assets/creatures.toml — designer-owned, no script edit, no rebuild
[deer]  flee_threshold = 12.0  roam_radius = 18.0  sleep_hours = 6.0  engage_ratio = 0.0
[wolf]  flee_threshold =  3.0  roam_radius = 40.0  sleep_hours = 4.0  engage_ratio = 0.4
[boar]  flee_threshold =  1.5  roam_radius = 10.0  sleep_hours = 7.0  engage_ratio = 0.8
```

"All critters share one script with different factors — deer flee more" becomes
literally true, and "treat behaviour as data" stops meaning "recompile the
script".

Concrete wins:

1. **Designer-tunable without touching scripts.** Today `ROAM_RADIUS`,
   `FATIGUE_GO_HOME`, `BOREDOM_TAVERN` are `const`s in `hero.noiser` (§2e).
   Under this proposal they are rows in a data file that hot-reloads without a
   noiser compile at all.
2. **Per-entity variation for free.** A "bold" individual is a modified copy of
   its species row at spawn — no new script, no new host call.
3. **Parity checking.** badlands keeps a C++ town brain in lockstep with the
   noiser one; a shared params struct means both read the same table instead of
   two hand-synchronised constant sets.
4. **The schedule table case.** `sleep_hours` generalises to a fixed-size array
   of per-hour weights — expressible as a struct field, impossible as a uniform
   (§2c: array uniforms read zero) and awkward as a host call (one call per hour
   per entity).
5. **Removes host-call ceremony.** Every tunable currently costs a `@fn.`
   declaration in the script, a C++ free function, a `bind(...)` line in
   `BrainRuntime::create`, and a smoke-test assertion. A params block costs one
   struct field.

---

## 5. Why the workarounds are inadequate

**(a) One `@fn.` host call per factor, per entity, per tick.** This is the
pattern the current API forces. Measured on the pinned sha, VM backend, release
Rust / `-O1` C++:

| Program | ns per `Resume` | notes |
|---|---|---|
| generator with 0 host calls | 105 ns (profiling on) / 100 ns (off) | baseline |
| generator with 4 host calls | 355 ns (on) / 283 ns (off) | 200 000 resumes each |

That is **≈62 ns per host call with the default settings, ≈46 ns with profiling
disabled** — the delta being the two `high_resolution_clock::now()` samples the
trampoline takes per call. Four factors × 1 000 creatures × 30 Hz = 120 000
calls/s ≈ 7.4 ms of CPU per wall-clock second (≈0.25 ms per tick) purely to
re-read four numbers that never change for the lifetime of the entity. The cost
is not catastrophic on its own; it is the *shape* that is wrong — it scales as
`factors × entities × tick_rate` for data with zero time variation, and it grows
every time a designer wants one more knob.

**Host-call profiling is on by default.** `g_host_call_profiling_enabled = true`
(`noiser-vm/src/noiser.hpp:2199`) and `HostThunkTrampoline`
(`noiser.hpp:2209-2222`) brackets every call with two clock reads unless the host
knows to call `detail::SetHostCallProfiling(false)` — a function living in
`detail`, which does not read like the public switch. So the default cost of the
"pull each factor through a host call" pattern is the 62 ns column, not the 46 ns
one. (Already filed as `docs/noiser-feedback.md` item 3.)

Beyond the cycles: values that arrive through `@fn.` cannot be constant-folded,
cannot be validated at load, and put the tuning table in C++ — the opposite of
"behaviour as data".

**(b) `const` in the script.** What `hero.noiser` does today (§2e). Designers
must edit noiser source; every variant needs its own copy of the file, which
collides head-on with the one-entry-point-per-file limit (companion request); and
the values cannot differ per entity.

**(c) Smuggling through `warp_id`.** Three `i32`s total, one already spent on the
entity id. Adding a fourth parameter silently yields `0` (§2b); making one an
`f32` silently yields reinterpreted integer bits (§2a/§2b). It is untyped,
positional, and undiagnosed.

**(d) Uniforms.** They genuinely work per context (§2c) and are the best current
option — but: fields are addressed by `base_loc + index` with no
`GetUniformLocation("tuning.flee")` and no whole-struct setter, so reordering
two `f32`s in the script silently swaps two behaviours with no error anywhere;
array uniforms silently read `0`, which rules out any table-shaped tuning;
`@uni.x: vec4` ICEs the compiler; and the namespace is file-global, so the
multi-brain file this whole design depends on cannot give each brain its own
block. Uniforms are a fine *interim*, and §3.5(E) says what would make them
safe, but they are not the argument-shaped channel.

**(e) Buffer views.** Also genuinely work per context (§2d). But the row index
still has to reach the brain through `warp_id`; the host must guarantee the
backing storage neither moves nor dies while any brain is suspended (badlands
spawns/despawns continuously); element types are scalars/tuples, so named fields
are lost; and it is a global, not an argument, with the same
one-block-per-file consequence as uniforms.

---

## 6. Acceptance criteria — example verification programs

### V1 — a typed params block reaches the brain

```noiser
@fn.report: fn(x: f32) -> void;

struct CritterTuning { flee_threshold: f32, roam_radius: f32, sleep_hours: f32, engage_ratio: f32 }

pub gen fn deer(entity: i32, t: CritterTuning) -> i32 {
    loop {
        @fn.report(t.flee_threshold);
        @fn.report(t.roam_radius);
        @fn.report(t.sleep_hours);
        @fn.report(t.engage_ratio);
        yield 0;
    }
}
0.0
```

```cpp
struct CritterTuning { float flee_threshold, roam_radius, sleep_hours, engage_ratio; };
auto ctx = prog.PrepareWithParams(entry, NoiserInput{.warp_id = {5,0,0}},
                                  CritterTuning{12.0f, 18.0f, 6.0f, 0.0f});
prog.Resume(*ctx);
```

**Expected**: `report(12) report(18) report(6) report(0)`, identical on every
tick (the block is immutable and survives yields), with `entity == 5` still
arriving from `warp_id.x`.
**Today**: compiles clean and prints `report(8.40779e-45) report(0) report(0)
report(0)` — `warp_id.y`'s bits in field 0, zeros after (§2a).

### V2 — three brains, three tuning rows, one script (composes with named entry points)

```noiser
@fn.report:   fn(x: f32) -> void;
@fn.report_i: fn(x: i32) -> void;

struct CritterTuning { flee_threshold: f32, roam_radius: f32 }

pub gen fn deer(e: i32, t: CritterTuning) -> i32 { loop { @fn.report_i(1); @fn.report(t.flee_threshold); yield 0; } }
pub gen fn wolf(e: i32, t: CritterTuning) -> i32 { loop { @fn.report_i(2); @fn.report(t.flee_threshold); yield 0; } }
pub gen fn boar(e: i32, t: CritterTuning) -> i32 { loop { @fn.report_i(3); @fn.report(t.flee_threshold); yield 0; } }
0.0
```

```cpp
struct Row { const char* name; float flee, roam; };
for (auto r : {Row{"deer",12.f,18.f}, Row{"wolf",3.f,40.f}, Row{"boar",1.5f,10.f}}) {
    auto e   = prog.GetGeneratorLocation(r.name).value();
    auto ctx = prog.PrepareWithParams(e, input, CritterTuning{r.flee, r.roam});
    prog.Resume(*ctx);
}
```

**Expected**: `1 12`, `2 3`, `3 1.5` — deterministically, with all three
contexts allowed to coexist.
**Today**: neither piece exists. `Prepare` runs one arbitrary generator (20
loads of the equivalent three-generator file: 7×deer, 7×wolf, 6×boar) and the
tuning fields are garbage as in V1.

### V3 — errors are named, and existing scripts are untouched

```noiser
// V3a — no params block: must behave exactly as it does today
@fn.report: fn(x: f32) -> void;
pub gen fn brain(entity: i32) -> i32 { loop { @fn.report(1.0); yield 0; } }
0.0
```
```noiser
// V3b — declares a block; host must be forced to supply one
@fn.report: fn(x: f32) -> void;
struct Tuning { a: f32 }
pub gen fn brain(entity: i32, t: Tuning) -> i32 { loop { @fn.report(t.a); yield 0; } }
0.0
```
```noiser
// V3c — heap type in the block: must be a compile error
struct Bad { xs: Vec<f32> }
pub gen fn brain(entity: i32, b: Bad) -> i32 { loop { yield 0; } }
0.0
```

**Expected**:
- V3a: `prog.Prepare(input)` succeeds; `report(1)` per tick;
  `GetGeneratorParamsType(entry) == nullptr`. Byte-identical behaviour to today.
- V3b: `prog.Prepare(input)` returns an error mentioning `brain` and
  `PrepareWithParams`; `PrepareWithParams(entry, input, std::span<const
  uint8_t>{})` returns `"params block for 'brain' is 0 bytes, expected 4"`;
  the correct 4-byte call succeeds and reports the supplied value.
- V3c: compile fails with a message naming `Bad::xs` and the unsupported type.

**Today**: V3a works. V3b compiles and silently reports `warp_id.y` reinterpreted
as `f32` — `report(0)` for a default `NoiserInput`, no error at any layer. V3c
**also compiles clean and runs** (`COMPILE OK`, resumes fine, no host calls): a
`Vec<f32>` inside a generator parameter is accepted with no diagnostic, because
the parameter is never validated at all.

---

## 7. Backward compatibility

- **`NoiserInput` is unchanged.** `{warp_id, warp_size}` keeps its meaning and
  its `Reset` semantics; the params block is a separate, additive argument.
  badlands' `warp_id.x`-carries-the-entity-slot pattern keeps working verbatim,
  and remains the recommended encoding for the entity id.
- **`Prepare(input)` is unchanged** for every generator that declares only
  warp-filled scalar parameters — i.e. every generator that exists today. All
  in-tree noise scripts and generator tests fall in this class.
- **The warp fill rule is unchanged**: leading `i32` parameters continue to take
  `warp_id.x/y/z`, still capped at three.
- **Only genuinely new behaviour is added at the point where today's behaviour is
  undefined.** A struct/tuple/`f32` parameter on a generator currently produces
  reinterpreted-integer garbage; no correct program can depend on that, so
  turning it into "supplied by the host, or a compile/`Prepare` error" breaks
  nothing real. The one observable change is that a script which *accidentally*
  declared such a parameter now fails loudly — which is the intent.
- **Bytecode format**: one extra `TypeDescriptor` per generator-table entry
  (`params_type`, `Void` when absent) plus a `has_params_block` bit. Needs a
  version bump; readers of the older format see `Void` and behave as today. The
  C++ `ParseGeneratorMetadata` already parses two optional descriptors per entry
  in exactly this style, so the parse is a copy of an existing block.
- **Uniforms and buffer views are untouched.** Scripts already using `@uni.` or
  `@buf.` keep working; the §3.5(E) improvements are additive
  (`SetUniformStruct`, dotted uniform names) and change no existing signature.
- **`ResumeWithTypedValues` and the resume wire format are untouched**; the params
  decode reuses them read-only.

---

## 8. Related

- `docs/noiser-feedback.md` **item 10** — named generator entry points. The two
  requests are designed together: named entries give one file many brains,
  structured parameters give each of those brains its own tuning data. Either
  alone is half the feature.
- `docs/noiser-feedback.md` **item 8** — "Generator state snapshot for hot
  reload." A params block is the cheap 80% of that item for badlands' case: on
  reload the host re-`Prepare`s each brain with the same params, so a restarted
  brain at least resumes with its identity and tuning intact rather than with
  the script's compile-time defaults. Full state snapshotting remains the
  general answer.
- `docs/noiser-feedback.md` item 3 — host-call profiling defaults to on; the
  measured 62 ns vs 46 ns per call in §5(a) is that item's cost, and it is the
  cost the "one host call per factor per tick" workaround pays today.
- `docs/noiser-bugs-upstream/2026-07-22-feature-named-generator-entry-points.md`
  — companion request.
- `third_party/noiser/docs/NOISER_DATA_EXCHANGE.md` — the wire format this
  proposal reuses, including the truncation/zero-fill policy that §3.4
  deliberately does **not** apply to host setup data.
- Code touched: `noiser-vm/src/noiser.hpp` (`NoiserInput:817`, `SetUniform`
  overloads, `RegisterBufferByName:1143`, `Prepare:1339`,
  `ResumeWithTypedValues:1417`, `HostThunkTrampoline:2209`);
  `noiser-vm/src/noiser.cpp:830-874` (`ParseGeneratorMetadata`);
  `noiser-compiler/src/bytecode/header.rs:572-578`;
  `noiser-compiler/src/bytecode/serialization.rs:1843-2040`;
  `noiser-compiler/src/bytecode/type_utils.rs` (`resume_abi_layout`).
- badlands consumers: `game/src/brain.cpp` (`spawn_brain`, the `bind(...)`
  block), `scripts/brains/hero.noiser` (the `const` block and `profile_for`).
- Separable defects noticed while testing this area, not filed on their own yet:
  array uniforms silently read `0`; `@uni.x: vec4 = vec4(...)` ICEs with
  `type_descriptor_to_abi_layout: Unknown has no physical layout`
  (`abi_layout.rs:282`) while `Vec4` works.
