# The AI Sandbox as a Puppet Master, and Duel Mode

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.
>
> Plan file lives here only because plan mode restricts writes; copy it to `docs/superpowers/plans/2026-07-31-sandbox-modes-duel.md` as the first commit of implementation.

## Context

We want to watch classes fight: sample two creatures, drop them in a randomly-chosen arena, give them a minute, log who won, repeat. `badlands_ai_sandbox` becomes the general app for driving the AI, with **duel as its first mode**.

Exploration turned up three things that must be fixed rather than built around:

- **The arena is a special case, not a place.** `WorldConfig::arena_half_x/z` is an analytic rectangle enforced by a step refusal in `follow_paths` (`movement.cpp:305`) and a clamp in `separate_units` (`movement.cpp:484`). The pathfinder knows nothing about it, so nothing inside an arena can ever obstruct a route.
- **Every world is coupled to one hand-authored map.** `make_world` copies `SymbolicMapGenerator`'s output unconditionally (`sim.cpp:102`), which stages every arena on that map's central **Lake**.
- **The sandbox is a pile of one-offs**, not a harness: `SeedTown()` hardcodes a town seed and a JSON scenario branch, and the arena's walls are view-only decoration from `src/game/arena.cpp` that no game system can see.

### The layering principle this plan enforces

**No game mechanic may be aware that it is being driven by a sandbox.** The sandbox is a puppet master *above* the game: it configures a world, populates it, and lets the game run. Every system in play — map, buildings, navmesh, brains, combat, skills, the tick — behaves exactly as it does in the real game, because it cannot tell the difference.

| Layer | What it gains | What it must never contain |
|---|---|---|
| **Game** — `game/`, `badlands_game_lib` | The plop placement primitive; a generic `Wall` building kind; a map choice per world; structures declared in the initial config | The words arena, duel, mode, sandbox |
| **Puppet master** — `src/executables/ai_sandbox/` | The mode interface, duel mode, the arena shape builder, the debug UI | Anything the game needs to know about |

A wall block is a building the game already understands. An **arena is a pattern of walls that only the sandbox knows how to lay out** — the navmesh sees buildings, exactly as it does in town.

**Goal:** the sandbox stages a duel in a plopped arena on a flat map, logs the result, and restages — with one less special case in the sim than we started with, and no game system able to tell it is being watched.

**Tech Stack:** C++20/23 + EnTT + Catch2 (`game/`), CMake/Ninja.

## Global Constraints

- **No game mechanic knows about the sandbox.** If a change requires the game to branch on "am I in a mode", it is the wrong change.
- **Append-only id spaces.** `BuildingKind`, `CreatureId`, `CommandKind`, `StatusKind`, `GameEventKind` are never renumbered; new entries go before `Count`.
- **Initial config is not a command.** World construction plops the arena directly, exactly as `prebuild_colony` places the Castle (`sim.cpp:130`). `state = f(initial config, command log, N ticks)` holds because the arena *is* config.
- **Sim timers are int64 milliseconds** off `game.world_millis`; never a float accumulator.
- **Tests assert invariants and mechanism, never authored numbers.** An arena test checks "sealed and mutually reachable", not "127 wall blocks".
- **Results are logged, not stored.** One `spdlog::info` line per duel. No files, no charts, no matrix, no accumulator.
- **No debug controls that weren't asked for** — no ImGui panels, sliders, or env hooks beyond the mode selector.
- **`ctest` is canonical.** `scripts/build.sh`, `scripts/test.sh`.
- Work on a normal branch; no git worktrees in this repo.

## Key facts established during exploration

