# The Intention Contract (Slice B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-tick brain wire with the intention contract — brain SUGGESTS one intention per wakeup (with an idleness hint), engine DECIDES (validates, executes, wakes on guaranteed events), timed-event inbox + statuses in the state view — with the Nim brain as the sole hero brain, the C++ hero decision layer and the noiser brain path deleted, and a diagram-rich HTML design doc as the canonical contract description.

**Architecture:** Wire v2 is a wholesale replacement of `brain_abi.h`/`abi.nim` (breaking bump to `BL_ABI_VERSION 2`). Engine-side: a durable `CurrentIntention` per hero, a TTL-decremented `EventInbox`, a wake-condition rule, and an `apply_intention` validation/execution seam that reuses the existing command producers — the accepted intention IS the logged command, and the idleness hint rides in the logged `SetBehavior` duration (the existing deliberation-pause pattern) so replay reproduces the wake schedule without thinking. Deletion is staged after the flip so the build stays green per task. Skills are an implementation detail here: `UseSkill` is only a reserved intention kind.

**Tech Stack:** C++23/EnTT, Nim→wasm32-wasi (`scripts/build_brains.sh`, pinned wasi-sdk, LFS-committed artifacts), wasmtime host (`src/crates/brainhost` — unchanged), Catch2.

Spec: `docs/superpowers/specs/2026-07-24-intention-contract-design.md` (commit 4af3f6d, branch `feat/intention-contract`).

## Global Constraints

- Brain SUGGESTS, engine DECIDES: invalid suggestions are ignored with `spdlog::warn` (never fatal, never executed); wasm *mechanical* failures (load/tick/wire-shape) stay FATAL per the standing policy.
- Guaranteed wakes (v1, append-only vocabulary): `DamageTaken`, `ThreatSighted`, `MoveBlocked`, `IntentionEnded`; plus idle-hint expiry and having-no-intention. Spurious wakeups are contractual — the brain must re-decide idempotently.
- Determinism contract holds unchanged: `state = f(initial config, seed, command log, N ticks)`; replay never calls the brain; hints/pauses reach the log via existing command kinds (`SetBehavior` duration) — NO new CommandKind this slice.
- Wire structs: int64 fields first, explicit `_pad`, sizeof/offsetof static-asserted on both sides; `abi.nim` hand-mirrored; append-only id vocabularies (`BL_INT_*`, `BL_EV_*`, `BL_ST_*`).
- NO second C++ brain: after this slice no C++ code scores or chooses hero activities. Heroes without wasm IDLE (host combat execution still defends them). Critter/townfolk/monster C++ brains stay.
- Noiser BRAIN path deleted; noiser itself stays for mapgen/texgen (`noiser-bundle` untouched).
- The HTML design doc is self-contained (inline SVG only — the repo doc must render offline; no CDN scripts, no mermaid runtime) and frames skills as a future intention kind only.
- Commits: `feat(game)`/`refactor(game)`/`docs` style + trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Canonical test runner: `ctest --test-dir build` (env vars; see memory). `scripts/build_brains.sh` rebuilds wasm artifacts; stage LFS paths deliberately.

## Context

The user's goals (recorded in the spec): the brain-world boundary becomes a formal, replaceable interface (LLM/NN-implementable later) — sparse wakeups, deferred results, engine-authoritative. Key existing code: think dispatch `game/src/sim.cpp:240-281` (combat pre-empt at :56-77 stays as host execution); wasm host `game/src/wasm_brain.{h,cpp}` (367 lines, pack/decode/apply — rewritten); `town_brain.{h,cpp}` (299 lines: `town_think`/`hero_activities` DELETED; `observe_hero`/`weights_for` KEPT — perception is state, not decisions; `BrainDecision`/`apply_brain_decision` replaced by `Intention`/`apply_intention`); behaviours layer is SHARED — deer/townfolk keep `selectors.{h,cpp}` and the flee/graze/roam/idle/visit_taxable/deposit blocks; only hero-only blocks (go_home, buy, visit_tavern, chat, hunt, explore) + `deliberation.{h,cpp}` die; noiser path `brain.{h,cpp}` (358 lines) + `Brain::state` + `BrainDesc.noiser_source` + `SimStats.script_intents/noiser_bugs` + `Sim::ReloadScript` + the legacy Intent movement loop (`sim.cpp:283-299`) + `scripts/brains/*.noiser` die. Nim side is modular (`abi/blocks/selectors/deliberation/hero_view/rng/hero.nim`, 2256 lines total) — scoring blocks are REUSED; the scaffold/tick loop is rewritten to react-on-wake. `MoveBlocked` component + `BlViewSuggest.move_blocked` already model refusal; the inbox generalizes it.

