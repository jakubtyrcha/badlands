# Named generator entry points — select a `gen fn` by name at `Prepare` time

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Kind**: feature request
**Status**: new

**Discovered in**: badlands — a colony/hero game that runs one noiser generator
per entity as its game-AI brain (`scripts/brains/hero.noiser`,
`game/src/brain.cpp`), now growing from one archetype to four (heroes, townfolk,
critters, monsters) with several variants each.
**Backend observed**: VM (`NoiserBackend::kVM`). WASM/JIT not tested.

---

## 1. Summary

A compiled noiser program exposes exactly one generator entry point, and there
is no way for the host to say *which* one. `parse_first_generator`
(`noiser-compiler/src/bytecode/serialization.rs:1861`) and its C++ twin
`ParseGeneratorMetadata` (`noiser-vm/src/noiser.cpp:830`) both walk the bytecode
generator table — which **already stores a name per generator** — and keep only
entry `[0]`, discarding every other name and address. We would like
`program.PrepareEntry("wolf", input)`: one `critters.noiser` holding `deer`,
`wolf` and `boar`, one `townfolk.noiser` holding `peasant`, `taxman` and
`guard`, each a real generator, selected by name by the host at spawn.

### Scope: this is a feature request for *named selection* only

While measuring the current behaviour we found that today's **implicit**
selection is not merely "the first generator in the file" — it is
non-deterministic: the compiler stores generators in a `HashMap<String, GenDef>`
and serialises the table by iterating it, so entry `[0]` is chosen by hash order
and re-rolls on every compile. A file containing two generators — including the
completely ordinary case of one private helper `gen fn` above the brain — runs a
coin-flip choice of entry point, with no diagnostic.

**That is a separate correctness bug and is filed separately**, in
[`2026-07-22-entry-point-selection-is-nondeterministic.md`](2026-07-22-entry-point-selection-is-nondeterministic.md)
(independently reproduced; it also confounded another in-flight bug
investigation). **It must be fixed regardless of whether this feature lands** —
it breaks scripts that will only ever have one brain plus a helper, and it is
not something a host can work around.

This document assumes that fix and asks for the orthogonal capability on top of
it: **letting the host name the entry point it wants**. The evidence in §2 is
retained because it also demonstrates *why* an implicit rule is the wrong
interface for the use case, but the full measurement set, root-cause analysis
and verification programs for the determinism defect live in the bug report, not
here.

---

## 2. Current behaviour

All snippets were run against the pinned sha with a minimal C++ harness
(compile on a 64 MiB-stack thread → `BindCallableByName` for the `@fn.`s the
script declares → `FreezeHostThunks()` → `Prepare` → `Resume` ×N). `report_i`
prints its argument, so the printed number identifies *which* generator is
running.

### 2a. Three brains in one file: the entry point is chosen at random

```noiser
@fn.report_i: fn(x: i32) -> void;

pub gen fn deer(e: i32) -> i32 { loop { @fn.report_i(1); yield 0; } }
pub gen fn wolf(e: i32) -> i32 { loop { @fn.report_i(2); yield 0; } }
pub gen fn boar(e: i32) -> i32 { loop { @fn.report_i(3); yield 0; } }
0.0
```

One load, verbatim:

```
COMPILE OK
    report_i(2)
  tick 0: yield: i32=0 f32=0 (4 bytes)
    report_i(2)
  tick 1: yield: i32=0 f32=0 (4 bytes)
DONE (2 host calls over 2 ticks)
```

`wolf` ran. Not `deer`. Re-running the identical binary on the identical source
20 times:

```
   7 report_i(1)      <- deer
   7 report_i(2)      <- wolf
   6 report_i(3)      <- boar
```

There is no host API that could have expressed a preference: `Prepare` takes
only `NoiserInput`.

### 2b. A private helper `gen fn` hijacks the entry point ~60% of the time

Visibility is not consulted. A non-`pub` generator competes with the `pub` one:

```noiser
@fn.report_i: fn(x: i32) -> void;
@fn.report:   fn(x: f32) -> void;
gen fn other(x: i32) -> i32 { loop { @fn.report_i(7); yield 7; } }
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(42.0); yield 0; } }
0.0
```

