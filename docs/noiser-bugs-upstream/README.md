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

## Status

No upstream noiser bugs were hit implementing the v0.3 town host-call surface —
the perception tuples (including a host fn taking an `i32` kind arg alongside a
`(f32,f32,f32,f32)` tuple return), the durable `intent_move_to`, and the
`intent_enter`/`intent_buy` calls all bound and fired cleanly
(`game/tests/noiser_smoke_tests.cpp`). This folder therefore ships with just
this README. The known `@fn -> vecN` compiler ICE remains tracked in
`docs/noiser-feedback.md` and pinned by `noiser_smoke_tests.cpp`; the brain
works around it with flat `(f32, …)` tuple returns.
