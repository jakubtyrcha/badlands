# Phase 1a — Cleanup + Sim C++ API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the vestigial Rust host and replace the game simulation's data-only C ABI with a C++ `badlands::Sim` API over entt, behavior-identical, so later increments can unify the world onto one persistent registry.

**Architecture:** `badlands_game_lib` keeps existing as a static C++ library, but its public interface changes from `extern "C"` functions taking an opaque `BadlandsGame*` (`game/include/badlands_game.h`) to a C++ class `badlands::Sim` (`game/include/badlands_sim.hpp`) that still owns its own `entt::registry` internally. The internal systems (already free functions taking the world by reference) are renamed to take `Sim&`. The only live consumers — the C++ `GameView` and the sim/duel Catch2 tests — are moved onto the C++ API; then the C ABI header and its shims are deleted. No rendering, no world data flow, and no observable behavior changes in this increment: `Sim` still returns POD snapshot vectors exactly like the old ABI. The unification of the render registry and buildings-as-entities are **out of scope** here (later increments).

**Tech Stack:** C++23, entt (single-include), CMake+Ninja, Catch2. No new dependencies.

## Global Constraints

- **No `Game`/`game_` prefixes in new code; namespace everything in `badlands`.** The new API is `badlands::Sim` + `badlands::` result structs. (Forward-thinking naming — the project has near-zero legacy baggage.)
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

**Renamed / modified:**
- `game/src/game_state.h` → `game/src/sim_internal.hpp` — full internal state struct (`entt::registry`, `slots`, `brains`, `placement`, counters), now named `SimState`; `Sim`'s pimpl target.
- `game/src/game.cpp` → `game/src/sim.cpp` — implements `badlands::Sim` methods over `SimState`; keeps the tick order + snapshot logic.
- `game/src/{placement,movement,heroes,brain}.{h,cpp,hpp}` — systems change signature `BadlandsGame&` → `SimState&` (or `Sim&`; see Interfaces). Behavior unchanged.
- `src/game/views/game_view.{hpp,cpp}` — consume `badlands::Sim` instead of `game_*`.
- `game/tests/sim_tests.cpp`, `game/tests/duel_test.cpp`, `game/tests/duel_common.h` — use `badlands::Sim` instead of the C ABI.
- `game/tests/movement_tests.cpp`, `game/tests/heroes_tests.cpp`, `game/tests/placement_tests.cpp` — include `sim_internal.hpp`; `BadlandsGame`→`Sim`/`SimState` per rename.
- `src/game/scene/building_composer.cpp`, `src/game/scene/building_scene.cpp`, `src/game/views/model_viewer_view.{hpp,cpp}` — switch the handle-less helpers (`game_render_box`, `game_building_def`, `GameBuildingKind`) to their `badlands::` equivalents.
- `CMakeLists.txt` — `badlands_game_lib` sources (`game.cpp`→`sim.cpp`), public include stays `game/include`; remove nothing else.

**Deleted (Task 5 — after consumers migrated):**
- `game/include/badlands_game.h` (the C ABI).

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
  std::unique_ptr<struct SimState> state_;
};