---

### Task 1: The HTML design doc (canonical contract description)

**Files:**
- Create: `docs/design/intention-contract.html`

**Interfaces:** none consumed/produced in code; the doc's diagrams define the vocabulary the later tasks implement (names must match: `BL_INT_*`, `BL_EV_*`, `BL_ST_*`, `CurrentIntention`, `EventInbox`, `apply_intention`).

- [ ] **Step 1: Author the page.** Self-contained HTML (inline CSS, inline SVG — NO external scripts/fonts; must render from `file://`). Sections, each with a hand-authored SVG diagram plus short prose:
  1. **The loop** — brain⇄engine cycle: `state + events → react() → suggestion → validate → execute → world mutates → state + events` with "results deferred by design" annotated on the return edge.
  2. **Wakeup state machine** — states: `Sleeping (intention running / idle hint)` → wake edges: guaranteed events (DamageTaken, ThreatSighted, MoveBlocked, IntentionEnded), hint expiry, no-intention; note "spurious wakeups allowed; bypass statuses skip think".
  3. **Wire layout** — the v2 view (self / statuses / suggest / factors / events / chars) and suggestion (intention + activity label + idle hint) as labeled byte-block diagrams (field order: int64 first, explicit pads).
  4. **Event-inbox lifecycle** — write → sticky (TTL decrements per tick) → read on any wake → expiry; one timeline SVG.
  5. **Vocabularies** — tables for `BL_INT_*` (MoveTo, Attack, Shoot, Enter, EnterHome, Buy, Chat, Idle; UseSkill reserved), `BL_EV_*`, `BL_ST_*` (Chatting, MeleeLocked, InsideBuilding), each marked append-only. Skills appear ONLY as the reserved kind + one line ("an implementation detail of a future slice").
  6. **Determinism** — how hints ride logged commands; replay never thinks.
- [ ] **Step 2: Verify** it renders: `ls -la docs/design/` + open-check is manual; structurally verify no external refs: `grep -n "http\|cdn\|<script src" docs/design/intention-contract.html` → no hits.
- [ ] **Step 3: Commit** — `docs: intention-contract design doc (HTML, canonical)`.

---

### Task 2: Engine groundwork — inbox, statuses, current intention, wake rule (inert)

**Files:**
- Modify: `game/src/components.h` (new components), `game/src/game_state.h` (helpers decl if needed)
- Create: `game/src/intention.h`, `game/src/intention.cpp`
- Modify: `game/src/sim.cpp` (tick wiring: inbox TTL decrement; DamageTaken writer), `game/src/movement.cpp` (MoveBlocked → inbox mirror), `game/src/behaviours/perception.cpp` or the threat-collection site (ThreatSighted writer — first sighting edge, not per-tick)
- Modify: `CMakeLists.txt` (add `game/src/intention.cpp` to `badlands_game_lib`; add `game/tests/intention_tests.cpp`)
- Test: `game/tests/intention_tests.cpp`

**Interfaces (produces — the flip and deletion tasks consume these exactly):**

```cpp
// components.h — native mirrors of the wire vocabularies (append-only).
enum class InboxEventKind : int32_t { None = 0, DamageTaken, ThreatSighted, MoveBlocked, IntentionEnded };
struct InboxEvent {
    int64_t at_millis = 0;
    int64_t ttl_millis = 0;      // decremented per tick; <= 0 -> dropped
    InboxEventKind kind = InboxEventKind::None;
    uint32_t source_slot = UINT32_MAX;
    float param = 0.0f;          // kind-specific (damage amount, distance, 1/0 completed)
};
inline constexpr int32_t kInboxCapacity = 8;
struct EventInbox {              // newest-wins ring; heroes only
    InboxEvent events[kInboxCapacity]{};
    int32_t count = 0;
};
enum class IntentionKind : int32_t { None = 0, MoveTo, Attack, Shoot, Enter, EnterHome, Buy, Chat, Idle };  // UseSkill reserved next slice
struct CurrentIntention {        // what the engine is executing for this hero
    IntentionKind kind = IntentionKind::None;
    glm::vec2 point{0.0f, 0.0f};
    uint32_t target_slot = UINT32_MAX;
    int32_t arg = 0;
    int64_t started_at_millis = 0;
    int64_t wake_at_millis = 0;  // idle-hint / Idle-duration deadline (0 = none)
};
```