20 loads:

```
  12 report_i(7)      <- the private helper `other` became the program entry
   8 report(42)       <- the intended `pub gen fn brain`
```

This is the shape that bites in practice, because it is what any attempt to
factor a brain into sub-behaviours produces. It also silently changes what the
program yields — `other` yields `7`, `brain` yields `0` — and, when the two
generators have different yield types, what `GetYieldType()` reports: a file
whose helper is `gen fn steps() -> f32` and whose brain is
`pub gen fn brain(e: i32) -> i32` was observed yielding `f32 1.0, 2.0, 3.0` on
the loads where the helper won the lottery.

### 2c. Non-determinism is per *compile*, not per process

Two `NoiserProgram::Compile` calls on the same source inside one process can
disagree (8 runs of a two-generator file, `alpha`=1, `beta`=2):

```
compile#1 -> report_i(1)   compile#2 -> report_i(2)
compile#1 -> report_i(1)   compile#2 -> report_i(1)
compile#1 -> report_i(2)   compile#2 -> report_i(2)
compile#1 -> report_i(2)   compile#2 -> report_i(1)
compile#1 -> report_i(1)   compile#2 -> report_i(1)
compile#1 -> report_i(1)   compile#2 -> report_i(2)
compile#1 -> report_i(1)   compile#2 -> report_i(1)
compile#1 -> report_i(1)   compile#2 -> report_i(1)
```

So a hot-reload of an unchanged file can silently swap the running brain.

### 2d. The names are already in the bytecode; both parsers throw them away

The generator table is
`[gen_count: u8]` then per entry
`[name_len: u8][name bytes][yield_type: TypeDescriptor][address: u32][context_size: u16][param_count: u8][resume_type: TypeDescriptor]`.

`GeneratorInfo` (`serialization.rs:1843`) even has a `pub name: String` field.
`parse_first_generator` parses entry 0 and returns; `ParseGeneratorMetadata`
(`noiser.cpp:830-874`) loops over **all** entries, reads each name length, skips
the bytes, and then guards every store with `if (i == 0)`. The data needed for
this feature is already produced and already reaching the C++ side.

### 2e. Related current limitation: `pub gen fn` cannot be imported

Modules are otherwise a working sharing mechanism (`FileModuleResolver`); a
`pub fn` imports and runs. A `pub gen fn` does not:

```noiser
// mods/behaviour.noiser
pub gen fn shared_brain(e: i32) -> i32 { loop { @fn.report(77.0); yield 0; } }
```
```noiser
// main
import { shared_brain } from behaviour;
pub gen fn deer(e: i32) -> i32 { var g = shared_brain(e); loop { @fn.report(1.0); yield 0; } }
0.0
```
```
COMPILE FAIL: undefined function: behaviour::shared_brain
```

(Importing it without instantiating it compiles and is dead-code-eliminated —
10/10 loads kept `deer` as the entry.) This matters here because it removes the
obvious "just put the shared coroutine in a module" answer: today a brain
generator can only live in the file that is compiled as the program.

---

## 3. Proposed behaviour

### 3.1 Script syntax — none required

Generator names are already emitted. No new keyword is needed. `pub` becomes
meaningful rather than decorative:

- `pub gen fn <name>` — an **entry point**: addressable by name from the host,
  and included in the reflected roster.
- `gen fn <name>` — internal only: usable as a sub-generator, never selectable
  as an entry point.

An optional attribute could mark the default when a program wants one:

```noiser
#[entry]                 // optional; at most one per program
pub gen fn deer(e: i32) -> i32 { ... }
```

### 3.2 Compiler / Rust side

1. **Make the table deterministic (source order).** Replace
   `generators: HashMap<String, GenDef>` (`bytecode/mod.rs:2148`) with an
   insertion-ordered map (`IndexMap`, or `Vec<(String, GenDef)>` + a name index).
   `header.rs:577-578` then emits source order. Also fix
   `bytecode/mod.rs:1367`, `compiler.generators.keys().next().cloned()`, which
   picks the standalone-generator name the same arbitrary way.
   *This alone removes the coin flip and is worth landing independently.*