// ---- handle-less helpers (were game_*; pure computations) ----------------
BuildingDef BuildingDefOf(BuildingKind kind);                    // was game_building_def
RenderBox RenderBoxOf(BuildingKind kind, int32_t rotation_index); // was game_render_box
CharacterDesc MercenaryDesc(float pos_x, float pos_z);           // was game_desc_mercenary
CharacterDesc GoblinDesc(float pos_x, float pos_z);             // was game_desc_goblin

}  // namespace badlands
```

- Consumers replace: `game_create(s)` → `badlands::Sim sim(s);`; `game_tick(g,dt)` → `sim.Tick(dt)`; `game_buildings(g,buf,cap)` → `auto v = sim.Buildings();`; `game_world` → `sim.World()`; `game_dispatch(g,&a)` → `sim.Dispatch(a)`; `game_render_box(k,r)` → `badlands::RenderBoxOf(k,r)`; etc.
- **Internal systems** keep operating on the full state: rename `BadlandsGame` → `SimState` and change signatures `plan_paths(SimState&, float)`, `follow_paths`, `update_melee_locks`, `separate_units`, `place_building`, `process_poppables`, `rebuild_occupancy`, `spawn_entity`, `recruit`, `destroy_building_impl`, `resume_brain`, etc. `SimState` holds `entt::registry registry; std::vector<entt::entity> slots; std::unique_ptr<BrainRuntime> brains; PlacementState placement; uint32_t gold; Pathfinder pathfinder; uint64_t ticks, script_intents; uint32_t noiser_bugs;`. `Sim::state_` points at one `SimState`; `Sim::registry()` returns `state_->registry`.

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

### Task 2: Introduce `badlands::Sim`; make the C ABI a thin shim over it

Rename the internal world to `SimState`, add the `Sim` class + public header, and reimplement the existing `game_*` C ABI functions as thin wrappers that call `Sim`. This keeps `sim_tests`/`duel_test`/`GameView` compiling and green **unchanged** mid-refactor, so the risky rename is isolated from the consumer migration.

**Files:**
- Create: `game/include/badlands_sim.hpp` (full content in Interfaces above — fill the elided enum members, Pathfinder signatures, and struct fields verbatim from `game/include/badlands_game.h`).
- Rename: `game/src/game_state.h` → `game/src/sim_internal.hpp`; struct `BadlandsGame` → `SimState`.
- Rename: `game/src/game.cpp` → `game/src/sim.cpp`; add `Sim` method bodies (delegating to the existing logic) + the handle-less helpers.
- Modify: `game/src/{placement,movement,heroes,brain}.{h,cpp}` — signatures `BadlandsGame&` → `SimState&`.
- Modify: `game/include/badlands_game.h` + a new `game/src/c_abi_shim.cpp` — the `game_*` functions now hold a `Sim` and forward to it (or `reinterpret_cast` the opaque handle to `Sim`). Simplest: `struct BadlandsGame` stays as the opaque C type == `badlands::Sim` via `game_create` returning `reinterpret_cast<BadlandsGame*>(new badlands::Sim(...))`.
- Modify: `CMakeLists.txt` — replace `game/src/game.cpp` with `game/src/sim.cpp` (+ `game/src/c_abi_shim.cpp`) in `badlands_game_lib` sources (CMakeLists.txt:227-250).

**Interfaces:**
- Consumes: the existing sim implementation (tick order `sim.cpp` ex-`game.cpp:174-262`, snapshot logic ex-`game.cpp:264-306` + `placement.cpp:531-610`).
- Produces: `badlands::Sim` (see Interfaces block) **and** the unchanged `game_*` C ABI as a shim, both valid simultaneously.

- [ ] **Step 1: Establish a behavior baseline before the rename**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
ctest --test-dir build --output-on-failure -R "game_tests" | tee /tmp/sim_baseline.txt
```
Expected: records the current pass/fail set of `badlands_game_tests` (green except the 3 noiser ICEs). This is the oracle Task 2 must not regress.

- [ ] **Step 2: Create the public header**

Create `game/include/badlands_sim.hpp` with the full content from the Interfaces section, replacing every `/* … */` ellipsis with the exact fields/members/signatures copied from `game/include/badlands_game.h` (enum members lines 89-101 & 162-167; `Pathfinder` fn-pointer sigs lines 216-222; struct fields per the cited lines). Keep field order identical to the C structs so the snapshot copies stay trivial.

- [ ] **Step 3: Rename the internal world struct**

