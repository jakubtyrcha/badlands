# Phase 1a — Cleanup + Sim C++ API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the vestigial Rust host and replace the game simulation's data-only C ABI with a C++ `badlands::Sim` API over entt, behavior-identical, so later increments can unify the world onto one persistent registry.

**Architecture:** `badlands_game_lib` keeps existing as a static C++ library. Its public interface gains a C++ class `badlands::Sim` (`game/include/badlands_sim.hpp`) that owns the sim world and exposes tick/spawn/dispatch/snapshot as C++ methods. The migration is **strictly additive** at first: the existing internal world struct (`BadlandsGame`, in `game/src/game_state.h`) and the free-function systems over it are **left unchanged**; the shared operations (world construction, tick, the snapshot loops, dispatch, probe) are **extracted into `namespace badlands` free functions over `BadlandsGame&`** that both `Sim` and the existing `game_*` C ABI call. This keeps the C ABI and every existing test compiling and green throughout the risky middle of the refactor. `GameView` then moves onto `Sim` (Task 3); the sim/duel tests move onto `Sim` (Task 4); finally the C ABI is deleted and the remaining internal tests construct the world directly instead of via `game_create` (Task 5). No rendering, no world data flow, and no observable behavior changes: `Sim` returns POD snapshot vectors with the same semantics as the old ABI. Renaming the internal `BadlandsGame` struct, unifying the render registry, and buildings-as-entities are all **out of scope** here (later increments).

**Tech Stack:** C++23, entt (single-include), CMake+Ninja, Catch2. No new dependencies.

## Global Constraints

- **No `Game`/`game_` prefixes in new code; namespace everything in `badlands`.** The new API is `badlands::Sim` + `badlands::` result structs. (Forward-thinking naming — the project has near-zero legacy baggage.) The **existing** internal `BadlandsGame` struct is left as-is this increment — it is private to `game/src`, does not leak, and renaming it would break the internal tests mid-flight for no behavior gain. Rename it in a later cosmetic pass.
- **Additive, test-green-throughout.** Do NOT rename `BadlandsGame`, do NOT move `game_state.h`/`game.cpp`, and do NOT `reinterpret_cast` the C handle to `Sim`. The internal tests reach straight into `game_create(...)->registry` and call the free-function systems directly; those must keep compiling and passing at every task boundary.
- **Behavior-preserving.** Every existing C++ test that passes before this increment must pass after, and the three apps render identically. The one accepted pre-existing failure is the 3 noiser-compiler ICE asserts in `game/tests/noiser_smoke_tests.cpp` (unrelated; present on merge-base) — that stays red and is not caused by this work.
- **The sim still owns its own registry in this increment.** Do NOT wire `Sim` onto GameView's render registry yet, and do NOT touch `SceneGraph`/`SyncToRegistry`. Those are the next increment.
- **Buildings stay in `PlacementState`.** Do not migrate buildings to entt entities here.
- **Rust feature-libs are untouched.** `src/crates/{wesl,assets,nav}` and `crates/noiser-bundle` are the live Rust; only the dead root crate (`src/{main,app,game_ffi,nav,assets}.rs`, `src/{scene,gpu,ui}/`, root `Cargo.toml`, root `build.rs` if present, `tests/cpp_tests.rs` if present) is deleted.
- **Build/run from repo root** (shaders/assets resolve relative to cwd).
- Commit after each task. End commit messages with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer.

---

## File Structure

**Deleted (Task 1 — dead Rust host):**
- `src/main.rs`, `src/app.rs`, `src/game_ffi.rs`, `src/nav.rs`, `src/assets.rs`
- `src/scene/` (5 files), `src/gpu/` (6 files), `src/ui/` (6 files)
- root `Cargo.toml`, root `build.rs` (if it exists), `tests/cpp_tests.rs` (if it exists)
- Any now-orphaned root-crate glue (`.cargo/`, root `Cargo.lock`) **only if** confirmed to belong to the root crate and not to `src/crates/*`.

**Created:**
- `game/include/badlands_sim.hpp` — the public C++ API: `class badlands::Sim`, the `badlands::` POD result structs + enums, handle-less helpers.
- `game/src/sim.cpp` — `badlands::Sim` method bodies + the extracted shared free functions (`make_world`, `tick_world`, `characters_of`, `buildings_of`, `world_of`, `stats_of`, `probe_of`, `spawn_into`, `dispatch_into`) over `BadlandsGame&`, plus `RenderBoxOf`/`BuildingDefOf`/`MercenaryDesc`/`GoblinDesc`.
- `game/src/sim_internal.hpp` — declares the extracted free functions (above) so both `sim.cpp`, the C ABI (`game.cpp`), and the Task-5 internal tests can call them. Includes `game_state.h`.

