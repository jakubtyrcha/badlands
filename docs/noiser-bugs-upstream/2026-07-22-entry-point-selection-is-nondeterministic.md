# Generator entry-point selection is non-deterministic — a file with 2+ `gen fn` runs an arbitrary one

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Label**: `compiler`
**Status**: new

**Discovered in**: badlands — a game running one noiser generator per entity as
its AI brain (`game/src/brain.cpp`, `scripts/brains/hero.noiser`).
**Backend observed**: VM (`NoiserBackend::kVM`). WASM/JIT not tested, but the
root cause is in the shared compiler, so both backends are presumed affected.
**Independently reproduced** by a second investigator (measurements in §Actual).

## Problem

When a script contains more than one `gen fn`, which one becomes the program's
entry point is **chosen at random at compile time**. The compiler holds
generators in a `HashMap<String, GenDef>` and serialises the bytecode generator
table by iterating it, so table order is Rust hash order; the VM host then takes
table entry `[0]` as the entry point. Source order is not consulted, `pub` is not
consulted, and there is no diagnostic. Two compiles of byte-identical source —
even inside a single process — can run different generators.

## Minimum Reproduction

Three brains in one file. `report_i` prints its argument, so the printed number
says which generator is running:

```noiser
@fn.report_i: fn(x: i32) -> void;

pub gen fn deer(e: i32) -> i32 { loop { @fn.report_i(1); yield 0; } }
pub gen fn wolf(e: i32) -> i32 { loop { @fn.report_i(2); yield 0; } }
pub gen fn boar(e: i32) -> i32 { loop { @fn.report_i(3); yield 0; } }
0.0
```

Harness: compile on a 64 MiB-stack thread → `BindCallableByName("report_i", …)`
→ `FreezeHostThunks()` → `Prepare(NoiserInput{})` → `Resume`. Load the same
source repeatedly and tally which number is printed.

The variant that matters more in practice — an ordinary private helper
generator sitting above the brain, which is what any attempt to factor a brain
into sub-behaviours produces:

```noiser
@fn.report_i: fn(x: i32) -> void;
@fn.report:   fn(x: f32) -> void;

gen fn other(x: i32) -> i32 { loop { @fn.report_i(7); yield 7; } }

pub gen fn brain(e: i32) -> i32 { loop { @fn.report(42.0); yield 0; } }
0.0
```

## Expected

Entry-point selection must be **deterministic and documented**. Recommended rule:

1. **The first `pub gen fn` in source order** is the entry point.
2. If the program has no `pub` generator, the first `gen fn` in source order (so
   today's single-generator scripts, `pub` or not, keep working).
3. Non-`pub` generators are **never** entry-point candidates when a `pub` one
   exists. `pub` should mean "may be entered from the host"; today visibility is
   not recorded in the generator table at all, so this needs a `FLAG_GEN_PUB`
   bit per entry (bytecode version bump; old readers treat every generator as
   `pub`, which is today's effective behaviour).
4. A program with more than one `pub` generator should log a one-time warning
   naming the chosen entry, since the choice is otherwise invisible. (The proper
   fix for that case — letting the host name the entry — is the separate feature
   request, `2026-07-22-feature-named-generator-entry-points.md`.)

The **minimum** fix, needing no format change and no new API, is step 1 alone:
emit the generator table in source order.

Concretely, on the two repros above: the first must print `1` (`deer`) on every
load, forever; the second must print `42` (`brain`) on every load, forever.

## Actual

Both repros select an arbitrary generator, re-rolled on every compile.

**Three brains, three independent batches of 20 loads each** (same binary, same
source file, nothing changed between loads):

```
--- 20 loads, batch 1 ---        --- batch 2 ---              --- batch 3 ---
   4 report_i(1)   deer             8 report_i(1)                6 report_i(1)
   9 report_i(2)   wolf             7 report_i(2)                6 report_i(2)
   7 report_i(3)   boar             5 report_i(3)                8 report_i(3)
```

Roughly uniform over the three generators. Earlier batches from the same session
gave `8/4/8`, `7/7/6` and `7/6/7` — consistently ~1/3 each, never a stable
answer. A single load, verbatim:

```
COMPILE OK
    report_i(2)
  tick 0: yield: i32=0 f32=0 (4 bytes)
    report_i(2)
  tick 1: yield: i32=0 f32=0 (4 bytes)
DONE (2 host calls over 2 ticks)
```

`wolf` ran. Not `deer`. There is no error, no warning, and no host API that
could have expressed a preference — `Prepare` takes only `NoiserInput`.

**A non-`pub` helper hijacks the entry point roughly half the time.** Second
repro, 20 loads:

```
   9 report_i(7)      <- the private helper `other` became the program entry
  11 report(42)       <- the intended `pub gen fn brain`
```

Across three batches the private helper won **9, 12 and 15 of 20**. Visibility is
simply not consulted.

**Independent confirmation** (second investigator, same sha):

- *Test 1* — a file with a non-`pub` `gen fn steps()` above a `pub gen fn
  brain()`, with `steps()` made to report a distinguishing `-999`: **20 loads →
  brain ran 9, steps ran 11, truly silent 0.** Matches the non-`pub`-competes
  finding above; here the private generator won more than half the time.
- *Test 2* — a second file, **25 loads → 15 lost the lottery (the intended entry
  never ran), 10 ran the intended entry.**

**Two compiles of identical source inside one process can disagree.** Compiling
the same two-generator source twice in a single process and running each
(`alpha`=1, `beta`=2), 10 runs:

```
compile#1 -> report_i(1)   compile#2 -> report_i(1)
compile#1 -> report_i(2)   compile#2 -> report_i(1)     <- disagree
compile#1 -> report_i(2)   compile#2 -> report_i(2)
compile#1 -> report_i(2)   compile#2 -> report_i(2)
compile#1 -> report_i(1)   compile#2 -> report_i(1)
compile#1 -> report_i(2)   compile#2 -> report_i(2)
compile#1 -> report_i(2)   compile#2 -> report_i(1)     <- disagree
compile#1 -> report_i(1)   compile#2 -> report_i(2)     <- disagree
compile#1 -> report_i(2)   compile#2 -> report_i(1)     <- disagree
compile#1 -> report_i(2)   compile#2 -> report_i(2)
```

4 of 10. So this is not a per-process seed that a host could pin by setting an
environment variable — it re-rolls per `Compile` call. A hot reload of an
**unchanged** file can silently swap the running generator.

**Selection also silently changes the program's reflected type.** In a file whose
helper is `gen fn steps() -> f32` and whose brain is `pub gen fn brain(e: i32)
-> i32`, loads that lose the lottery yield `f32 1.0, 2.0, 3.0` instead of `i32
0` — and `GetYieldType()` reports the helper's type, which a host will believe.

## Trace

No panic — this is a silent wrong-result. Root cause, by file:line:

- **`noiser-compiler/src/bytecode/mod.rs:2148`** —
  `pub(super) generators: HashMap<String, GenDef>,`
  Generators are stored in a hash map, so insertion (source) order is lost.
- **`noiser-compiler/src/bytecode/header.rs:572-578`** — the generator table is
  emitted by iterating that map:
  ```rust
  output.push(checked_u8(self.generators.len(), "generator count"));
  for (name, gen_def) in &self.generators {
  ```
  `HashMap` iteration order is randomised (`RandomState`, seeded per map
  instance), so the emitted table order is arbitrary and varies per compile.
- **`noiser-compiler/src/bytecode/mod.rs:1367`** —
  `compiler.generators.keys().next().cloned()`
  picks the standalone-generator name the same arbitrary way (Wasm/NoIR path).
- **`noiser-compiler/src/bytecode/serialization.rs:1861`** —
  `parse_first_generator` returns table entry `[0]`, i.e. whichever generator
  hashing happened to place first.
- **`noiser-vm/src/noiser.cpp:830-874`** — `ParseGeneratorMetadata` walks the
  whole table but guards every store with `if (i == 0)`, so the C++
  `SharedState::first_generator_address`, `generator_yield_type`,
  `generator_context_size`, `generator_param_count` and
  `generator_resume_type` all come from that arbitrary entry.

The fix is localised: make the container insertion-ordered (`IndexMap`, or
`Vec<(String, GenDef)>` plus a name index) and `header.rs` emits source order;
`mod.rs:1367` then takes a well-defined first element. Nothing downstream of the
table needs to change for the minimum fix.

## Severity

This is a **silent correctness bug**, not a limitation:

- **A script can pass CI and run a different generator in production.** The test
  run and the shipping run are separate compiles, so they are separate rolls.
  Nothing in the toolchain — no compile error, no warning, no log line, no
  reflection field — reveals which generator was selected. The only way to find
  out is to observe behaviour.
- **It re-rolls on hot reload.** Reloading an unchanged file can swap the
  running generator mid-session.
- **It corrupts reflection**, not just execution: `GetYieldType()` and the
  generator's `param_count` / `context_size` come from the arbitrarily selected
  entry, so a host that reflects the program to size its buffers is reading
  whichever generator won.
- **It makes the current single-brain-per-file workaround unreliable.** The
  accepted convention today is "one `pub gen fn` per file" — that is what
  badlands does, and what the docs imply. That convention does **not** protect
  you: a *private* helper `gen fn` in the same file competes on equal terms and
  won 9–15 of every 20 loads in the measurements above. So the moment anyone
  factors a brain into a sub-behaviour generator — the single most natural thing
  to do with coroutines — their file becomes a coin flip, with no diagnostic.
  There is currently **no way to write a file containing two generators
  safely**, and no way to know you have done it.
- **It is nearly undiagnosable from the outside.** The failure presents as "my
  brain does nothing" or "my brain does something else's job", intermittently.

## This confounds other reproductions — read before diagnosing anything else

**Any repro whose `.noiser` file contains more than one `gen fn` may be measuring
this bug rather than the one it intends to.** A load that loses the lottery
typically presents as *"the entry generator's body never executes: it compiles,
resumes cleanly every tick, and makes zero host calls, with no error"* — because
some other generator, usually a helper that makes no host calls, is running
instead.

Known affected report:

- **`docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`**
  — its repros all place a `gen fn steps()` (or equivalent sub-generator) in the
  same file as the `pub gen fn brain`, so every load was a coin flip. Its
  "outcome (A): entry generator body never runs, zero host calls, no error" is
  this bug, and its claim that the underlying defect is *non-deterministic* is an
  artefact: once entry selection is controlled, that bug is fully deterministic.
  The report's other half — `while let .Just(v) = p.next()` failing to compile
  with `Cannot extract variant payload from non-enum type: Tuple([I32, F32])` —
  is a compile-time result and is unaffected. That report's owner has been
  notified; it is being corrected there, not here.

**Instructions for implementers and for anyone filing a generator bug:**

1. Control for this first. Reduce the repro to **exactly one `gen fn` in the
   file** if the bug survives that reduction. If it cannot be reduced (the bug
   is *about* multiple generators), then:
2. **Verify which generator actually ran** before interpreting the result — give
   every generator in the file a distinct host call or a distinct yield value,
   and check it on every load.
3. **Run each repro at least 20 times** and report the distribution, not one
   observation. A bug reported from a single load of a two-generator file is not
   evidence of anything.
4. Treat "zero host calls, resumes fine, no error" as this bug until proven
   otherwise.

## Example verification programs

### V1 — three `pub` generators: the first in source order wins, always

```noiser
@fn.report_i: fn(x: i32) -> void;

pub gen fn deer(e: i32) -> i32 { loop { @fn.report_i(1); yield 0; } }
pub gen fn wolf(e: i32) -> i32 { loop { @fn.report_i(2); yield 0; } }
pub gen fn boar(e: i32) -> i32 { loop { @fn.report_i(3); yield 0; } }
0.0
```

**Expected after fix**: `report_i(1)` on every tick, on **100 of 100** loads.
Reordering the three declarations in the source changes which one runs, and
nothing else does.
**Today**: ~1/3 each — measured `4/9/7`, `8/7/5`, `6/6/8` over three batches of
20.

### V2 — a private helper never becomes the entry point

```noiser
@fn.report_i: fn(x: i32) -> void;
@fn.report:   fn(x: f32) -> void;

gen fn other(x: i32) -> i32 { loop { @fn.report_i(7); yield 7; } }

pub gen fn brain(e: i32) -> i32 { loop { @fn.report(42.0); yield 0; } }
0.0
```

**Expected after fix**: `report(42)` on every tick, on **100 of 100** loads;
`report_i(7)` never appears; `GetYieldType()` consistently reports `brain`'s
`i32`.
**Today**: the private `other` won **9, 12 and 15 of 20** loads across three
batches; independently, 11 of 20 and 15 of 25 in a second investigator's runs.

### V3 — byte-for-byte reproducibility across compiles

```noiser
@fn.report_i: fn(x: i32) -> void;
pub gen fn alpha(e: i32) -> i32 { loop { @fn.report_i(1); yield 0; } }
pub gen fn beta(e: i32)  -> i32 { loop { @fn.report_i(2); yield 0; } }
0.0
```

```cpp
auto a = *NoiserProgram::Compile(src);
auto b = *NoiserProgram::Compile(src);          // same process, same source
assert(a->GetBytecode() == b->GetBytecode());   // must hold
```

**Expected after fix**: the two bytecode buffers are byte-identical, and both
programs print `1`. Compiling the file in two different processes must also
produce identical bytecode (a useful CI check: compile twice, diff the
bytecode).
**Today**: the two programs disagreed in **4 of 10** runs (`compile#1 -> 2,
compile#2 -> 1`, etc.), so the bytecode differs.

### V4 — single-generator scripts are unaffected (regression guard)

```noiser
@fn.report: fn(x: f32) -> void;
pub gen fn brain(entity: i32) -> i32 { loop { @fn.report(1.0); yield 0; } }
0.0
```

**Expected after fix**: identical to today — `report(1)` per tick, same yield
type, same `param_count`. This is the guard that the ordering change is a no-op
for every existing script, all of which have zero or one generator.
**Today**: works (one candidate, so the lottery is a no-op).

## Related

- `docs/noiser-bugs-upstream/2026-07-22-feature-named-generator-entry-points.md`
  — the feature request this was found inside. **The two are independent**: that
  request asks for *named* selection (`program.PrepareEntry("wolf", input)`) so
  one `critters.noiser` can hold `deer`, `wolf` and `boar`. This bug is that the
  *implicit* selection is non-deterministic, and **must be fixed regardless of
  whether the feature lands** — including for scripts that will only ever have
  one brain plus a helper.
- `docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`
  — confounded by this bug; see the section above.
- `docs/noiser-feedback.md` item 10 — "Named generator entry points —
  `parse_first_generator` limits a file to one brain." The limitation is real,
  but the sharper problem is that the file does not reliably run *any* chosen
  brain.
- Code: `noiser-compiler/src/bytecode/mod.rs:1367`, `mod.rs:2148`,
  `header.rs:572-578`, `serialization.rs:1861-2040`;
  `noiser-vm/src/noiser.cpp:830-874`.