```bash
cd /Users/jakub/repos/badlands-clone-2
git mv game/src/game_state.h game/src/sim_internal.hpp
```
Then in `sim_internal.hpp`: rename `struct BadlandsGame` → `struct SimState`, change the `GamePathfinder pathfinder{}` member type to `badlands::Pathfinder pathfinder{}`, and `#include "badlands_sim.hpp"` for the `Pathfinder`/enum types. Across `game/src/*.{h,cpp}` replace the type token `BadlandsGame` → `SimState` and includes of `"game_state.h"` → `"sim_internal.hpp"`:
```bash
grep -rl "BadlandsGame\|game_state.h" game/src | xargs sed -i '' 's/BadlandsGame/SimState/g; s/game_state\.h/sim_internal.hpp/g'
```
(Do NOT touch `game/include/badlands_game.h` yet — the opaque C `typedef struct BadlandsGame BadlandsGame;` there stays as the C handle name.)

- [ ] **Step 4: Rename the impl TU and add the `Sim` class + helpers**

```bash
git mv game/src/game.cpp game/src/sim.cpp
```
In `sim.cpp`: keep all existing free-function logic (now over `SimState&`). Add the pimpl + methods:
```cpp
#include "badlands_sim.hpp"
#include "sim_internal.hpp"
namespace badlands {
struct SimState;  // full def in sim_internal.hpp

Sim::Sim(const char* brain_script_source)
    : state_(std::make_unique<SimState>()) {
  // move here the body of the old game_create (game.cpp:111-162): brain runtime
  // init, origin Castle prebuild, etc., operating on *state_.
}
Sim::~Sim() = default;
Sim::Sim(Sim&&) noexcept = default;
Sim& Sim::operator=(Sim&&) noexcept = default;

void Sim::Tick(float dt) { /* old game_tick body (game.cpp:174-262) over *state_ */ }
uint32_t Sim::Spawn(const CharacterDesc& d) { /* old game_spawn over *state_ */ }
bool Sim::ReloadScript(const std::string& s) { /* old game_reload_script */ }
int64_t Sim::Dispatch(const Action& a) { /* old game_dispatch */ }
void Sim::SetPathfinder(const Pathfinder& pf) { state_->pathfinder = pf; }
std::vector<CharacterState> Sim::Characters() const { /* old game_state loop → vector */ }
std::vector<BuildingState> Sim::Buildings() const { /* old game_buildings → vector */ }
WorldState Sim::World() const { /* old game_world */ }
SimStats Sim::GetStats() const { /* old game_stats */ }
PlacementProbe Sim::ProbePlacement(const PlacementDesc& d,
    std::vector<GridTriangle>& out) const { /* old game_probe_placement */ }
entt::registry& Sim::registry() { return state_->registry; }
const entt::registry& Sim::registry() const { return state_->registry; }

BuildingDef BuildingDefOf(BuildingKind k) { /* old game_building_def */ }
RenderBox RenderBoxOf(BuildingKind k, int32_t r) { /* old game_render_box */ }
CharacterDesc MercenaryDesc(float x, float z) { /* old game_desc_mercenary */ }
CharacterDesc GoblinDesc(float x, float z) { /* old game_desc_goblin */ }
}  // namespace badlands
```
Note: the old handle-less helpers currently live in `placement.cpp` (`game_render_box`/`game_building_def` at placement.cpp:504-529) — move their bodies into `RenderBoxOf`/`BuildingDefOf` (or have the wrappers call the existing free functions; either is fine as long as the C ABI is not the definition site).

- [ ] **Step 5: Make the C ABI a thin shim (temporary)**