- The grid is **256 × 256 tiles**, 1 tile = 1 world unit = 1 m, spanning `[-128, 128)` (`kGridHalfExtentTiles = 128`; the `// 48` comment at `placement.h:20` is stale). The map is 256 m square, so arenas of 30–50 m fit easily.
- A **block is 4 m** (`SymbolicMapGenerator::kSpacingM`) — the unit "block sized" refers to.
- Buildings already block pathfinding: `SimNavSource::blocked` reads `placement.footprint` (`nav_world.cpp:54`). **This is the whole mechanism** — obstruction needs no new concept.
- The placement grid supports **diagonal footprints** natively (`rot` 1/3, `diagonal_spans`), which is what makes clean 45° walls possible.
- Footprint membership uses **open intervals** against triangle centroids that never lie on lattice lines (`in_footprint`, `triangle_centroid`), so abutting footprints tile with **no gap and no overlap**.
- The navmesh **dilates obstacles by `clearance_cells = 1`** (`nav_world.cpp:32`), so a 4 m block occupies 6 m for routing; corridors must keep ≥ 3 m of free space.
- `reproject_out_of_footprints` (`movement.cpp:100`) already pushes a body out of a building to the nearest footprint edge, gated on navmesh presence (`movement.cpp:464`).
- The margin (`kMarginRadius = 1.0`, `placement.cpp:187`) is baked into both `placement_valid` and `commit` — it is exactly the player constraint that forbids gapless walls.
- `SdlViewerApp::Run` parses its own argv and ignores unknown flags; `main_ai_sandbox.cpp` can capture argv into its factory lambda, so **a `--mode` flag needs no engine interface change**.

---

## File Structure

**Deleted**
- `src/executables/duelsim/` — the whole tool (main, `duel_matrix`, `svg_chart`, tests).
- `src/game/scenario.{h,cpp}`, `src/game/tests/scenario_tests.cpp`, `assets/scenarios/`.
- `src/game/arena.{h,cpp}` — a view-only tile decoration in the game lib; reborn app-side as a plop builder.
- The town seed inside `ai_sandbox_view.cpp` (buildings, recruiting, the deer herd).
- `WorldConfig::arena_half_x/z` and both branches that enforce them.
- CMake targets `badlands_duelsim`, `badlands_duelsim_tests`, `badlands_scenario_tests`; the `duelsim_out/` line in `.gitignore`.

**New — game layer** (mode-blind, all of it true of the real game)
- `src/game/map/flat_map_generator.{hpp,cpp}` — an all-Plains, height-0 `MapData`.

**New — puppet master** (`src/executables/ai_sandbox/`)
- `sandbox_mode.hpp` — the mode contract.
- `arena.{hpp,cpp}` — arena shapes as lists of buildings to plop.
- `duel_mode.{hpp,cpp}` — the pool, the sampler, the round lifecycle, the log line.
- `tests/ai_sandbox_tests.cpp` — arena invariants + duel lifecycle.

**Modified (principal)**
- `game/src/placement.{h,cpp}` — `plop_building` extracted as the primitive.
- `game/include/badlands_sim.hpp` — `BuildingKind::Wall`; `WorldConfig::{map, plops}`; `arena_half_*` deleted.
- `game/src/sim.cpp`, `game/src/game_state.h`, `game/src/movement.cpp` — map selection, config plops, deletions.
- `src/game/building_catalog.cpp` — the `Wall` visual.
- `src/executables/ai_sandbox/{main_ai_sandbox.cpp, ai_sandbox_view.{hpp,cpp}}` — mode host.
- Tests: `game/tests/{placement,movement,combat,intention,symbolic_map}_tests.cpp`.
- `CMakeLists.txt`.

---

## Task 0: Remove duelsim and the scenario loader

Pure deletion, first, so nothing later is written against code that is going away. The sandbox falls back to its town seed for one commit (it already does when no scenario loads), which keeps the build and the app green.

**Files:** delete `src/executables/duelsim/`, `src/game/scenario.{h,cpp}`, `src/game/tests/scenario_tests.cpp`, `assets/scenarios/`; modify `CMakeLists.txt`, `.gitignore`, `src/executables/ai_sandbox/ai_sandbox_view.{hpp,cpp}`