```cpp
// intention.h — the engine side of the contract (validation + execution + wakes).
struct Intention {               // host-native decoded suggestion
    IntentionKind kind = IntentionKind::None;
    glm::vec2 point{0.0f, 0.0f};
    uint32_t target_slot = UINT32_MAX;
    int32_t arg = 0;
    int64_t duration_millis = 0;   // Idle
    int32_t activity_label = -1;   // ActivityId, inspection only
    int64_t idle_hint_millis = 0;
};
// Validates + adopts a suggestion: feasibility-checks it (known target, enterable
// building, sane point), maps it onto the existing command producers
// (enqueue_move_to / enqueue_set_behavior / command_queue), stamps
// CurrentIntention, and logs the wake schedule via the SetBehavior duration.
// Invalid -> spdlog::warn + no execution + CurrentIntention untouched.
// Returns whether the suggestion was adopted.
bool apply_intention(BadlandsGame& game, uint32_t slot, const Intention& intent);
// Push into a hero's inbox (newest evicts oldest when full). ttl from factors.
void push_inbox_event(BadlandsGame& game, entt::entity e, InboxEvent ev);
// Per-tick maintenance: decrement TTLs, drop expired, detect intention
// completion/abort (arrival, dead target, gone building) -> IntentionEnded.
void advance_intentions(BadlandsGame& game);
// The wake rule (pure over the components): true when the hero has no active
// intention, an inbox event arrived since last think, or wake_at passed.
bool should_wake(const BadlandsGame& game, entt::entity e);
```

- Inbox TTL default: new `ProgressionFactors`-style scalar is NOT added; use a compile-time constant `kInboxTtlMillis = 3000` in components.h (constants until a knob is asked for — CLAUDE.md).
- Writers wired this task (inert consumers): DamageTaken from the `emit_char_hit` seam (both damage sites route through it), MoveBlocked mirrored where `MoveBlocked` component is written (movement.cpp), ThreatSighted on the empty→nonempty edge of the hero's perceived threat list, IntentionEnded from `advance_intentions`.
- Heroes get `EventInbox` + `CurrentIntention` at spawn (heroes.cpp hero branch).

**TDD:** intention_tests.cpp — inbox push/evict/TTL-expiry; DamageTaken lands in the victim's inbox after a hit (flat-world duel via direct spawns); MoveBlocked mirror; should_wake truth table (no intention / new event / deadline); apply_intention: valid MoveTo adopts + logs MoveTo command + SetBehavior carries the hint; invalid target warns + adopts nothing; Idle sets wake_at. Then commit — `feat(game): intention-contract engine groundwork (inbox, wake rule, apply seam)`.

---

### Task 3: The contract flip — wire v2 + host rewrite + Nim rewrite + artifacts

Single task, single commit (the ABI bump is atomic: wire, host, guest, and their tests cannot be split green). Steps inside are still bite-sized.

**Files:**
- Rewrite: `game/src/brain_abi.h` (v2), `scripts/brains/nim/abi.nim` (mirror)
- Rewrite: `game/src/wasm_brain.{h,cpp}` (think-on-wake; pack v2 view; decode suggestion → `Intention`; drop `BrainDecision` dependency)
- Rewrite: `scripts/brains/nim/hero.nim` + `brain_scaffold.nim` (react-on-wake); reuse `blocks.nim`/`selectors.nim`/`hero_view.nim`/`rng.nim` scoring; delete `deliberation.nim` (the pause IS the idle hint now)
- Modify: `game/src/sim.cpp` think loop (wasm heroes: `should_wake` gates `tick_wasm_brain`; combat pre-empt unchanged as host execution)
- Rebuild: `assets/brains/*.wasm` via `scripts/build_brains.sh` (LFS — stage deliberately); the `idle_test.nim`/`trap_test.nim` fixtures updated to v2
- Rewrite tests: `game/tests/brain_abi_tests.cpp`, `game/tests/wasm_brain_tests.cpp`
- Delete: `game/tests/hero_brain_parity_tests.cpp` (its subject — decision parity — dies here; the C++ side stops being consulted for wasm worlds this task)