2. **Record visibility.** Add a `FLAG_GEN_PUB` bit to each generator-table entry
   (there is a spare byte slot next to `param_count`; or bump the table entry
   with an explicit `flags: u8`). Bytecode version bump, old readers ignore it.
3. **Expose the whole table.**

```rust
/// All generators, in source order. Entry 0 is the default entry point.
pub fn parse_generators(bytecode: &[u8]) -> Vec<GeneratorInfo>;

/// Look up one by name (accepts `name` or `module::name`).
pub fn find_generator(bytecode: &[u8], name: &str) -> Option<GeneratorInfo>;

/// Unchanged; becomes `parse_generators(bc).into_iter().next()`.
pub fn parse_first_generator(bytecode: &[u8]) -> Option<GeneratorInfo>;
```

4. **Make the entry a property of the context, not the program.** Today
   `noiser_vm_set_generator_info(program, entry_point, context_size,
   is_standalone)` (`noiser_vm_ffi.h:170`) is called once per program at load
   (`noiser.cpp:2385`, `noiser.cpp:3978`), so a program has a single entry
   forever. badlands needs a `deer` context and a `wolf` context **alive at the
   same time from one program**, so the entry must travel with the context:

```c
/// Create a context whose entry point is the given generator.
/// Falls back to the program-level generator info when entry_point == 0.
NoiserVmContext* noiser_vm_prepare_context_with_entry(
    NoiserVmProgram* program, NoiserVmInput input,
    uint32_t entry_point, uint16_t context_size, uint8_t param_count,
    const uint8_t* yield_type_desc, uint32_t yield_type_desc_len);
```

### 3.3 C++ API

```cpp
/// One row of the bytecode generator table.
struct GeneratorInfo {
  std::string_view name;                 // as written in the script
  std::string_view module;               // "" for the main module
  bool is_pub = false;
  uint32_t address = 0;                  // absolute entry address
  uint16_t context_size = 0;
  uint8_t param_count = 0;
  const TypeReflection* yield_type = nullptr;
  const TypeReflection* resume_type = nullptr;
};

/// Opaque handle; resolve once during setup, reuse per spawn (hot path).
struct GeneratorHandle { uint16_t index; };

class NoiserProgram {
  /// Every generator in the program, in source order. Empty for non-generator
  /// programs. Lets a host validate a script's brain roster at load time.
  [[nodiscard]] std::span<const GeneratorInfo> GetGenerators() const;

  /// Resolve a generator by name. Accepts "deer" or "critters::deer".
  /// nullopt if absent or if the name resolves to a non-`pub` generator.
  [[nodiscard]] std::optional<GeneratorHandle> GetGeneratorLocation(
      std::string_view name) const;

  /// Prepare a context on a named entry point.
  [[nodiscard]] std::expected<ExecutionContext, CompileError> PrepareEntry(
      GeneratorHandle entry, const NoiserInput& input) const;

  /// Convenience overload; resolves the name each call.
  [[nodiscard]] std::expected<ExecutionContext, CompileError> PrepareEntry(
      std::string_view name, const NoiserInput& input) const;

  /// Unchanged. See §7 for what it means on a multi-generator program.
  [[nodiscard]] ExecutionContext Prepare(const NoiserInput& input) const;
};
```

Mirrored on `ExecutionContext::Reset(entry, input)` so a pooled context can be
re-pointed at another brain without reallocating.

### 3.4 Error semantics

| Situation | Result |
|---|---|
| Name not found | `std::unexpected(CompileError{ .message = "no generator named 'wolf'; available: deer, boar" })`. No context is created. |
| Name found but not `pub` | `std::unexpected(... "generator 'helper' is not `pub` and cannot be an entry point")`. |
| Name resolves to a plain `fn`, not a `gen fn` | `std::unexpected(... "'observe' is a function, not a generator")`. |
| Program is not a generator program at all | `std::unexpected(... "program has no generators")`. |
| Duplicate names in one module | Already a source-level redefinition error; unchanged. |
| Same name in two imported modules | Ambiguity error listing both; resolve with `critters::deer`. |
| `GetGeneratorLocation` on a stale handle after hot-reload | Handles are indices into a specific `NoiserProgram`; a new program means re-resolving. Document it; `PrepareEntry(std::string_view)` avoids the trap. |

