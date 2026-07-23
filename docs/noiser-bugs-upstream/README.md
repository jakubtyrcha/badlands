# noiser bugs — upstream-submittable repros

Minimal, self-contained reproductions of **noiser language/VM bugs** hit while
using noiser as the badlands game-AI scripting layer. Each file here is written
to drop into the upstream tracker (`third_party/noiser/noiser-bugs/`) unchanged.

This folder is the **actionable, one-file-per-bug** companion to
`docs/noiser-feedback.md`. That file is the running *integration log* — proposals,
positive findings, and already-worked-around gotchas. When a gotcha is a genuine
upstream bug (a compiler crash, wrong VM result, or a valid program that fails),
it gets a companion minimal repro **here**.

## Workflow — report BEFORE the workaround

This is mandatory (mirrors the upstream `noiser-bugs` skill,
`third_party/noiser/.claude/skills/noiser-bugs`):

1. **Shrink** the failure to a minimal standalone `.noiser` snippet that still
   reproduces it (delete everything not needed to trigger the bug).
2. **File it here** as `YYYY-MM-DD-brief-description.md` using the template
   below, with the right `Label`.
3. **Only then** write the script-side workaround, cross-referencing the repro
   file (and note the gotcha in `docs/noiser-feedback.md`).

Workarounds hide bugs; reports track them.

## Labels (exactly one per bug)

| Label | Scope |
|-------|-------|
| `parser` | Lexer, tokenizer, syntax parsing |
| `compiler` | AST transforms, type inference, symbol resolution, bytecode emission |
| `vm-bytecode` | Invalid bytecode emitted (wrong opcodes, stack misalignment) |
| `vm` | C++ runtime execution bugs (wrong results / crashes on valid bytecode) |
| `wasm-backend` | WASM (Binaryen/Wasmtime) compilation issues |

## Template

Filename: `YYYY-MM-DD-brief-description.md`

```markdown
# [Brief Title]

**Date**: YYYY-MM-DD
**Version (noiser sha)**: <submodule sha>
**Label**: [parser|compiler|vm-bytecode|vm|wasm-backend]
**Status**: [new|confirmed|fixed|wontfix]

## Problem
One or two sentences: what breaks.

## Minimum Reproduction
```noiser
// smallest snippet that still fails
```

## Expected
What a correct implementation should do.

## Actual
The compile/VM error or wrong result (verbatim).

## Trace
Panic/backtrace or the failing file:line, if any.

## Related
Cross-links: docs/noiser-feedback.md entry, the script-side workaround,
upstream issues.
```

## Index (2026-07-22 sweep)

Filed against noiser sha `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`. Every claim
in these reports is backed by an executed run, not by reading the compiler.

### Read this one first

| File | Why it comes first |
|---|---|
| `2026-07-22-entry-point-selection-is-nondeterministic.md` | A file with 2+ `gen fn` runs an **arbitrary** one — `generators: HashMap<String, GenDef>` serialized in iteration order, read back as entry `[0]`. Silent, no diagnostic, varies per load and even between two compiles in one process. **It confounds any reproduction involving more than one generator**, and it did confound ours: two revisions of the nested-generator report were wrong because of it. Control for it before reproducing anything else here. |

### Bugs

| File | Label | Summary |
|---|---|---|
| `2026-07-22-entry-point-selection-is-nondeterministic.md` | compiler | Entry generator chosen by HashMap order; non-`pub` helpers compete. |
| `2026-07-22-nested-generator-next-returns-tuple-not-maybe.md` | vm-bytecode | Resuming a sub-generator owned by the entry generator fails deterministically on the **second** resume (`RestoreState: state buffer index 0 >= buffer len 0`). Creating one is harmless. |
| `2026-07-22-generic-maybe-instantiation-ices-inside-generator-body.md` | compiler | `let`-binding a builtin generic returning `?T` (e.g. `Vec::pop`) inside the entry generator ICEs on `Maybe$f32`. Workaround: hoist into a plain `fn`. |
| `2026-07-22-generator-passed-to-function-loses-state.md` | vm-bytecode | Generator handles **do** survive `fn` boundaries; the failure returns once an entry generator owns the instance. Refutes its own original premise. |
| `2026-07-22-host-fn-vecn-return-ices-compiler.md` | compiler | `@fn … -> vec4` ICEs at the call site. Root cause: stdlib registers `Vec4` capitalised but `ivec4` lowercase, so the **documented** lowercase spelling is unregistered and degrades to `Unknown`. Not vector-specific — any unknown type name panics instead of diagnosing. |
| `2026-07-22-host-fn-vecn-argument-rejected-at-callsite.md` | compiler | Same root cause on the argument side: declaring compiles, every call fails `expected Unknown, got HomogTuple`. `ivecN` args work — the exact mirror image. |
| `2026-07-22-generic-protocol-bounds-fail-to-parse.md` | compiler | Inline bound on a **user-declared** protocol ignores the user's `impl`; `where` works. Parameterized inline bounds are documented-as-unsupported but diagnosed poorly. |

Both `vecN` reports carry a **"read before closing as duplicate"** block: this
family was previously falsified upstream by a matrix that only exercised the
*capitalised* spelling at *top level*. Both axes matter.

### Feature requests

| File | Summary |
|---|---|
| `2026-07-22-feature-named-generator-entry-points.md` | Select a `gen fn` by name at `Prepare` time, so one file can hold `deer`/`wolf`/`boar`. Distinct from the bug above: selection must first become *deterministic*, then *selectable*. |
| `2026-07-22-feature-structured-generator-parameters.md` | Hand a typed data block to a generator at `Prepare` time. Today the only channel is `warp_id`/`warp_size` (two ivec3s), so per-type tuning data costs one host call per field per entity per tick. |
| `2026-07-22-feature-cpp-embedding-gaps.md` | Six C++ host/build gaps, each tied to a workaround badlands ships today: 24 stubbed JIT symbols, a 64 MiB compile thread, the `detail::SetHostCallProfiling` opt-out, generator snapshot for hot reload, a hand-maintained 54-function FFI header with cbindgen declared but unused, and glm/spdlog in the public header. |

### Fixed / retracted

| File | Status |
|---|---|
| `2026-07-13-struct-of-enums-return-from-perception-corrupts.md` | **fixed** — no longer reproduces on `52174b2`; 25/25 identical correct runs, verified against a faithful re-creation of the original repro. badlands' tuple workaround is no longer required. |

### What this sweep corrected in our own records

- struct-of-enums: **fixed**, not a live bug.
- `vecN` argument: fails at the **call site**, not on declaration.
- Local multi-file imports: **work** (`FileModuleResolver`), contradicting an
  earlier note.
- The nested-generator "silent no-op" and its apparent non-determinism: **the
  entry lottery**, not sub-generator corruption.
- "Creating a sub-generator corrupts the parent": **withdrawn** — 8/8 correct
  once controlled.

### Known hazards when writing repros

- **Dead-code elimination silently drops host calls.** `fn side(k: f32) -> f32
  { @fn.report(k); k + 1.0 }` never fires `@fn.report` when the argument derives
  only from params/constants, and a call whose result is unused is removed
  entirely. This matters because "hoist into a plain `fn`" is the recommended
  workaround elsewhere in this folder.
- **`available:` symbol lists in import errors are emitted in HashMap order**
  and differ between runs — do not diff them.