**Wire v2 (exact shapes — the doc's diagrams, the header, and abi.nim must agree):**

```c
#define BL_ABI_VERSION 2
#define BL_MAX_THREATS 8
#define BL_MAX_CHARS 16
#define BL_MAX_EVENTS 8
#define BL_MAX_STATUSES 8
#define BL_MAX_ACTIVITIES 14
// Intention kinds (append-only): 0 none/no-change.
#define BL_INT_NONE 0
#define BL_INT_MOVE_TO 1
#define BL_INT_ATTACK 2
#define BL_INT_SHOOT 3
#define BL_INT_ENTER 4
#define BL_INT_ENTER_HOME 5
#define BL_INT_BUY 6
#define BL_INT_CHAT 7
#define BL_INT_IDLE 8
#define BL_INT_USE_SKILL 9   /* reserved: rejected by the host until the skills slice */
// Event kinds (append-only), mirroring InboxEventKind.
#define BL_EV_NONE 0
#define BL_EV_DAMAGE_TAKEN 1
#define BL_EV_THREAT_SIGHTED 2
#define BL_EV_MOVE_BLOCKED 3
#define BL_EV_INTENTION_ENDED 4
// Status kinds (append-only).
#define BL_ST_NONE 0
#define BL_ST_CHATTING 1
#define BL_ST_MELEE_LOCKED 2
#define BL_ST_INSIDE_BUILDING 3

typedef struct BlEvent {
    int64_t at_millis;
    int64_t ttl_millis;
    uint32_t kind;
    uint32_t source_slot;
    float param;
    uint32_t _pad;
} BlEvent;                       /* 32 bytes */

typedef struct BlStatus {
    int64_t remaining_millis;    /* 0 = indefinite */
    uint32_t kind;
    uint32_t _pad;
} BlStatus;                      /* 16 bytes */
```

`BlViewSelf` keeps its v1 fields and appends the current-intention summary (`int32_t intention_kind; uint32_t _pad2; int64_t intention_wake_at;` — int64 grouped first per layout rules; reorder accordingly and re-assert). `BlViewSuggest`, `BlViewFactors` (minus the two think_* fields — deliberation is gone; hint replaces it), `BlViewChar` carry over. New top-level:

```c
typedef struct BlViewWire {
    uint32_t version;            /* == 2 */
    uint32_t _pad;
    BlViewSelf self;
    BlViewSuggest suggest;
    BlViewFactors factors;
    int32_t status_count;
    uint32_t _pad2;
    BlStatus statuses[BL_MAX_STATUSES];
    int32_t event_count;
    uint32_t _pad3;
    BlEvent events[BL_MAX_EVENTS];
    int32_t char_count;
    uint32_t _pad4;
    BlViewChar chars[BL_MAX_CHARS];
} BlViewWire;

typedef struct BlSuggestionWire {
    int64_t idle_hint_millis;    /* 0 = none */
    int64_t duration_millis;     /* BL_INT_IDLE only */
    int32_t intention_kind;      /* BL_INT_* */
    int32_t activity_label;      /* ActivityId, inspection only */
    float point_x, point_z;
    uint32_t target_slot;
    int32_t arg;                 /* building kind for ENTER, etc. */
} BlSuggestionWire;              /* 40 bytes */
```

Compute exact sizeofs during implementation and static-assert them on both sides (the plan deliberately does not guess the totals; the layout RULES are the contract).

**Host (`wasm_brain.cpp`) flow per hero per tick:** combat pre-empt first (unchanged, host execution); then `if (should_wake(...))`: build `WorldView` (`observe_hero` — kept), pack v2 view (statuses assembled from the existing components; inbox copied), `bh_tick`, read `BlSuggestionWire`, decode into `Intention` (trust boundary: reject non-finite/out-of-range → FATAL as today for malformed wires; a well-formed but infeasible intention is the WARN+ignore path in `apply_intention`), `apply_intention`. Mark the think (clear "unseen inbox" bookkeeping).

**Nim (`hero.nim`) react-on-wake:** score activities with the existing `blocks.nim`/`selectors.nim` over the packed view (threat-aware: threats in view → suggest Attack/flee-style MoveTo; else needs scoring as today), emit ONE intention + activity label + idle hint (hint = the old deliberation draw via `rng.nim`, bounded by a compiled constant since `think_*` factors left the wire; events inspected: MoveBlocked → avoid re-suggesting the blocked point this wake; DamageTaken/ThreatSighted → prefer danger-band response). Handle spurious wakeups (pure re-decision).

**TDD:** brain_abi_tests (sizes/offsets both sides), wasm_brain_tests (pack: statuses/events land in the wire; decode: each BL_INT_* maps; BL_INT_USE_SKILL warns+ignores; hint plumbs to wake_at), an integration case: wasm town world runs N ticks, heroes act (log has MoveTo/SetBehavior), sleeping hero (long hint) is woken by damage (inject a hit; brain re-decides within a tick). Rebuild artifacts, run full ctest. Commit — `feat(game): wire v2 — the intention contract (think-on-wake, Nim sole hero brain)`.

---

### Task 4: Delete the C++ hero decision layer + migrate its tests

**Files:**
- Delete from `game/src/town_brain.{h,cpp}`: `town_think`, `hero_activities`, `BrainDecision`, `apply_brain_decision` (keep + rename the file's remainder — `observe_hero`/`weights_for` — into `game/src/hero_perception.{h,cpp}`; fix includers)
- Delete: `game/src/behaviours/deliberation.{h,cpp}`; hero-only blocks (`score_/act_` for go_home, buy, visit_tavern, chat, hunt, explore) from `behaviours/blocks.{h,cpp}` (flee/graze/roam/idle/visit_taxable/deposit stay — deer/townfolk use them)
- Modify: `game/src/sim.cpp` dispatch — `BrainKind::Town` without wasm: no think at all (idle); with wasm: Task 3's path
- Tests: per the migration map below
- Modify: `CMakeLists.txt` (remove deleted TUs/tests; add renamed perception TU)

**Test migration map** (from the full-suite survey; verify each against its file when executing). Shared fixture first: add `game/tests/fixtures/wasm_hero.h` with `std::vector<uint8_t> load_hero_wasm()` (reads `assets/brains/hero.wasm` from the repo-root cwd, `REQUIRE`s non-empty) and `make_wasm_world()` returning `make_world(BrainDesc{.wasm_bytes=..., .wasm_len=...})` — every migrated case uses it.

- **DELETE whole file:** `behaviours_tests.cpp` (tests `hero_activities()`/hero scoring blocks directly); `deliberation_tests.cpp` (tests `deliberate()`/Think pause — the hint replaces it; its threat-proximity API subset is shared WorldView infra already covered by critter tests, accepted); `hero_brain_parity_tests.cpp` (twin premise gone — SALVAGE its two tail wasm-only smokes (GoHome, Buy) into the new `hero_behavior_tests.cpp` first).
- **SPLIT files** (delete block-level cases, keep mechanics, migrate end-to-end decision cases to `make_wasm_world()`):
  - `chat_tests.cpp`: delete the score_chat/act_chat block cases; KEEP session/need mechanics driven via direct `apply_command`/`advance_needs`; migrate "two heroes find each other and talk" to the wasm world.
  - `exploration_tests.cpp`: KEEP `pick_exploration_target` picker + walkability/blocked cases (brain-free); delete the `score_explore` block case; migrate "a hunter actually sets off" to wasm.
  - `hunter_tests.cpp`: delete `score_hunt`/`act_hunt` block case; KEEP "recruiting yields a Hunter"; migrate "runs down a deer" + "hunts then rests" to wasm.
  - `needs_tests.cpp`: KEEP drain/refill/leave-when-full mechanics; migrate the 3 end-to-end GoHome/VisitTavern decision cases to wasm.
  - `archetype_tests.cpp`: KEEP spawn-recipe cases; migrate "recruited heroes still run the town loop" to wasm.
  - `determinism_tests.cpp`: KEEP run-twice/replay as-is (decision-agnostic); migrate "a run with fog of war and explorers replays exactly" (needs a real Explore decision) to wasm — this becomes the contract's replay gate.
  - `activity_stats_tests.cpp`: easy — histogram totals reconcile regardless of choice; flip its world to wasm so the counts stay meaningful, assertions unchanged.
- **KEEP unchanged (survey-verified no hero-decision dependency):** everything else — combat, movement, command, heroes (recruitment/residency "no UI, no brain"), events (combat_preempt-driven duel pattern), vision/fog, nav/navmesh, placement, progression, skills, clock, critter/townfolk/rat (their brains stay), entity_memory, facing, rendering/GPU suites, game_ui, imgui_input, sim_tests (mock combat mechanics), factors_sanitize (already wasm-targeting where it matters).
- **Note:** `brain_abi_tests.cpp`/`wasm_brain_tests.cpp` were classified keep-by-subject but are REWRITTEN in Task 3 (the wire flip is their subject).

**TDD shape:** first land the new wasm-driven behavior tests green (they exist from Task 3's integration case + a new `hero_behavior_tests.cpp` asserting histogram-in-spirit: over a few in-game hours hunters accrue Hunt ticks, night raises GoHome share, a spawned rat flips nearby heroes out of Idle), then delete the layer + its unit tests, then fix every remaining red by the map. Commit — `refactor(game): delete the C++ hero decision layer (Nim is the only hero brain)`.

---

### Task 5: Delete the noiser brain path

**Files:**
- Delete: `game/src/brain.{h,cpp}` — EXCEPT `BrainKind` + the `Brain` component, which move to a slim `game/src/brain_kind.h` (`Brain` loses its `state` pointer; `.kind` only); fix includers (heroes.cpp spawn, sim.cpp dispatch, archetype recipes)
- Modify: `game/include/badlands_sim.hpp` — `BrainDesc` drops `noiser_source` (wasm_bytes/len only); `SimStats` drops `script_intents`/`noiser_bugs`; `Sim::ReloadScript` removed
- Modify: `game/src/sim.cpp` — drop `game.brains`, `reload_script`, `report_bug`'s brain-downgrade semantics (keep a plain warn if still referenced), the legacy Intent movement loop (`tick_world`'s kind-1 block), `noiser_bugs`/`script_intents` fields on `BadlandsGame`
- Delete: `scripts/brains/hero.noiser`, `scripts/brains/combat_test.noiser` (+ any siblings), `game/tests/duel_test.cpp`, `game/tests/noiser_smoke_tests.cpp`, the `BADLANDS_COMBAT_SCRIPT`/`BADLANDS_BRAIN_SCRIPT` env plumbing in CMake `add_test`
- Modify: `game/src/noiser_jit_stubs.cpp` — keep only if mapgen/texgen link needs it from this lib (verify with the build); `crates/noiser-bundle` and all mapgen/texgen noiser use UNTOUCHED
- Modify: CLAUDE.md repo-state paragraph (brain description) + `.superpowers` ledger note; `README` if it names the noiser brain path
- Tests: delete `duel_test.cpp` + `noiser_smoke_tests.cpp` outright (pure noiser plumbing per the survey). Exactly three surviving files reference the removed counters — `sim_tests.cpp` (asserts both `== 0` in the mock duel), `wasm_brain_tests.cpp` (reads them repeatedly), `factors_sanitize_tests.cpp` (reads `noiser_bugs` in the wasm Think-pause repro) — drop those assertions/reads (the wasm-decision-delivered signal, if a test needs one, becomes "the command log grew"). Also fix any `Intent`-driven fixtures (grep `intent_move`/`Intent{` in tests).

**TDD shape:** deletion-driven — full ctest red list after each removal step is the worklist; end green. Verify `badlands_mapview --preview-image-only` still works (noiser mapgen intact). Commit — `refactor(game): delete the noiser brain path (wasm-only brains)`.

---

### Task 6: Final gates + docs

- [ ] Full `ctest --test-dir build` green; `cmake --build build` warning-clean for changed code.
- [ ] Headless smokes: `./build/badlands_game --screenshot out.png`; 30s alarm-bounded `badlands_ai_sandbox` run — heroes visibly act (inspector activities change; not all Idle).
- [ ] Determinism: run-twice + replay suites green under wasm heroes (they now COVER the contract: wake schedule from logged hints).
- [ ] `docs/design/intention-contract.html` untouched by implementation drift? Re-read; fix names if any diverged.
- [ ] Update CLAUDE.md's "Repository state" bullet (hero brain = intention contract; no mock/noiser brains) — one commit `docs: repo state after the intention contract`.
- [ ] Copy this plan to `docs/superpowers/plans/2026-07-24-intention-contract.md` and commit (plan mode barred writing it there during planning).

## Verification (end of plan)

1. `ctest --test-dir build` — all green (target count will drop with deleted suites; record the new number in the ledger).
2. `scripts/build_brains.sh` idempotent (re-run produces identical artifacts; `git status` clean).
3. Behavior-in-spirit: `hero_behavior_tests.cpp` assertions (hunt share, night rest, threat interrupt) + a manual ai_sandbox look.
4. Replay determinism is the load-bearing gate: the wake schedule must reproduce from the log alone.

## Out of scope

Skills through the contract (`BL_INT_USE_SKILL` execution, Calcify, SkillUsed events, cooldown wakes), bypass statuses with real entries (Incapacitated), LLM/NN brain implementations, retiring `hero_view.nim` niceties not needed by the rewrite.