Missing-name **must not** fall back to entry 0 — silently running the wrong
brain is the failure mode this request exists to remove.

### 3.5 Interaction with host binding and `GetCallableLocation`

Nothing changes, and that is the point:

- `@fn.`, `@uni.` and `@buf.` slot tables are **program-wide**, not per
  generator. `GetCallableLocation("perceive_self")`, `BindCallableByName`, and
  `FreezeHostThunks()` stay exactly as they are: bind once per program, before
  any `Prepare`/`PrepareEntry`. Verified today that a file's generators share
  one `@fn.` table (§2b: both generators' calls dispatch correctly from one
  binding set).
- A generator that never calls a given `@fn.` simply never touches that slot, so
  a `critters.noiser` declaring the union of what `deer`, `wolf` and `boar` need
  binds once and every entry works. badlands' existing "bind only what the
  script declares, probed via `GetCallableLocation`" pattern
  (`game/src/brain.cpp:286-310`) continues to work unmodified.
- Per-entry **yield/resume types** must come from the selected entry, not the
  program: `GetYieldType()` keeps returning the default entry's type;
  `GetGenerators()[h.index].yield_type` gives the selected one. Hosts that mix
  entries with different yield types read the per-entry reflection.

---

## 4. Use cases (badlands)

**One file per archetype instead of one file per variant.**

```noiser
// scripts/brains/critters.noiser
import { flee_score, wander_step, nearest_threat } from critter_common;

@fn.perceive_self:   fn(e: i32) -> (f32, f32, f32, f32);
@fn.perceive_threat: fn(e: i32) -> (f32, f32, f32, f32);
@fn.intent_move_to:  fn(e: i32, x: f32, z: f32) -> void;

pub gen fn deer(e: i32) -> i32 { /* flees early, wide roam */ }
pub gen fn wolf(e: i32) -> i32 { /* stalks, engages */ }
pub gen fn boar(e: i32) -> i32 { /* holds ground, charges */ }
0.0
```

```noiser
// scripts/brains/townfolk.noiser  — peasant/taxman/guard share the
// errand-and-schedule spine that hero.noiser already implements
pub gen fn peasant(e: i32) -> i32 { ... }
pub gen fn taxman(e: i32)  -> i32 { ... }
pub gen fn guard(e: i32)   -> i32 { ... }
0.0
```

Host side, in badlands terms (`game/src/brain.cpp`):

```cpp
// once, at load: compile + bind + freeze, exactly as today
auto runtime = BrainRuntime::create(game, source, err);

// once, at load: validate the roster against the game's creature kinds
for (const auto& g : runtime->program->GetGenerators())
    spdlog::info("brain script exports '{}' (pub={})", g.name, g.is_pub);

// per spawn: pick the brain for this creature kind
std::unique_ptr<BrainState> spawn_brain(const BrainRuntime& rt, uint32_t slot,
                                        std::string_view kind) {
    auto entry = rt.program->GetGeneratorLocation(kind);   // "deer" / "wolf"
    if (!entry) return nullptr;                            // fail loudly
    auto ctx = rt.program->PrepareEntry(
        *entry, NoiserInput{.warp_id = {int32_t(slot), 0, 0}});
    if (!ctx) return nullptr;
    return std::make_unique<BrainState>(std::move(*ctx));
}
```

Concrete wins:

1. **Module cohesion.** All critter brains see the same constants, the same
   perception surface, the same helper `fn`s, in one reviewable file. The four
   archetypes become four files instead of a dozen near-duplicates.
2. **One reload unit.** `game_reload_script` recompiles one file and every
   critter picks it up; today each variant is a separate compile + separate
   keep-last-good state machine.
3. **Load-time validation.** `GetGenerators()` vs the game's creature-kind enum
   catches "designer renamed `boar` to `hog`" at load, not as a silently wrong
   brain three minutes into a session.
4. **Debug UI.** The ImGui debug surface can list the roster and force a
   selected entity onto another brain (`PrepareEntry("wolf", …)`) — an
   A/B tool that is impossible today.