**Modified (kept, not renamed):**
- `game/src/game.cpp` — the `game_*` C ABI bodies become thin wrappers that call the extracted free functions and memberwise-copy the `badlands::` result structs into the `Game*` POD out-params. Signatures + behavior unchanged. Still built.
- `game/src/game_state.h`, `game/src/{placement,movement,heroes,brain}.{h,cpp}` — **unchanged** (`BadlandsGame` stays; systems keep taking `BadlandsGame&`). `RenderBoxOf`/`BuildingDefOf` may reuse the existing `game_render_box`/`game_building_def` bodies in `placement.cpp` (call them, or move the bodies — either way the C ABI keeps working).
- `src/game/views/game_view.{hpp,cpp}` (Task 3) — consume `badlands::Sim` instead of `game_*`.
- `src/game/scene/building_composer.{hpp,cpp}` (Task 3) — handle-less helper `game_render_box` → `badlands::RenderBoxOf`; kind param → `badlands::BuildingKind`.
- `game/tests/sim_tests.cpp`, `game/tests/duel_test.cpp`, `game/tests/duel_common.h` (Task 4) — use `badlands::Sim` instead of the C ABI.
- `game/tests/movement_tests.cpp`, `heroes_tests.cpp`, `placement_tests.cpp` (Task 5) — replace `game_create(nullptr)` / `game_destroy(game)` with direct construction (`badlands::make_world(nullptr)`), keeping `->registry` and the direct system calls. Only touched once the C ABI is removed.
- `src/game/views/model_viewer_view.{hpp,cpp}`, `src/game/scene/building_scene.cpp` (Task 5) — residual `GameBuildingKind`/`game_render_box`/`badlands_game.h` → `badlands::` equivalents.
- `CMakeLists.txt` — add `game/src/sim.cpp` to `badlands_game_lib` sources; public include stays `game/include`.

**Deleted (Task 5 — after consumers migrated):**
- `game/include/badlands_game.h` (the C ABI) and the `game_*` bodies in `game.cpp`.

---

## Interfaces

**Produced by this increment (later increments and all consumers rely on these):**

`game/include/badlands_sim.hpp`, namespace `badlands`:

```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <entt/fwd.hpp>   // entt::registry (forward)
#include <glm/glm.hpp>    // only if a struct needs glm; result PODs below use plain floats

namespace badlands {

// ---- enums (were GameBuildingKind / GameActionKind) --------------------
enum class BuildingKind : int32_t {
  Castle = 0, /* … keep the exact 10 members + order from badlands_game.h:89-101 … */
  Sewer, Count
};
enum class ActionKind : int32_t {
  PlaceBuilding = 0, RecruitHero, DestroyBuilding, Count
};
inline constexpr int32_t kGridHalfExtentTiles = 48;  // was GAME_GRID_HALF_EXTENT_TILES

// ---- POD result structs (field-for-field from badlands_game.h) ----------
struct CharacterDesc { float pos_x, pos_z; int32_t team; float hp, move_speed,
  attack_range, attack_damage, attack_cooldown, size_x, size_y, size_z,
  color_r, color_g, color_b; };
struct CharacterState { uint32_t id; int32_t team; float pos_x, pos_z, hp, max_hp,
  size_x, size_y, size_z, color_r, color_g, color_b; int32_t home_building_id,
  inside_building_id; };
// NB: NOT `Stats` — badlands::Stats already exists (a sim component,
// game/src/components.h:24). Use SimStats for the run counters.
struct SimStats { uint64_t ticks, script_intents; uint32_t noiser_bugs; };
struct BuildingDef { int32_t width_tiles, depth_tiles; bool poppable,
  user_destructible, enemy_targettable; };
struct RenderBox { float size_x, size_z, yaw_radians; };
struct PlacementDesc { int32_t kind, rotation_index; float world_x, world_z; };
struct GridTriangle { int32_t tile_x, tile_z; uint32_t corner, state; };
struct PlacementProbe { bool valid; float snapped_x, snapped_z; };
struct Action { ActionKind kind; uint32_t target_id; float world_x, world_z;
  int32_t param_a, param_b; };
struct BuildingState { uint32_t id; BuildingKind kind; float center_x, center_z;
  int32_t rotation_index, width_tiles, depth_tiles; };
struct WorldState { uint32_t gold; int32_t grid_half_extent_tiles;
  uint32_t queued_poppables, urban_quarters, guild_roster_cap; };

// Injected Rust nav provider (was GamePathfinder) — kept as-is, by value.
struct Pathfinder {
  void* ctx = nullptr;
  void (*add_obstacle)(void*, /*…exact sig from badlands_game.h:216-222…*/) = nullptr;
  void (*remove_obstacle)(void*, /*…*/) = nullptr;
  int32_t (*find_path)(void*, /*…*/) = nullptr;
};

// ---- the sim ------------------------------------------------------------
class Sim {
 public:
  explicit Sim(const char* brain_script_source = nullptr);
  ~Sim();
  Sim(Sim&&) noexcept;
  Sim& operator=(Sim&&) noexcept;
  Sim(const Sim&) = delete;
  Sim& operator=(const Sim&) = delete;

  uint32_t Spawn(const CharacterDesc& desc);
  void Tick(float dt);
  bool ReloadScript(const std::string& source);
  int64_t Dispatch(const Action& action);
  void SetPathfinder(const Pathfinder& pf);

  // Snapshot accessors — identical semantics to the old ABI, POD vectors.
  std::vector<CharacterState> Characters() const;   // was game_state
  std::vector<BuildingState> Buildings() const;      // was game_buildings
  WorldState World() const;                          // was game_world
  SimStats GetStats() const;                         // was game_stats
  // Placement preview; returns validity, fills out_triangles (was game_probe_placement).
  PlacementProbe ProbePlacement(const PlacementDesc& desc,
                                std::vector<GridTriangle>& out_triangles) const;

  // The shared world. Later increments read render/sim components off this.
  entt::registry& registry();
  const entt::registry& registry() const;

 private:
  std::unique_ptr<struct BadlandsGame> world_;   // the EXISTING internal world, unchanged
};

// ---- handle-less helpers (were game_*; pure computations) ----------------
BuildingDef BuildingDefOf(BuildingKind kind);                    // was game_building_def
RenderBox RenderBoxOf(BuildingKind kind, int32_t rotation_index); // was game_render_box
CharacterDesc MercenaryDesc(float pos_x, float pos_z);           // was game_desc_mercenary
CharacterDesc GoblinDesc(float pos_x, float pos_z);             // was game_desc_goblin

}  // namespace badlands
```