- [ ] **Step 1: Delete the tool and the loader**

```bash
git rm -r src/executables/duelsim src/game/scenario.h src/game/scenario.cpp \
          src/game/tests/scenario_tests.cpp assets/scenarios
```

- [ ] **Step 2: Unwire them** — drop the `badlands_duelsim`, `badlands_duelsim_tests` and `badlands_scenario_tests` targets from `CMakeLists.txt`, drop `src/game/scenario.cpp` from `badlands_app_sources_ai_sandbox`, and drop `duelsim_out/` from `.gitignore`.

- [ ] **Step 3: Strip the scenario branch from the view** — in `ai_sandbox_view.cpp`, remove the `BADLANDS_SCENARIO` load, the `scenario_is_arena_` branch of `SeedTown` (`:210-250`), the arena tally block in `DrawInspector` (`:636-647`), and the `scenario_`/`scenario_is_arena_`/`scenario_load_error_` members. `SeedTown` unconditionally seeds the town.

- [ ] **Step 4: Run** — `scripts/build.sh` → `BUILD OK`; `scripts/test.sh` → green (three targets fewer).

- [ ] **Step 5: Commit**

```bash
git commit -m "chore: remove duelsim and the sandbox scenario loader"
```

---

## Task 1: Plopping is the placement primitive

The layering the rest rests on, and a game mechanic in its own right: **plopping is the primitive, and the player's building mechanic is plopping plus the player's constraints.** Nothing here knows what the plops will be used for.

**Files:** `game/src/placement.{h,cpp}`, `game/include/badlands_sim.hpp`, `src/game/building_catalog.cpp`, `game/tests/placement_tests.cpp`

**Interfaces produced:**

```cpp
// placement.h

// Whether a placement enforces the player's spacing rule -- the ONE axis on
// which plopping and player placement differ at the occupancy layer.
enum class PlacementMargin { None, Player };

// The PRIMITIVE: stamp a footprint into the grid and record the building.
// Validates bounds and footprint-vs-footprint overlap ONLY -- no margin, no
// urban sprawl, no poppables, no cost. Two plops may TOUCH, which is what
// makes a solid wall expressible.
//
// This is the reusable layer. place_building (below) is this plus the player's
// constraints; anything else that authors structures directly -- a prefab, a
// scripted layout, a generated map feature -- calls this and gets the same
// buildings the player's own placements produce.
uint32_t plop_building(BadlandsGame& game, const PlacementDesc& desc);

// The PLAYER path, unchanged in behaviour: refuses a placement that lands on an
// existing footprint OR margin (either direction), then plops it, then accrues
// urban sprawl and processes poppables.
uint32_t place_building(BadlandsGame& game, const PlacementDesc& desc, bool player);

// Both rules, explicit. `Player` is the existing two-way margin test; `None`
// tests footprint-vs-footprint only.
bool placement_valid(const PlacementState& st, const Footprint& fp, PlacementMargin margin);
```

`PlacedBuilding` gains `PlacementMargin margin = PlacementMargin::Player;` — **load-bearing**, because `rebuild_occupancy` restamps every alive building after a destruction and would otherwise re-inflate a plopped wall with a margin it never had.

**The new building kind is `Wall`, not `ArenaBlock`.** A 4 × 4 solid structure is a thing the game has; an arena is not. `kDefs` row: `{4, 4, false, false, false, 0.0f, 0, {}}` — not poppable, not player-destructible, **not enemy-targettable** (nothing gnaws a wall), grants no vision, recruits nobody.

- [ ] **Step 1: Write the failing tests** — append to `game/tests/placement_tests.cpp`