5. **Determinism.** The same script + same entity kind produces the same brain
   on every run, which is a hard requirement for the deterministic-sim/replay
   direction badlands is built around.

---

## 5. Why the workarounds are inadequate

**(a) One file per brain.** This is what badlands does today. It costs a
near-duplicate file per variant (`deer.noiser`, `wolf.noiser`, `boar.noiser`
differing in four constants and one branch), and it defeats module cohesion in a
way that cannot be recovered by importing, because **`pub gen fn` is not
importable** (§2e): only plain `fn` helpers can be shared. The coroutine-shaped
part — the part with the interesting sequencing and the part actually worth
sharing — must be copy-pasted into every file. It also multiplies the host side:
one `NoiserProgram`, one bind set, one freeze, one keep-last-good reload path
*per variant*.

**(b) In-script `if creature_kind == N` dispatch.** A single mega-brain that
branches on a host-supplied kind:

- pays the branch every tick for every entity, forever;
- makes every entity's generator context the size of the *union* of all
  archetypes' live locals (`context_size` is per generator; merging them merges
  their state), so a deer carries the wolf's stalking state;
- cannot be tuned per type — the constants are file-global (see the companion
  request on structured parameters);
- re-creates exactly the god-function that splitting into archetypes was meant
  to remove, and grows quadratically as variants multiply;
- and **is not actually safe today**: the moment that file contains a second
  generator — a helper, a sub-behaviour, anything — the entry point becomes a
  coin flip (§2b). So the workaround for the missing feature is blocked by the
  same defect.

**(c) "Just put the brain first in the file."** Does not work. Source position is
not consulted at all (§2a–2c): the table is emitted in `HashMap` order.

---

## 6. Acceptance criteria — example verification programs

Each program is given with the exact expected output after implementation and
with its measured behaviour today.

### V1 — select each brain by name from one file

```noiser
// critters.noiser
@fn.report_i: fn(x: i32) -> void;

pub gen fn deer(e: i32) -> i32 { loop { @fn.report_i(1); yield 0; } }
pub gen fn wolf(e: i32) -> i32 { loop { @fn.report_i(2); yield 0; } }
pub gen fn boar(e: i32) -> i32 { loop { @fn.report_i(3); yield 0; } }
0.0
```

```cpp
auto prog = *NoiserProgram::Compile(src);
prog.BindCallableByName("report_i", std::function<void(int32_t)>(print));
prog.FreezeHostThunks();
for (auto name : {"deer", "wolf", "boar", "wolf"}) {
    auto ctx = prog.PrepareEntry(name, NoiserInput{.warp_id = {0,0,0}});
    (void)prog.Resume(*ctx);
}
```

**Expected**: `1 2 3 2` — on every run, and with all four contexts allowed to
coexist before being resumed.
**Today**: `PrepareEntry` does not exist. `Prepare` gives one arbitrary brain
repeated four times; over 20 loads the choice was 7×`deer`, 7×`wolf`, 6×`boar`.

### V2 — a private helper generator never becomes the entry, and the roster reflects visibility

```noiser
@fn.report_i: fn(x: i32) -> void;
@fn.report:   fn(x: f32) -> void;

gen fn other(x: i32) -> i32 { loop { @fn.report_i(7); yield 7; } }

pub gen fn brain(e: i32) -> i32 { loop { @fn.report(42.0); yield 0; } }
0.0
```

```cpp
auto gens = prog.GetGenerators();
// gens == [ {name:"other", is_pub:false, param_count:1},
//           {name:"brain", is_pub:true,  param_count:1} ]   // source order
assert(!prog.GetGeneratorLocation("other"));                 // not pub
auto ctx = prog.PrepareEntry("brain", input);                // ok
```

**Expected**: `report(42)` on every tick, on every run, 100/100 loads;
`GetGenerators()` returns both rows in source order with correct `is_pub`;
`GetGeneratorLocation("other")` is `nullopt`;
`PrepareEntry("nope", …)` returns an error whose message lists `brain`.
**Today**: `GetGenerators()`/`GetGeneratorLocation` do not exist, and plain
`Prepare` ran the private `other` in **12 of 20 loads**.

### V3 — backward compatibility: a single-generator script is untouched