Create `game/src/c_abi_shim.cpp`. Keep `game/include/badlands_game.h` unchanged for now. Implement every `game_*` by treating the opaque handle as a `Sim`:
```cpp
#include "badlands_game.h"
#include "badlands_sim.hpp"
using badlands::Sim;
static Sim* S(BadlandsGame* g) { return reinterpret_cast<Sim*>(g); }
BadlandsGame* game_create(const char* s) { return reinterpret_cast<BadlandsGame*>(new Sim(s)); }
void game_destroy(BadlandsGame* g) { delete S(g); }
void game_tick(BadlandsGame* g, float dt) { S(g)->Tick(dt); }
uint32_t game_buildings(const BadlandsGame* g, GameBuildingState* out, uint32_t cap) {
  auto v = reinterpret_cast<const Sim*>(g)->Buildings();
  for (uint32_t i = 0; i < v.size() && i < cap; ++i)
    out[i] = /* memberwise copy badlands::BuildingState → GameBuildingState */;
  return static_cast<uint32_t>(v.size());
}
// … one forwarder per remaining game_* function, memberwise-copying the POD structs …
```
Remove the old definitions from `sim.cpp`/`placement.cpp` (they now live on `Sim`). Update `CMakeLists.txt:227-250`: swap `game/src/game.cpp` → `game/src/sim.cpp` and add `game/src/c_abi_shim.cpp`.

- [ ] **Step 6: Build**

Run: `cmake --build build`
Expected: compiles clean. Fix any missed `BadlandsGame`→`SimState` token or include.

- [ ] **Step 7: Run tests — verify no regression vs the baseline**

Run: `ctest --test-dir build --output-on-failure -R "game_tests"`
Expected: identical pass/fail set to `/tmp/sim_baseline.txt` (green except the 3 noiser ICEs). The C ABI still works via the shim, so `sim_tests`/`duel_test` pass unchanged; `movement_tests`/`heroes_tests` pass over `SimState`.

- [ ] **Step 8: Full build + full ctest (apps still link)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: all targets link (GameView still uses the C ABI shim), all green except the known ICEs.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor(sim): add badlands::Sim C++ API; C ABI becomes a thin shim over it

Rename the internal world BadlandsGame->SimState; systems now take SimState&.
New public header game/include/badlands_sim.hpp exposes class badlands::Sim with
the same snapshot semantics. The game_* C ABI is retained temporarily as a
forwarding shim so consumers migrate in the next tasks. Behavior identical.

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

- [ ] **Step 3: Fix internal-test includes**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
grep -rl "game_state.h\|BadlandsGame" game/tests | xargs sed -i '' 's/game_state\.h/sim_internal.hpp/g; s/BadlandsGame/SimState/g'
```
Then, where a test constructed the world directly, ensure it either builds a `badlands::Sim` and uses `sim.registry()` / `sim_internal.hpp` accessors, or constructs a bare `SimState` for a pure-system unit test — whichever matches the original intent.

- [ ] **Step 4: Build the test target**

Run: `cmake --build build --target badlands_game_tests`
Expected: compiles clean.

- [ ] **Step 5: Run the suite**

Run: `ctest --test-dir build --output-on-failure -R "game_tests"`
Expected: all sim/duel/movement/heroes/placement cases green; only the 3 noiser ICEs red (unchanged).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "test(sim): move sim/duel tests onto badlands::Sim; internal tests to sim_internal.hpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Delete the C ABI

Now that no live code calls `game_*`, remove the header + shim.

**Files:**
- Delete: `game/include/badlands_game.h`, `game/src/c_abi_shim.cpp`.
- Modify: `CMakeLists.txt` — drop `game/src/c_abi_shim.cpp` from `badlands_game_lib` sources.
- Modify (if any remain): `src/game/views/model_viewer_view.{hpp,cpp}`, `src/game/scene/building_scene.cpp`, `src/game/main_*.cpp` — any residual `#include "badlands_game.h"` / `GameBuildingKind` / `game_render_box` → `badlands_sim.hpp` / `badlands::BuildingKind` / `badlands::RenderBoxOf`.

**Interfaces:**
- Consumes: nothing new.
- Produces: `badlands_game_lib` with a single public header, `badlands_sim.hpp`. No `extern "C"` sim ABI anywhere.

- [ ] **Step 1: Find every remaining reference**