```cpp
TEST_CASE("plopped footprints may touch", "[placement][plop]") {
    // Two Walls plopped edge to edge both succeed, and every tile between them
    // is footprint-blocked -- no free lane, which is what makes a wall solid.
}
TEST_CASE("player placement still refuses to touch a plopped block", "[placement][plop]") {
    // place_building adjacent to a plopped Wall fails: the player's margin rule
    // is unchanged by the primitive existing underneath it.
}
TEST_CASE("a plop may land on an existing building's margin", "[placement][plop]") {
    // Place a Watchtower, then plop a Wall one tile away -> succeeds. The
    // reverse still fails, per the test above.
}
TEST_CASE("plopping accrues no urban sprawl and no poppables", "[placement][plop]") {
    // urban_quarters and buildings.size() move only by the plop itself.
}
TEST_CASE("plops survive a rebuild_occupancy round trip", "[placement][plop]") {
    // Plop two touching Walls + place one player building; destroy the player
    // building; the plopped pair is still touching and still un-margined.
}
TEST_CASE("a plop bumps nav_epoch", "[placement][plop]") {
    // The navmesh must rebuild for a plopped obstacle exactly as for a placed one.
}
```

- [ ] **Step 2: Run, confirm failure** — `scripts/build.sh badlands_game_tests` → FAIL.

- [ ] **Step 3: Implement**
  - Append `Wall` to `BuildingKind` before `Count`, its `kDefs` row, and its `kBuildingLabels` entry (`"Wall"`) — that table's `static_assert` will otherwise fail the build, which is it doing its job.
  - Thread `PlacementMargin` through `placement_valid` and `commit`; store it on `PlacedBuilding`; honour it in `rebuild_occupancy`.
  - `plop_building` = snap → `placement_valid(st, fp, None)` → `commit(..., None)`.
  - `place_building` = `placement_valid(st, fp, Player)` → `commit(..., Player)` → sprawl/poppables when `player`.
  - `building_visual(BuildingKind::Wall)`: height 2.4 m, `RoofShape::None`, `MaterialId::RockWall` — the `Sewer` row is the precedent for a roofless block.

- [ ] **Step 4: Run** — `scripts/test.sh badlands_game_tests "[placement]"`, then `scripts/test.sh`.

- [ ] **Step 5: Commit**

```bash
git add game/src/placement.h game/src/placement.cpp game/include/badlands_sim.hpp \
        src/game/building_catalog.cpp game/tests/placement_tests.cpp
git commit -m "feat(game): plopping is the placement primitive; player rules layer on top"
```

---

## Task 2: A world picks its own map, and declares its structures

Three game-level changes, all mode-blind: a world can choose a featureless map, a world can declare structures that exist at construction, and the arena rectangle dies.

**Files:** `src/game/map/flat_map_generator.{hpp,cpp}`, `game/include/badlands_sim.hpp`, `game/src/{sim.cpp,game_state.h,movement.cpp}`; tests `game/tests/{symbolic_map,movement,combat,intention}_tests.cpp`; `CMakeLists.txt`

**Interfaces produced:**

```cpp
// flat_map_generator.hpp -- a featureless flat plain, the whole map. No seed,
// no lake, no biome variety. Exists so a world that wants NO terrain can say
// so, instead of inheriting the town's hand-authored map and quietly staging
// itself on the Lake at the origin.
class FlatMapGenerator : public MapGenerator {
 public:
  MapData Generate() const override;
  // Deliberately the symbolic map's lattice: the placement grid must span the
  // map (make_world's static_assert), so both generators are 256 m square.
  static constexpr int kNodesPerSide = 65;
  static constexpr float kSpacingM = 4.0f;
  static constexpr float kMapSizeM = (kNodesPerSide - 1) * kSpacingM;  // 256 m
  static constexpr float kHeightM = 0.0f;
};

// badlands_sim.hpp
enum class MapKind : int32_t { Symbolic = 0, FlatPlains };

struct WorldConfig {
    bool prebuild_colony = true;
    bool terrain_blocking = true;
    MapKind map = MapKind::Symbolic;
    // Structures that exist when the world is built, plopped in order through
    // plop_building -- no player constraints, so they may touch. Initial
    // config, exactly like prebuild_colony. The sim does not know or care what
    // shape they form.
    std::vector<PlacementDesc> plops;
    int64_t millis_per_day = kDefaultMillisPerDay;
    // arena_half_x / arena_half_z: DELETED. A wall is a building now.
};
```

