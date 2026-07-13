# Struct-of-enums return, built from perception data, is silently corrupted

- **Label:** `compiler` (likely struct-return / RVO layout or codegen; produces wrong
  runtime values, no compile error and no runtime trap)
- **Date:** 2026-07-13
- **Discovered in:** badlands hero-brain framework (`scripts/brains/warrior.noiser`)
- **Backends observed:** VM (badlands runs `NoiserBackend::kVM`); WASM not tested

## Summary

A `struct` whose fields are `enum`s (`Decision { goal: Goal, command: Command }`), when
**produced by a function chain that is fed a struct assembled from `@fn` host-call results**
(`observe()` builds a `View` from perception tuples → `decide()`/`combat_act()` returns a
`Decision` built from that `View`), is **silently corrupted**: a later `match` on the produced
struct's enum field takes the wrong arm. The program **compiles cleanly, runs without any error
or trap**, but the enum payload/tag read back is wrong — every `match` falls through to the
"empty" arm, so no host calls fire.

Returning the same two enums as a **tuple `(Goal, Command)`** instead of wrapping them in a
struct **works correctly**. So does returning a bare enum, or building the `Decision` from a
**literal** `View` (not one sourced from host calls). The corruption needs the combination:
*host-call-sourced source struct* **+** *a produced struct-of-enums return*.

## Reproduction (fails)

Observed: both units sit still forever; `script_intents == 0`; `noiser_bugs == 0` (no error).

```noiser
@fn.perceive_self: fn(e: i32) -> (f32, f32, f32, f32);
@fn.perceive_target: fn(e: i32) -> (f32, f32, f32, f32);
@fn.attack_range: fn(e: i32) -> f32;
@fn.intent_move_to: fn(e: i32, x: f32, z: f32) -> void;
@fn.intent_attack: fn(e: i32) -> void;

struct View { px: f32, pz: f32, range: f32, tx: f32, tz: f32, exists: f32 }
enum Goal { Hold, GoTo(f32, f32) }
enum Command { NoOp, Attack }
struct Decision { goal: Goal, command: Command }   // <-- struct of enums

fn observe(e: i32) -> View {
    let (sx, sz, hp, cd) = @fn.perceive_self(e);
    let (tx, tz, thp, has) = @fn.perceive_target(e);
    View { px: sx, pz: sz, range: @fn.attack_range(e), tx: tx, tz: tz, exists: has }
}
fn combat_act(v: View) -> Decision {
    Decision { goal: Goal::GoTo(v.tx, v.tz), command: Command::NoOp }
}
fn decide(v: View) -> Decision {
    if v.exists > 0.5 { combat_act(v) } else { Decision { goal: Goal::Hold, command: Command::NoOp } }
}
pub gen fn brain(entity: i32) -> i32 {
    loop {
        let d = decide(observe(entity));
        match d.goal {                                 // <-- always falls to Hold
            Goal::GoTo(x, z) => @fn.intent_move_to(entity, x, z),
            Goal::Hold => {},
        }
        match d.command { Command::Attack => @fn.intent_attack(entity), Command::NoOp => {} }
        yield 0;
    }
}
0.0
```

## Workaround (works) — return a tuple of the enums, not a struct

Identical logic, but `decide` returns `(Goal, Command)`; units approach and fight correctly
(`script_intents > 0`, HP drops).

```noiser
fn decide(v: View) -> (Goal, Command) {
    if v.exists > 0.5 {
        (Goal::GoTo(v.tx, v.tz), Command::NoOp)
    } else {
        (Goal::Hold, Command::NoOp)
    }
}
// brain: let (g, c) = decide(observe(entity)); match g { ... } match c { ... }
```

## Controls that isolate the trigger

| Variant | Result |
|---|---|
| `observe()` → return bare `Goal` enum, `match` it | ✅ correct |
| `Decision` struct built from a **literal** `View` (no host calls) → `match` | ✅ correct |
| `Decision` struct returned through nested fns, **literal** `View` | ✅ correct |
| `observe()` (host-call-sourced `View`) → `Decision` struct → `match` | ❌ **corrupted** (this bug) |
| `observe()` → `(Goal, Command)` tuple → `match` | ✅ correct (workaround) |

So neither "struct of enums", nor "host-call perception", nor "nested struct returns" is
sufficient alone — the fault needs the host-call-sourced source struct feeding a produced
struct-of-enums return.

## Execution context

- noiser submodule `@ 7fd974a`; badlands compiles the brain via `NoiserProgram::Compile(source)`
  (VM backend) and runs it once per entity per tick (`game/src/brain.cpp`).
- Reproduced against the shipping duel test (`game/tests/duel_test.cpp`) which spawns two
  entities running the same brain; with the failing form the duel never resolves (units never
  move), with the tuple form it resolves normally.
- No `--check`/compile error; full bytecode emits; no runtime trap; `noiser_bugs` stays 0. The
  only symptom is wrong runtime values (every enum `match` on the produced struct mis-dispatches).
