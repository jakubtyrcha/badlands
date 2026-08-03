# game/ — the world simulation

`badlands_game_lib`: the EnTT world sim (placement, movement, brains, combat, needs,
skills, progression), called by the apps through the C++ `badlands::Sim` facade
(`game/include/badlands_sim.hpp` — pimpl, POD snapshot vectors; the old C ABI is
gone, and the surviving C ABIs are the Rust crates and the wasm brain contract).
This is the only place gameplay state lives; `src/game/` is the render/scene layer,
not this.

Paths below are repo-relative. Sim sources are `game/src/…`; note that `game/tests/`
holds render-layer tests too (see Tests).

## Time convention — FOUR clocks, and they never mix
Every duration, rate and timestamp belongs to exactly one base, and the name says which.

| Base | What it is | Name / unit | Who may read it |
|---|---|---|---|
| **Real** | wall clock; ignores pause and speed | `real_dt`, `real_*`, float seconds | the app shell only (SDL loop, SimClock input) |
| **Presentation** | real × playback speed; stops when paused | `anim_*`, float seconds | views only — animation, VFX, tracers, camera easing |
| **Sim** | the deterministic tick clock | `*_ticks` (int64), **1 tick = 1/120 s** | everything under `game/`. THE gameplay clock |
| **Narrative** | day/night, derived from sim time | `*_hours`, `time_of_day`, `*_days` | the day cycle, and needs |

- **Authored data is in SIM SECONDS.** A `3` in `skills.json` is 3 sim seconds; at 1× playback that happens to be 3 real seconds, and that equality is a property of the speed setting, not the definition.
- **A TICK is the atom of sim time, and a tick is 1/120 s** (`kTicksPerSecond = 120`). 120 was chosen because it divides every usual step rate — 30, 60, 120 — so a sim step is always a whole number of ticks and no clock can drift against another.
- **Sim time is NOT milliseconds.** 1000 does not divide by 30, so a millisecond clock at 30 Hz truncates (33 ms/step → 30 steps = 990 ms, a 1% drift against the clock that decides how many steps to run). Ticks remove the rounding rather than documenting it.
- **The sim STEP is separate from the tick.** The step rate is how often the world advances (30 Hz today = `kTicksPerStep` 4); the tick is what time is *measured* in. Changing the step rate must never change what a duration means.
- Authored seconds convert once, at the boundary: `ticks = round(seconds × 120)`. Milliseconds survive only where a wire or a display asks for them.
- **Nothing under `game/` may see real or presentation time.** If a sim system takes a `dt`, that is the bug: it makes the result depend on a number that is not in the command log, which the determinism contract forbids.
- **Narrative time is DERIVED and is never a gameplay duration.** A cooldown is never "half an in-game hour". One deliberate exception, and it says so in its signature: `reserve_rate_per_step(float hours, int64_t ticks_per_day)` (`game/src/components.h`) — a need is *about* the day, so it is authored against it. Note it returns a per-STEP delta, and its day length is a runtime `ticks_per_day` off `WorldConfig`, never a constant.
- **Speed changes how fast ticks are issued, never what a tick means.** `SimClock` decides how many ticks to run; the length of one is a compile-time constant.
- When talking to a human, real-world units are fine ("a 3-second stun"). In code and data they mean sim time, always.

## Event sourcing is the mutation contract
- **Every mutation is a `Command`** (`game/src/command.h`) — player action and AI decision alike — applied at one point and appended to `command_log`.
- **`state = f(initial config, command log, N ticks)`**, enforced by `game/tests/determinism_tests.cpp` (run-twice + replay-the-log).
- **A new mechanic is a new `CommandKind` + handler.** A brain never writes the registry directly.

## The registry is SHARED with the renderer, and the traffic is one-way
- **`Sim::registry()` hands out the world, and the render layer puts its OWN components on sim entities** (per-character animation blend state). That is only safe because of the next rule.
- **No sim system may ever read a render component.** It is not compiler-enforced — the accessor is a mutable reference — so it is pinned by a test in `game/tests/determinism_tests.cpp` that runs the same world with and without render components attached and requires identical state AND command log.
- **`CharacterAnim` (`badlands_sim.hpp`) is the one thing the sim writes FOR the renderer**, produced by `project_anim_state` (`game/src/anim_projection.*`) as the last act of every tick. Nothing under `game/` reads it back.
- **Animation observes state; it never consumes events.** An event can be retracted by a later mechanic, and a consumer that latched onto one must then cancel what it started; a state is what stuck, so an observer self-corrects. `GameEvent` stays the HUD's channel.
- **The sim owns the TIMING, the view owns the CLIP.** A swing's window is authored data in ticks; the view stretches its clip to fit, so a blow and its animation cannot drift. No clip ever gets named under `game/`.
- **The projector runs LAST in `step_world` and that placement is load-bearing.** It reads `moved_by_path_scratch`, whose contract is "never read outside the tick that wrote it"; earlier placement reads a half-filled buffer and reports actions later systems superseded.

## Facing has TWO writers, and both are deliberate
- **Movement writes the direction of travel** (`movement.cpp`), and **`declare_strike` turns the attacker toward its target** (`strike.cpp`). Nothing else writes `Facing`.
- **The strike turn exists because a fighter that stopped moving kept whatever way it last walked** and swung at empty air. Invisible on a symmetric capsule; unmissable the moment a skeleton was drawn on one.
- **It aims the VISION CONE too, and that is the point rather than a side effect** — a unit looks at what it is fighting. Deterministic, since both positions are sim state.
- **Coincident positions keep the last facing.** `Facing.dir` is documented unit-length and the vision cone divides by it, so a zero vector is never stored.

## Brains and combat
- **The hero brain is Nim→WASM and is the only hero brain** — run in a wasmtime host (`src/crates/brainhost`) behind the wire contract `game/src/brain_abi.h`. A world with no wasm bytes loaded simply idles its heroes; no mock/C++ hero decision layer exists.
- **Monster brains (rats, goblins) are engine code** (`game/src/monster_brain.*`, `game/src/critter_brain.*`), but they go through the same seams as wasm heroes.
- **Combat is brain-driven end-to-end through the intention/action gateway.** Both kinds of brain fight by adopting an `Attack` intention (engagement) and enqueueing `BL_ACT_ATTACK` actions (swings) through `apply_intention`/`resolve_action` (`game/src/intention.h`); there is no host-level combat path.
- **Defense is passive-only** (`resolve_attack`'s defender gates). A brainless entity issues no intentions and no actions.
- **The brain intends, the game validates and executes.** A refused intention produces an event the brain can reconsider from — refusal is data, not an exception.

## Tests
- **`game/tests/` feeds ~15 different executables, not one.** The sim suite is `badlands_game_tests`; the render-layer tests that also live here go to `badlands_geometry_tests`, `badlands_tree_tests`, `badlands_water_tests`, `badlands_terrain_mesh_tests` and others.
- **Test sources are listed explicitly in the root `CMakeLists.txt` — there is no globbing.** A new `game/tests/foo_tests.cpp` that is not added to a target compiles into nothing and silently never runs.
- Run through ctest (`scripts/test.sh badlands_game_tests "[combat]"` for a filtered run); a bare test binary can fail on env ctest supplies.
- **Never assert on shipped data files.** Test loaders and manifests against test-local fixtures, so tuning a JSON never breaks a test.