**Deletions, in full:**

| Where | What |
|---|---|
| `badlands_sim.hpp:680-681` | `WorldConfig::arena_half_x/z` |
| `game_state.h:70-71` | `BadlandsGame::arena_half_x/z` |
| `sim.cpp:105-106` | the two assignments |
| `movement.cpp:305-312` | the `past_arena_edge` branch in `follow_paths` |
| `movement.cpp:481-489` | the arena clamp in `separate_units` |

**What replaces the deleted behaviour, and what does not.** A plopped wall blocks the *navmesh*, so `plan_paths` routes around it and reports a goal behind it as unreachable; `reproject_out_of_footprints` keeps a shoved body out of it. A wall does **not** refuse a step in `follow_paths` — after this the only step refusal left is impassable terrain, which is correct: confinement is the pathfinder's job now, not a clamp's.

- [ ] **Step 1: Write the failing tests**

```cpp
// game/tests/symbolic_map_tests.cpp -- add to the existing target
TEST_CASE("the flat map is plains everywhere, at one height", "[map][flat]") {
    // A spread of positions incl. the ORIGIN: DominantBiomeAt == Plains and
    // HeightAt == 0 at every one. The origin especially -- the symbolic map's
    // Lake there is the coupling this generator exists to break.
}
// game/tests/movement_tests.cpp -- the arena-edge tests, rehomed onto buildings
TEST_CASE("a goal behind a plopped wall is unreachable", "[movement][plop]") {
    // Flat map, terrain_blocking on, a wall of touching Walls across the path.
    // plan_paths reports unreachable -> MoveBlocked, and the unit never ends up
    // on the far side.
}
TEST_CASE("separation cannot shove a unit inside a wall", "[movement][plop]") {
    // Two units piled on one spot hard against a plopped wall: after
    // separate_units neither is inside a footprint.
}
TEST_CASE("a world plops its configured structures", "[sim][plop]") {
    // WorldConfig::plops of three touching Walls -> three alive buildings, and
    // the navmesh treats the span as blocked.
}
// game/tests/intention_tests.cpp -- rewrite of the arena-wall MoveBlocked test
TEST_CASE("a refused step mirrors MoveBlocked into the hero's inbox", "[intention]") {
    // Same assertion, now driven by an unreachable goal behind a plopped wall.
}
```

Delete `combat_tests.cpp`'s two arena-edge tests (`:506-554`) outright — their subject no longer exists, and the movement tests above cover the replacement seam in its proper home.

- [ ] **Step 2: Run, confirm failure.**

- [ ] **Step 3: Implement**
  - `FlatMapGenerator`: `MapData(65, 65, 4.0f)`, every `Plains` slice 255 and every other slice 0, heights 0.
  - `make_world`: select the generator on `config.map`, each cached in its own function-local `static const MapData` (the symbolic one already is — generation costs ~0.7 s and must not be paid per world). Extend the grid-spans-the-map `static_assert` to cover both.
  - `make_world`: after the castle prebuild, `for (const PlacementDesc& d : config.plops) plop_building(*game, d);` — log a warning naming the index on a refused plop, since a silently-dropped structure is a hole in whatever it was part of.
  - `make_flat_world()` (`sim.cpp:164`) now sets `map = MapKind::FlatPlains`, making its name true for every test that uses it.
  - Perform every deletion in the table above.
  - Add `src/game/map/flat_map_generator.cpp` to `badlands_game_lib` and to `badlands_symbolic_map_tests`.