`game/src/sim_internal.hpp`, namespace `badlands` — the shared operations, extracted so both `Sim` and the C ABI call one implementation (this is what makes the increment additive):

```cpp
#pragma once
#include <memory>
#include <vector>
#include "badlands_sim.hpp"
#include "game_state.h"          // struct BadlandsGame (UNCHANGED)
namespace badlands {
std::unique_ptr<BadlandsGame> make_world(const char* brain_script_source);
void tick_world(BadlandsGame&, float dt);
uint32_t spawn_into(BadlandsGame&, const CharacterDesc&);
int64_t dispatch_into(BadlandsGame&, const Action&);
bool reload_script(BadlandsGame&, const std::string&);
std::vector<CharacterState> characters_of(const BadlandsGame&);
std::vector<BuildingState>  buildings_of(const BadlandsGame&);
WorldState world_of(const BadlandsGame&);
SimStats   stats_of(const BadlandsGame&);
PlacementProbe probe_of(const BadlandsGame&, const PlacementDesc&, std::vector<GridTriangle>&);
}  // namespace badlands
```

- **`Sim` pimpl:** `world_` is a `std::unique_ptr<BadlandsGame>` initialized by `make_world(script)`; `Sim::Tick` → `tick_world(*world_, dt)`; `Sim::Characters` → `characters_of(*world_)`; `Sim::registry()` → `world_->registry`; etc. `Sim`'s destructor is defined out-of-line in `sim.cpp` (where `BadlandsGame` is complete).
- **The C ABI (`game.cpp`) keeps its own lifecycle:** `game_create(s)` → `return make_world(s).release();` (a real `BadlandsGame*`, so the internal tests' `game_create(...)->registry` and `plan_paths(*game, dt)` keep working); `game_destroy(g)` → `delete g;`; `game_tick(g,dt)` → `tick_world(*g, dt)`; `game_buildings(g,out,cap)` → copy `buildings_of(*g)` into `out` with the same truncation idiom, memberwise-mapping `badlands::BuildingState`→`GameBuildingState` (incl. `BuildingKind`→`int32_t`). Same for `game_state`/`game_world`/`game_stats`/`game_dispatch`/`game_probe_placement`/`game_spawn`/`game_reload_script`.
- **The internal systems are UNCHANGED** — `plan_paths(BadlandsGame&, float)`, `follow_paths`, `update_melee_locks`, `separate_units`, `place_building`, `process_poppables`, `rebuild_occupancy`, `spawn_entity`, `recruit`, `destroy_building_impl`, `resume_brain`, etc. keep taking `BadlandsGame&`. `make_world`/`tick_world`/the `*_of` snapshots are the bodies moved out of the old `game_create`/`game_tick`/`game_state`/… — a move, not new logic.
- **Consumers replace:** `game_create(s)` → `badlands::Sim sim(s);`; `game_tick(g,dt)` → `sim.Tick(dt)`; `game_buildings(g,buf,cap)` → `auto v = sim.Buildings();`; `game_world` → `sim.World()`; `game_dispatch(g,&a)` → `sim.Dispatch(a)`; `game_render_box(k,r)` → `badlands::RenderBoxOf(k,r)`.

---

### Task 1: Delete the dead Rust host

**Files:**
- Delete: `src/main.rs`, `src/app.rs`, `src/game_ffi.rs`, `src/nav.rs`, `src/assets.rs`, `src/scene/`, `src/gpu/`, `src/ui/`, root `Cargo.toml`, root `build.rs` (if present), `tests/cpp_tests.rs` (if present), root `Cargo.lock` (only if it is the root crate's).
- Test: full C++ build + `ctest`.

**Interfaces:**
- Consumes: nothing.
- Produces: a tree with no vestigial Rust host. No symbol other code depends on is removed (verified: CMake builds only `src/crates/*` + `crates/noiser-bundle`).

- [ ] **Step 1: Re-verify nothing live references the targets**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
grep -rn "src/main.rs\|src/app\|src/scene\|src/gpu\|src/ui\|game_ffi\|src/nav.rs\|src/assets.rs" CMakeLists.txt cmake/
grep -rln "mod app\|mod scene\|mod gpu\|mod ui\|crate::app\|crate::scene\|crate::gpu\|crate::ui" src/crates crates
ls tests/cpp_tests.rs build.rs 2>/dev/null
```
Expected: first two commands print nothing (no CMake ref, no live-crate ref). Third lists whichever root-crate files exist (to include in the delete set). If either grep prints a hit, STOP and report — the assumption is wrong.

- [ ] **Step 2: Delete the files**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
git rm -r src/main.rs src/app.rs src/game_ffi.rs src/nav.rs src/assets.rs src/scene src/gpu src/ui Cargo.toml
git rm build.rs 2>/dev/null || true
git rm tests/cpp_tests.rs 2>/dev/null || true
# Cargo.lock: only if it belongs to the root crate (it does — the live crates have their own under src/crates/*/ and third_party/*)
git rm Cargo.lock 2>/dev/null || true
```
Expected: files staged for deletion. (`git rm` refuses if a path doesn't exist — the `|| true` guards the optional ones.)

- [ ] **Step 3: Configure + build the C++ project**

Run:
```bash
cmake -S . -B build -G Ninja && cmake --build build
```
Expected: configure + build succeed (Corrosion imports only `src/crates/*` + `noiser-bundle`; the deleted root crate was never in the build). If CMake errors about a missing path, the deletion caught a live file — restore it and report.

- [ ] **Step 4: Run the C++ test suite**

Run:
```bash
ctest --test-dir build --output-on-failure
```
Expected: same result as before this task — everything green **except** the 3 pre-existing noiser ICE asserts in `noiser_smoke_tests.cpp`. No NEW failures.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore: delete the vestigial Rust host (winit/wgpu), superseded by the C++ replatform

The root badlands crate (src/{main,app,game_ffi,nav,assets}.rs, src/{scene,gpu,ui})
was marked Dropped by the 2026-07-14 replatform spec but never removed. It is not
built by CMake (Corrosion imports only src/crates/* + noiser-bundle). No live code
references it.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Introduce `badlands::Sim` over the unchanged world (additive)

Add the `Sim` class + public header, extract the shared operations into `namespace badlands` free functions over the **unchanged** `BadlandsGame`, and re-point the `game_*` C ABI bodies at those same functions. Nothing is renamed; the C ABI keeps its exact signatures + behavior. `Sim` and the C ABI both compile; every existing test stays green.

**Files:**
- Create: `game/include/badlands_sim.hpp` (full content in Interfaces above — fill the elided enum members, Pathfinder signatures, and struct fields verbatim from `game/include/badlands_game.h`).
- Create: `game/src/sim_internal.hpp` (the extracted free-function declarations, in Interfaces above).
- Create: `game/src/sim.cpp` — `Sim` methods + the extracted free-function bodies + `RenderBoxOf`/`BuildingDefOf`/`MercenaryDesc`/`GoblinDesc`.
- Modify: `game/src/game.cpp` — `game_*` bodies become thin wrappers over the extracted free functions (see Interfaces). Do NOT rename anything; do NOT `reinterpret_cast`.
- Leave UNCHANGED: `game/src/game_state.h` (`struct BadlandsGame`), `game/src/{placement,movement,heroes,brain}.{h,cpp}`, `game/include/badlands_game.h`, and all `game/tests/*`.
- Modify: `CMakeLists.txt` — add `game/src/sim.cpp` to `badlands_game_lib` sources (CMakeLists.txt:227-250). `game.cpp` stays in the target.

**Interfaces:**
- Consumes: the existing sim implementation — the bodies of `game_create` (game.cpp:111-162), `game_tick` (game.cpp:174-262), the snapshot loops (game.cpp:264-306, placement.cpp:531-610), `game_dispatch`, `game_spawn`, `game_reload_script`.
- Produces: `badlands::Sim` + the extracted free functions (Interfaces block) **and** the byte-identical `game_*` C ABI, both valid simultaneously.

- [ ] **Step 1: Establish a behavior baseline**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
ctest --test-dir build --output-on-failure -R "game_tests" | tee /tmp/sim_baseline.txt
```
Expected: records the current pass/fail set of `badlands_game_tests` (green except the 3 noiser ICEs). This is the oracle Task 2 must not regress.

- [ ] **Step 2: Create the public header**

Create `game/include/badlands_sim.hpp` with the full content from the Interfaces section, replacing every `/* … */` ellipsis with the exact fields/members/signatures copied from `game/include/badlands_game.h` (enum members lines 89-101 & 162-167; `Pathfinder` fn-pointer sigs lines 216-222; struct fields per the cited lines). Keep field order identical to the C structs so the copies stay trivial. Remember: the run-counter struct is `SimStats`, **not** `Stats` (collision with the existing component `badlands::Stats`, `game/src/components.h:24`).

- [ ] **Step 3: Extract the shared operations into free functions**

Create `game/src/sim_internal.hpp` (declarations from the Interfaces block). Create `game/src/sim.cpp` and **move** — do not copy — the bodies out of the current `game_*` functions into these free functions over `BadlandsGame&`:
- `make_world(script)` ← the body of `game_create` (brain-runtime init, origin-Castle prebuild), returning `std::unique_ptr<BadlandsGame>`.
- `tick_world(g, dt)` ← the body of `game_tick`.
- `characters_of(g)` ← the `game_state` loop, appending `CharacterState` rows to a returned vector (no cap).
- `buildings_of(g)` ← the `game_buildings` loop (from placement.cpp) → vector.
- `world_of` / `stats_of` / `probe_of` / `spawn_into` / `dispatch_into` / `reload_script` ← the respective bodies.
- `RenderBoxOf`/`BuildingDefOf` ← reuse or move the `game_render_box`/`game_building_def` bodies (placement.cpp:504-529). `MercenaryDesc`/`GoblinDesc` ← the `game_desc_*` bodies.

Then define the `Sim` methods, each forwarding to its free function over `*world_` (e.g. `void Sim::Tick(float dt){ tick_world(*world_, dt); }`, `Sim::Sim(const char* s): world_(make_world(s)) {}`, `entt::registry& Sim::registry(){ return world_->registry; }`). `Sim::~Sim()` is defined here (out-of-line) so `BadlandsGame` is complete.

- [ ] **Step 4: Re-point the C ABI bodies at the free functions**

In `game.cpp`, rewrite each `game_*` body as a thin wrapper (signatures unchanged):
```cpp
BadlandsGame* game_create(const char* s) { return badlands::make_world(s).release(); }
void game_destroy(BadlandsGame* g) { delete g; }
void game_tick(BadlandsGame* g, float dt) { badlands::tick_world(*g, dt); }
uint32_t game_buildings(const BadlandsGame* g, GameBuildingState* out, uint32_t cap) {
  const auto v = badlands::buildings_of(*g);
  for (uint32_t i = 0; i < v.size() && i < cap; ++i) {
    out[i].id = v[i].id;
    out[i].kind = static_cast<int32_t>(v[i].kind);      // BuildingKind -> int32
    out[i].center_x = v[i].center_x; out[i].center_z = v[i].center_z;
    out[i].rotation_index = v[i].rotation_index;
    out[i].width_tiles = v[i].width_tiles; out[i].depth_tiles = v[i].depth_tiles;
  }
  return static_cast<uint32_t>(v.size());
}
// … one wrapper per remaining game_* function, memberwise-mapping the POD structs …
```
`game_create` returns a real `BadlandsGame*`, so the internal tests' `game_create(...)->registry` and `plan_paths(*game, dt)` keep working unchanged. Add `game/src/sim.cpp` to `CMakeLists.txt:227-250`.

- [ ] **Step 5: Build**

Run: `cmake --build build`
Expected: compiles clean. If `badlands::Stats` clashes anywhere, you used `Stats` instead of `SimStats` — fix it.

- [ ] **Step 6: Run tests — verify no regression vs the baseline**

Run: `ctest --test-dir build --output-on-failure -R "game_tests"`
Expected: identical pass/fail set to `/tmp/sim_baseline.txt` (green except the 3 noiser ICEs). The C ABI is unchanged in behavior, so `sim_tests`/`duel_test`/`movement_tests`/`heroes_tests` all pass exactly as before.

- [ ] **Step 7: Full build + full ctest (apps still link)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all targets link (GameView still uses the C ABI), all green except the known ICEs.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(sim): add badlands::Sim C++ API over the existing world (additive)

New public header game/include/badlands_sim.hpp exposes class badlands::Sim; the
shared operations are extracted into namespace-badlands free functions over the
unchanged BadlandsGame, and the game_* C ABI now forwards to them. Signatures and
behavior of the C ABI are unchanged; every existing test stays green. Consumers
migrate onto Sim in the following tasks.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Migrate `GameView` onto `badlands::Sim`

**Files:**
- Modify: `src/game/views/game_view.hpp` — replace `BadlandsGame* game_` (game_view.hpp:116) with `badlands::Sim sim_;` (value member, RAII); drop `#include "badlands_game.h"`, add `#include "badlands_sim.hpp"`; change `std::vector<GameBuildingState> building_rows_` (game_view.hpp:120) → `std::vector<badlands::BuildingState>`.
- Modify: `src/game/views/game_view.cpp` — every `game_*` call → `sim_` method (sites: 159 create, 107 destroy, 239 dispatch, 367 tick, 305 buildings, 442 world, 450 buildings-in-DrawUI).
- Modify: `src/game/scene/building_composer.{hpp,cpp}` — `AddBuildingToComposer` takes `badlands::BuildingKind`; `game_render_box(kind,0)` (building_composer.cpp:21) → `badlands::RenderBoxOf(kind, 0)`.

**Interfaces:**
- Consumes: `badlands::Sim`, `badlands::BuildingState`, `badlands::RenderBoxOf`, `badlands::Action`, `badlands::BuildingKind` (Task 2).
- Produces: a `GameView` with no C ABI dependency. Still uses its own render registry + `SceneGraph`/`SyncToRegistry` (unchanged — next increment).

- [ ] **Step 1: Baseline the game app's current render**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
./build/badlands_game --screenshot /tmp/game_before.png
```
Expected: a PNG of the demo town (Castle + 4 buildings). This is the visual oracle.

- [ ] **Step 2: Convert the handle + lifecycle**

In `game_view.hpp`: `#include "badlands_sim.hpp"` (drop `badlands_game.h`); replace the raw handle with `badlands::Sim sim_{nullptr};` (value; `nullptr` script = mock brains, matching old `game_create(nullptr)`); `building_rows_` → `std::vector<badlands::BuildingState>`. In `game_view.cpp`: delete `game_ = game_create(nullptr);` (159) and `game_destroy(game_);` (107) — RAII handles both.

- [ ] **Step 3: Convert tick, dispatch, and the snapshot reads**

- Tick (367): `if (game_) game_tick(game_, dt);` → `sim_.Tick(static_cast<float>(kTickDt));`
- Seed (239): build a `badlands::Action{ .kind = badlands::ActionKind::PlaceBuilding, … }` and `sim_.Dispatch(action);`
- BuildScene buildings (305-312): `const auto rows = sim_.Buildings();` then loop `rows` (drop the `game_buildings(...)` count-cap idiom); `static_cast<GameBuildingKind>(b.kind)` becomes just `b.kind` (already `badlands::BuildingKind`).
- DrawUI world (442): `const auto world = sim_.World();`
- DrawUI buildings (450): `const auto rows = sim_.Buildings();` then the ImGui list over `rows`.

- [ ] **Step 4: Convert the composer**

In `building_composer.{hpp,cpp}`: change the `AddBuildingToComposer` kind parameter type to `badlands::BuildingKind`; replace `game_render_box(kind, 0)` (building_composer.cpp:21) with `badlands::RenderBoxOf(kind, 0)`. Update the call in `game_view.cpp:312` accordingly.

- [ ] **Step 5: Build**

Run: `cmake --build build`
Expected: `badlands_game` links with no `game_*` references left in the game-app TUs.

- [ ] **Step 6: Screenshot parity check**

Run:
```bash
./build/badlands_game --screenshot /tmp/game_after.png
git diff --no-index --stat -- /tmp/game_before.png /tmp/game_after.png; echo "compare visually:"; ls -l /tmp/game_before.png /tmp/game_after.png
```
Expected: `/tmp/game_after.png` shows the identical demo town (same Castle + 4 buildings, same layout/colors). Open both to confirm (byte-identical is ideal but not required — same scene is the gate).

- [ ] **Step 7: Full build + tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all green except the 3 known noiser ICEs.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor(game): drive GameView through badlands::Sim, not the C ABI

RAII Sim value member replaces the opaque BadlandsGame* handle; tick/dispatch/
buildings/world go through the C++ API; building composer takes badlands::
BuildingKind + RenderBoxOf. Render output unchanged (screenshot parity). SceneGraph
sync is untouched — that is the next increment.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Migrate the sim/duel tests onto `badlands::Sim`

**Files:**
- Modify: `game/tests/sim_tests.cpp` — `game_create/spawn/tick/state` → `badlands::Sim` methods; include `badlands_sim.hpp` not `badlands_game.h`.
- Modify: `game/tests/duel_test.cpp`, `game/tests/duel_common.h` — same conversion for the duel harness.
- Modify (includes only): `game/tests/movement_tests.cpp`, `heroes_tests.cpp`, `placement_tests.cpp` — `#include "sim_internal.hpp"` (was `game_state.h`); any `BadlandsGame game` local → `SimState` (or construct a `Sim` and use `sim.registry()`/internal accessors). These already touch internals directly, so only the rename ripples.

**Interfaces:**
- Consumes: `badlands::Sim`, `SimState`, the sim systems (Task 2).
- Produces: a test suite that exercises the C++ API + internals, with no `game_*` C ABI use.

- [ ] **Step 1: Convert `sim_tests.cpp`**

Replace the pure-ABI flow, e.g.:
```cpp
// before: BadlandsGame* g = game_create(nullptr); uint32_t id = game_spawn(g, &d);
//         game_tick(g, dt); uint32_t n = game_state(g, buf, cap); … game_destroy(g);
// after:
badlands::Sim sim(nullptr);
uint32_t id = sim.Spawn(badlands::MercenaryDesc(-8.f, -12.f));
sim.Tick(0.1f);
auto rows = sim.Characters();
REQUIRE(rows.size() == /* expected */);
```
Keep every assertion's intent identical; only the access path changes.

- [ ] **Step 2: Convert the duel harness**

In `duel_common.h`/`duel_test.cpp`, replace the `game_*` calls with `badlands::Sim` equivalents (spawn both fighters via `sim.Spawn(...)`, loop `sim.Tick(dt)`, read `sim.Characters()` for HP/positions).

- [ ] **Step 3: Leave the internal tests alone**

`movement_tests.cpp`, `heroes_tests.cpp`, `placement_tests.cpp` still use `game_create(nullptr)` + `game->registry` + direct system calls, and the C ABI still exists and works — so **do not touch them in this task**. They convert off `game_create` in Task 5, when the C ABI is removed. Only `sim_tests.cpp`, `duel_test.cpp`, and `duel_common.h` change here.

- [ ] **Step 4: Build the test target**

Run: `cmake --build build --target badlands_game_tests`
Expected: compiles clean.

- [ ] **Step 5: Run the suite**

Run: `ctest --test-dir build --output-on-failure -R "game_tests"`
Expected: all sim/duel/movement/heroes/placement cases green; only the 3 noiser ICEs red (unchanged).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "test(sim): move sim/duel tests onto badlands::Sim

Internal system tests (movement/heroes/placement) still use game_create and are
untouched here — they convert in Task 5 when the C ABI is removed.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Delete the C ABI

Convert the remaining C-ABI consumers (the internal tests + any straggler geometry callers), then remove the header and the `game_*` bodies.

**Files:**
- Delete: `game/include/badlands_game.h`; remove the `game_*` function bodies from `game/src/game.cpp` (delete the file if nothing else lives in it, and drop it from CMake).
- Modify: `game/tests/movement_tests.cpp`, `heroes_tests.cpp`, `placement_tests.cpp` — replace `game_create(nullptr)`/`game_destroy(game)` with direct construction; keep `->registry` and the direct system calls.
- Modify (if any remain): `src/game/views/model_viewer_view.{hpp,cpp}`, `src/game/scene/building_scene.cpp`, `src/game/main_*.cpp` — residual `#include "badlands_game.h"` / `GameBuildingKind` / `game_render_box` → `badlands_sim.hpp` / `badlands::BuildingKind` / `badlands::RenderBoxOf`.

**Interfaces:**
- Consumes: `badlands::make_world` (Task 2) for the internal tests' construction.
- Produces: `badlands_game_lib` with a single public header, `badlands_sim.hpp`. No `extern "C"` sim ABI anywhere.

- [ ] **Step 1: Convert the internal tests off `game_create`**

In `movement_tests.cpp`, `heroes_tests.cpp`, `placement_tests.cpp`: these include `game_state.h` and hold a `BadlandsGame* game`. Replace construction/teardown while keeping every `game->registry.*` access and direct system call (`plan_paths(*game, dt)`, etc.) intact:
```cpp
// before: BadlandsGame* game = game_create(nullptr); … game_destroy(game);
// after:
auto owned = badlands::make_world(nullptr);   // #include "sim_internal.hpp"
BadlandsGame* game = owned.get();
// … body unchanged (game->registry, plan_paths(*game, dt), etc.) …
// no game_destroy — unique_ptr cleans up at scope exit
```
Any `game_dispatch(game, &a)` in `heroes_tests` → `badlands::dispatch_into(*game, a)` (map the `GameAction` fields to `badlands::Action`), or keep calling the internal free functions (`recruit`, `destroy_building_impl`) the test already uses.

- [ ] **Step 2: Find every remaining `game_*` / ABI reference**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
grep -rn "badlands_game\.h\|game_create\|game_tick\|game_state\|game_buildings\|game_dispatch\|game_render_box\|game_building_def\|GameBuildingKind\|GameBuildingState\|GameAction\|game_world\|game_spawn\|game_probe_placement\|game_stats" src game
```
Expected: lists the remaining stragglers (likely model_viewer + building_scene). If empty, skip Step 3.

- [ ] **Step 3: Convert the stragglers**

For each hit: swap the include to `badlands_sim.hpp`, `GameBuildingKind`→`badlands::BuildingKind`, `game_render_box(k,r)`→`badlands::RenderBoxOf(k,r)`, `game_building_def(k)`→`badlands::BuildingDefOf(k)`. (`model_viewer_view.hpp:19` includes the ABI purely for the enum + `GAME_BUILDING_KIND_COUNT` → use `badlands::BuildingKind` + `badlands::BuildingKind::Count`.)

- [ ] **Step 4: Delete the ABI**

```bash
git rm game/include/badlands_game.h
```
Remove the `game_*` bodies from `game/src/game.cpp`; if the file is now empty, `git rm` it and drop it from `CMakeLists.txt` (badlands_game_lib sources, ~227-250).

- [ ] **Step 5: Build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: full build succeeds with the ABI gone.

- [ ] **Step 6: Confirm the ABI is truly gone**

Run:
```bash
grep -rn "badlands_game\.h\|game_create\|game_tick\|game_buildings\|game_dispatch\|extern \"C\".*game_" src game
```
Expected: no hits.

- [ ] **Step 7: Full tests + all three apps smoke-run**

Run:
```bash
ctest --test-dir build --output-on-failure
perl -e 'alarm 30; exec @ARGV' ./build/badlands_game --screenshot /tmp/game_final.png
perl -e 'alarm 30; exec @ARGV' ./build/badlands_mapview --seed 2 --resolution 300x300 --preview-image-only --out /tmp/mv_final 2>/dev/null || ./build/badlands_mapview --preview-image-only --out /tmp/mv_final
```
Expected: tests green except the 3 noiser ICEs; `badlands_game` renders the demo town (compare `/tmp/game_final.png` to `/tmp/game_before.png` from Task 3 — identical scene); mapview unaffected.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor(sim): delete the game C ABI; badlands::Sim is the only sim interface

No code calls game_* anymore — the internal tests construct the world via
badlands::make_world. game/include/badlands_game.h and the game_* bodies are
removed. badlands_game_lib now exposes one public header, badlands_sim.hpp.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage (design doc Phase 1 / this increment):**
- "sim's separate registry + data-only C ABI dissolve → C++ systems over a registry" → Tasks 2-5 (C ABI → `Sim`; note: full registry *unification* with the render side is the next increment, deliberately). ✓
- "delete the dead Rust host" (user decision) → Task 1. ✓
- "each app behaves identically" → screenshot parity (Task 3 Step 6, Task 5 Step 7) + test-baseline parity (every task). ✓
- "sim systems remain unit-testable on a bare registry" → the movement/heroes/placement tests keep exercising the systems directly over `BadlandsGame` (via `game_create` through Task 4, via `make_world` from Task 5). ✓
- Out of scope, deferred to later increments (stated in Global Constraints): buildings-as-entities, `SyncToRegistry` removal, one shared registry, renderer-via-views, and the cosmetic `BadlandsGame`→`SimState` rename. ✓

**Placeholder scan:** the header's `/* … */` are explicit "copy verbatim from badlands_game.h:<lines>" instructions with the exact source cited, not vague TODOs. Task 2's free-function bodies say "move the body of `game_<fn>` (game.cpp:<lines>)" — a mechanical relocation, not new logic. Acceptable for a behavior-preserving refactor.

**Type consistency:** `Sim::Characters()`/`Buildings()`/`World()`/`GetStats()`/`ProbePlacement()`/`Dispatch()`/`Spawn()`/`Tick()`/`registry()` are used identically in Tasks 3-4 as declared in the Interfaces header. The extracted free functions (`make_world`/`tick_world`/`characters_of`/`buildings_of`/`world_of`/`stats_of`/`probe_of`/`spawn_into`/`dispatch_into`) are declared once in `sim_internal.hpp` and used by `Sim`, the C ABI, and (Task 5) the internal tests. `badlands::BuildingKind`/`BuildingState`/`Action`/`RenderBoxOf`/`BuildingDefOf` names are consistent across composer, view, and tests. The run-counter struct is `SimStats` (not `Stats`) everywhere.