Run:
```bash
cd /Users/jakub/repos/badlands-clone-2
grep -rn "badlands_game\.h\|game_create\|game_tick\|game_state\|game_buildings\|game_dispatch\|game_render_box\|game_building_def\|GameBuildingKind\|GameBuildingState\|GameAction\|game_world\|game_spawn\|game_probe_placement\|game_stats" src game | grep -v "badlands_game_lib\|badlands_game_tests"
```
Expected: lists the stragglers (likely model_viewer + building_scene). If empty, skip Step 2.

- [ ] **Step 2: Convert the stragglers**

For each hit: swap the include to `badlands_sim.hpp`, `GameBuildingKind`→`badlands::BuildingKind`, `game_render_box(k,r)`→`badlands::RenderBoxOf(k,r)`, `game_building_def(k)`→`badlands::BuildingDefOf(k)`. (`model_viewer_view.hpp:19` includes the ABI purely for the enum + `GAME_BUILDING_KIND_COUNT` → use `badlands::BuildingKind` + `badlands::BuildingKind::Count`.)

- [ ] **Step 3: Delete the ABI files**

```bash
git rm game/include/badlands_game.h game/src/c_abi_shim.cpp
```
Then remove the `game/src/c_abi_shim.cpp` line from `CMakeLists.txt` (badlands_game_lib sources, ~227-250).

- [ ] **Step 4: Build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: full build succeeds with the ABI gone.

- [ ] **Step 5: Confirm the ABI is truly gone**

Run:
```bash
grep -rn "badlands_game\.h\|game_create\|game_tick\|game_buildings\|game_dispatch\|extern \"C\".*game_" src game
```
Expected: no hits.

- [ ] **Step 6: Full tests + all three apps smoke-run**

Run:
```bash
ctest --test-dir build --output-on-failure
perl -e 'alarm 30; exec @ARGV' ./build/badlands_game --screenshot /tmp/game_final.png
perl -e 'alarm 30; exec @ARGV' ./build/badlands_mapview --seed 2 --resolution 300x300 --preview-image-only --out /tmp/mv_final 2>/dev/null || ./build/badlands_mapview --preview-image-only --out /tmp/mv_final
```
Expected: tests green except the 3 noiser ICEs; `badlands_game` renders the demo town (compare `/tmp/game_final.png` to `/tmp/game_before.png` from Task 3 — identical scene); mapview unaffected.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(sim): delete the game C ABI; badlands::Sim is the only sim interface

No live code calls game_* anymore. game/include/badlands_game.h and the forwarding
shim are removed. badlands_game_lib now exposes one public header, badlands_sim.hpp.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage (design doc Phase 1 / this increment):**
- "sim's separate registry + data-only C ABI dissolve → C++ systems over a registry" → Tasks 2-5 (C ABI → `Sim`; note: full registry *unification* with the render side is the next increment, deliberately). ✓
- "delete the dead Rust host" (user decision) → Task 1. ✓
- "each app behaves identically" → screenshot parity (Task 3 Step 6, Task 5 Step 6) + test-baseline parity (every task). ✓
- "sim systems remain unit-testable on a bare registry" → Task 4 keeps movement/heroes/placement tests on `SimState`. ✓
- Out of scope, deferred to later increments (stated in Global Constraints): buildings-as-entities, `SyncToRegistry` removal, one shared registry, renderer-via-views. ✓

**Placeholder scan:** the header's `/* … */` are explicit "copy verbatim from badlands_game.h:<lines>" instructions with the exact source cited, not vague TODOs. The method bodies in Task 2 Step 4 say "old <fn> body over *state_" with the exact source line ranges — a mechanical move, not new logic. Acceptable for a behavior-preserving refactor.

**Type consistency:** `Sim::Characters()`/`Buildings()`/`World()`/`GetStats()`/`ProbePlacement()`/`Dispatch()`/`Spawn()`/`Tick()`/`registry()` are used identically in Tasks 3-4 as declared in the Interfaces header. `badlands::BuildingKind`/`BuildingState`/`Action`/`RenderBoxOf`/`BuildingDefOf` names are consistent across composer, view, and tests. `SimState` is the single internal-world name everywhere after Task 2 Step 3.