- [ ] **Step 4: Run** — `scripts/test.sh` in full. Watch `[movement]`, `[combat]`, `[intention]`, `[determinism]`, `[critter]`, `[exploration]`: `make_flat_world` changing maps moves the biome under any test that read it.

- [ ] **Step 5: Commit**

```bash
git add src/game/map/flat_map_generator.hpp src/game/map/flat_map_generator.cpp \
        game/include/badlands_sim.hpp game/src/sim.cpp game/src/game_state.h \
        game/src/movement.cpp game/tests/ CMakeLists.txt
git commit -m "feat(game): a world picks its map and declares its structures; the arena rectangle is gone"
```

---

## Task 3: The sandbox becomes a mode host

Everything from here is puppet master. **Nothing in this task is visible to the game.**

**Files:** delete `src/game/arena.{h,cpp}`; create `src/executables/ai_sandbox/{sandbox_mode.hpp, arena.hpp, arena.cpp, duel_mode.hpp, duel_mode.cpp, tests/ai_sandbox_tests.cpp}`; modify `src/executables/ai_sandbox/{main_ai_sandbox.cpp, ai_sandbox_view.{hpp,cpp}}`, `CMakeLists.txt`

**Interfaces produced:**

```cpp
// sandbox_mode.hpp -- the puppet master's contract. A mode says what world to
// build and who is in it; the game then runs, unaware. NOTHING in game/ or
// badlands_game_lib knows this type exists: no system, brain, or component can
// tell it is being driven by a mode rather than by a player.
//
// View-free BY DESIGN -- no SceneGraph, Camera, ImGui, SDL or Dawn -- so a mode
// is testable without a window and the host stays the only thing that draws.
class SandboxMode {
 public:
  virtual ~SandboxMode() = default;
  virtual const char* name() const = 0;
  // Initial config for the next world: map, plops, day length.
  virtual WorldConfig Configure() = 0;
  // Populate the freshly-built world, through the ordinary Sim API.
  virtual void Stage(Sim& sim) = 0;
  // One tick's snapshot. Returns true to ask the host for a FRESH world
  // (Configure + Stage again). Any logging the mode wants happens here.
  virtual bool Observe(const std::vector<CharacterState>& rows, int64_t world_millis) = 0;
  // One line of mode-specific text for the debug panel.
  virtual std::string Status() const = 0;
};
```

```cpp
// arena.hpp -- an arena is a list of buildings to plop. Nothing about it is
// special: the output is ordinary PlacementDescs that become ordinary
// buildings. The GAME never learns that these particular walls form a ring.
enum class ArenaShape : int32_t { Tube = 0, Octagon, Diamond, Count };
const char* arena_shape_name(ArenaShape s);

struct ArenaLayout {
    std::vector<PlacementDesc> plops;  // walls first, then columns
    glm::vec2 spawn_a{}, spawn_b{};    // opposed interior points, both reachable
    glm::vec2 half_extent{};           // outer footprint half-size (floor + framing)
};

// Every shape is built from ONE kind -- a 4x4 Wall -- at rotation 0 for
// axis-aligned runs and rotation 1 (45 deg) for diagonal ones. Adjacent blocks
// share an edge and leave no gap, which is exactly what plop_building permits
// and place_building does not.
ArenaLayout build_arena(ArenaShape shape);
```

**The shapes.** Dimensions are targets, not contracts — the tests assert invariants, so a generator may round to the lattice.

- **Tube** — a 40 × 16 m interior inside a one-block-thick rectangular ring. The long axis is ~5× a bow's 7 m reach: the kiting reference. No columns.
- **Octagon** — 32 m across the flats, each corner cut by a run of 45°-rotated blocks. No straight line longer than the width and no corner to be pinned in: the tube's opposite. No columns.
- **Diamond** — a square rotated 45°, ~44 m across the diagonals, walls entirely of 45° blocks. Four **single-block columns** at `(±8, 0)` and `(0, ±8)`: 4 m of solid obstacle each, 6 m after navmesh dilation, leaving ~5 m lanes and an open centre. The shape where a kiter can break line of pursuit.