```noiser
@fn.report: fn(x: f32) -> void;
pub gen fn brain(entity: i32) -> i32 { loop { @fn.report(1.0); yield 0; } }
0.0
```

```cpp
auto ctx = prog.Prepare(NoiserInput{.warp_id = {5,0,0}});  // no entry name
```

**Expected**: identical to today — `report(1)` per tick; `GetGenerators()`
returns exactly one row named `brain`; `Prepare` and `PrepareEntry("brain", …)`
produce equivalent contexts; `GetYieldType()` unchanged.
**Today**: works (one candidate, so the lottery is a no-op). This program is the
regression guard for §7.

---

## 7. Backward compatibility

- **`NoiserInput` is untouched.** Entry selection is orthogonal to warp input.
- **`Prepare(input)` keeps its signature and stays the documented path for
  single-generator scripts** — the overwhelming majority of existing noise
  scripts, all of which have exactly one generator or none, where the proposal
  is a strict no-op.
- **Multi-generator programs under plain `Prepare`.** The current behaviour is
  unspecified-and-random, so no correct program can depend on it. Recommended:
  `Prepare` selects the **first `pub gen fn` in source order** (deterministic,
  matches what every author already believes) and logs a one-time warning naming
  the chosen entry when the program has more than one `pub` generator. A
  stricter alternative — return an error and force `PrepareEntry` — is
  defensible but would break any script that happens to have a helper generator
  today; the warning path is enough because the choice becomes deterministic.
- **`parse_first_generator` stays**, re-expressed over `parse_generators`, so
  existing Rust hosts and the ~15 in-tree call sites in
  `noiser-compiler/src/bytecode/*_tests.rs` keep compiling and now get a stable
  answer.
- **Bytecode version.** Adding a `pub` flag per table entry needs a format bump;
  readers of the old format treat every generator as `pub` (today's effective
  behaviour), so old bytecode keeps loading.
- **`noiser_vm_set_generator_info` stays** for the program-level default;
  `noiser_vm_prepare_context_with_entry` is additive and is only used by
  `PrepareEntry`.
- **Determinism change is observable but strictly an improvement**: a program
  whose entry point was previously random now consistently picks the
  source-first one. Any host that was accidentally relying on the old behaviour
  was already broken 50% of the time.

---

## 8. Related

- `docs/noiser-feedback.md` **item 10** — "Named generator entry points —
  `parse_first_generator` limits a file to one brain; a `gen fn` name table
  would allow `warrior`/`coward` variants per file." This report is that item,
  with the additional finding that the current selection is non-deterministic
  and that the name table already exists in the bytecode.
- `docs/noiser-feedback.md` **item 8** — "Generator state snapshot for hot
  reload." Named entry points make hot reload materially better even without
  snapshotting: one file reloads every critter brain, and the reloaded program's
  entry for a given creature kind is stable instead of a fresh coin flip.
- `docs/noiser-feedback.md` item 9 — one-call generator setup for Rust hosts;
  `parse_generators` + `find_generator` is the natural place to land it.
- `docs/noiser-bugs-upstream/2026-07-22-feature-structured-generator-parameters.md`
  — the companion request. The two compose: each named entry declares its own
  typed parameter block, which is what makes "all critters, one script,
  per-species tuning data" possible.
- `docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`
  — that report's non-deterministic "outcome (A): the entry generator's body
  never executes, zero host calls, no error" is consistent with the entry-point
  lottery documented here: its repros all place a second `gen fn` in the file,
  so some loads run the sub-generator as the program entry. Worth re-checking
  that report against a deterministic entry point before diagnosing the
  sub-generator machinery.
- Code touched by this proposal: `noiser-compiler/src/bytecode/mod.rs:1367`,
  `mod.rs:2148`, `header.rs:572-578`, `serialization.rs:1843-2040`;
  `noiser-vm/src/noiser.cpp:830-874`, `noiser.cpp:2385`, `noiser.cpp:3978`;
  `noiser-vm/src/noiser_vm_ffi.h:162-170`.
- badlands consumers: `game/src/brain.cpp` (`BrainRuntime::create`,
  `spawn_brain`, `resume_brain`), `scripts/brains/hero.noiser`.