Axis-aligned runs step by 4 m; 45° runs step by 3 m in both x and z, so `x+z` and `x−z` stay integral — which is what `snap_center` wants for an even diagonal span.

```cpp
// duel_mode.hpp -- sample a pairing and an arena, run it, log it, restage.
struct DuelConfig {
    uint64_t seed = 1;
    int32_t level = 1;
    int64_t max_millis = 60000;      // a minute of sim time, then a draw
    int64_t linger_millis = 5000;    // keep watching this long after a death
};

class DuelMode : public SandboxMode { /* ... */ };

// Every creature that can actually fight: not a Critter, and declaring at least
// one attack. Derived from the live catalog rather than hardcoded, so a
// creature added to the roster joins the pool the day it is added. Deer is
// excluded by that rule rather than by name -- and it must be, since a creature
// that never fights would never end a duel.
std::vector<CreatureId> duel_pool(const CreatureCatalog& catalog);

struct DuelSetup { ArenaShape shape; CreatureId left, right; };
// Pure: round index -> setup. Same seed and index give the same duel, so a
// session is reproducible without storing anything.
DuelSetup sample_duel(const DuelConfig& cfg, const std::vector<CreatureId>& pool,
                      uint32_t round);
```

Outcome is read off the snapshot rows the host already pulls: a team is alive while it has a living non-Critter row. One side left → linger, then log and restage. Both sides alive at `max_millis`, or both gone → a draw.

```
duel 12: Hunter vs MudGolem on diamond -> Hunter (team 0) in 38.4s
duel 13: Apprentice vs Bandit on tube -> draw (timeout) in 60.0s
```

**Host changes in `ai_sandbox_view.cpp`:**
- `SeedTown()` becomes `StageWorld()`: `mode_->Configure()` → construct `Sim` → `mode_->Stage(sim_)` → `BuildScene()` → `FrameCamera()`. The town seed, the deer herd, and `factors.json` loading go with it.
- `Update()` calls `mode_->Observe(char_rows_, world_millis)` each tick and restages when it returns true.
- **`AddWalls()` is deleted.** Arena walls are buildings, so `AddBuildings()` already draws them; the floor plane sizes off the mode's world rather than a tile count.
- **Hoist the hero wasm bytes to a member.** They are a local in `SeedTown` today (`:194`); every restage constructs a new `Sim`, and heroes with no wasm brain issue no intentions at all and lose every fight.
- `DrawInspector` gains one line: `mode_->Status()`.

`main_ai_sandbox.cpp` parses `--mode <name>` from its own argv (defaulting to `duel`) and constructs the mode before the factory — no engine change, since `SdlViewerApp` ignores flags it does not know.

- [ ] **Step 1: Write the failing tests** — `src/executables/ai_sandbox/tests/ai_sandbox_tests.cpp`

```cpp
// Arena invariants, run for EVERY ArenaShape in a loop -- so a fourth shape
// added later is covered the day it is added.
TEST_CASE("every arena plops completely", "[arena]") {
    // Build a flat world from the layout's plops; every plop returns a valid id
    // (none refused for overlap or bounds).
}
TEST_CASE("every arena is sealed", "[arena]") {
    // 4-connected flood fill over free tiles from spawn_a never reaches a tile
    // outside half_extent. A leak is the failure mode gapless plopping exists
    // to prevent, so it gets the direct test.
}
TEST_CASE("both spawn points are free and mutually reachable", "[arena]") {
    // Neither spawn is inside a footprint; the navmesh finds a path between
    // them. This is what proves clearance did not seal a corridor.
}
TEST_CASE("the diamond's columns obstruct", "[arena]") {
    // A straight line between two chosen points crosses a footprint, and the
    // navmesh path between them is strictly longer than that line.
}
TEST_CASE("the duel pool excludes anything that cannot fight", "[duel]") {
    // Deer absent; every entry declares an attack. Asserts the RULE against the
    // catalog it was handed, not a fixed roster listing.
}
TEST_CASE("sampling is a pure function of seed and round", "[duel]") {
    // Same (seed, round) twice -> identical setup; different rounds vary.
}
TEST_CASE("a duel ends when one side is gone, after the linger", "[duel]") {
    // Drive Observe with synthetic rows: still running at the death tick, done
    // exactly linger_millis later, winner = the surviving team.
}
TEST_CASE("a duel with both sides alive times out as a draw", "[duel]") { ... }
TEST_CASE("a duel with both sides gone is a draw, not a win", "[duel]") { ... }
```

- [ ] **Step 2: Register + run** — add `badlands_ai_sandbox_tests` (catch2 + `arena.cpp` + `duel_mode.cpp`, linking `badlands_game_lib` only — **no engine, no Dawn, no SDL**, which is the compile-time proof that a mode is view-free). FAIL.

- [ ] **Step 3: Implement** — delete `src/game/arena.{h,cpp}` and its `badlands_game_lib` entry; write the four new app files; rework the view into a mode host; add `--mode` parsing.

- [ ] **Step 4: Run and watch**

```bash
scripts/test.sh
perl -e 'alarm 180; exec @ARGV' ./build/badlands_ai_sandbox --mode duel
```

Confirm by eye: swings take time, a fighter **walks around** a column instead of through it, nobody escapes the ring, and one result line is logged per round.

- [ ] **Step 5: Commit**

```bash
git add src/executables/ai_sandbox/ CMakeLists.txt
git rm src/game/arena.h src/game/arena.cpp
git commit -m "feat(sandbox): the sandbox is a mode host, and duel is its first mode"
```

---

## Verification

1. `scripts/build.sh` → `BUILD OK`.
2. `scripts/test.sh` → green. Most likely to catch regressions: `[placement]`, `[movement]`, `[arena]`, `[duel]`, `[intention]`, `[determinism]`, `[critter]`, `[exploration]`.
3. **The layering holds:** `grep -rn "arena\|duel\|sandbox\|mode" game/ src/game/` (case-insensitive, excluding comments about the *player's* placement) returns nothing that names a sandbox concept. The game does not know what is driving it.
4. `grep -rn "arena_half" --include="*.cpp" --include="*.h" --include="*.hpp" .` → no hits outside `build/`. The rectangle is gone, not dormant.
5. Watch a duel session (Task 3, Step 4) — the eyeball checks listed there.
6. `perl -e 'alarm 30; exec @ARGV' ./build/badlands_game` → the town game is unaffected: buildings still place with margins, heroes still route around them.

## Declared but not executed

- **Walls do not stop projectiles or line of sight.** An arrow flies over a column and `select_target` sees through it. Cover is its own mechanic and is deliberately not smuggled in.
- **A wall does not refuse a step** — navmesh routing and footprint reprojection keep bodies out. If pressing units visibly jitter against walls during verification, a footprint step-refusal in `follow_paths` is the fix, and it is a *building* change that applies in town too.
- **Melee still ignores buildings** — a fighter can hit through a wall it is standing against. Inherited, unchanged.
- **The town seed is gone from the sandbox.** With it goes the only place that exercised needs, behaviours, and the economy under observation; `badlands_game` remains the app that shows those. A Town mode is the obvious second mode when that is wanted back.
- **Balance reporting is gone with duelsim** — no matrix, no calibration report against the threat anchors, no SVG charts. Duel log lines are what replaces it, read live.
- **Arena dimensions stay compiled.** Only the shape *choice* is sampled; the shapes themselves are lattice arithmetic, not tuning knobs.
- **Fleeing** remains the named follow-up from the previous slice; arenas with cover make it more interesting, not more urgent.
