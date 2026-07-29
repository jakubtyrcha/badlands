# Mapgen: agent-based canal pre-carve

Cuts a drainage skeleton across the plains with Physarum-style agents *before*
the erosion sim runs, so the hydrology has real gradients to follow instead of
having to invent them.

## Why

Three separate problems this session all trace to one cause.

- **Flats route by flood order, not gradient.** 33–45% of channel texels
  (seeds 1–3) take their receiver from the priority-flood wavefront, because on
  level ground every cell is flagged `in_lake` and excluded from steepest
  descent.
- **Lakes have no outlet.** Basins fill to the brim, so no river resumes below
  them.
- **The outlet notch had nowhere to drain.** Measured: 100% of notch channels
  hit the step cap without ever reaching lower ground.

The common cause: **the plains carry `kPlainsReliefM` = 2 m of relief across
the entire map and contain no incised drainage.** Water arriving from the
mountains has no channel to follow and no gradient worth following, so the
router falls back on ε bookkeeping and lakes fill to their rims.

Every fix attempted so far treated a symptom. This one gives the plains a
drainage skeleton, which is the thing they are missing.

## Decisions taken with the user

- Canals are **guaranteed monotonically descending** — drainage is assured by
  construction rather than by luck.
- Agents are absorbed by a **lake or the map edge**; hitting an existing trail
  **merges discharge and the agent continues along the combined path**.
- Seeding is **threshold-driven**: every qualifying highland-edge cell seeds an
  agent, however many that is.

## Placement in the pipeline

```
bedrock -> cone -> plains relief -> carve_cavities
  -> CANAL PRE-CARVE          <-- new, cuts B directly
  -> init_sediment -> micro_fill -> erosion loop -> finalize
```

It runs after cavities so basins already exist as attractors and terminals, and
before sediment so it cuts bedrock with no layer bookkeeping to respect.

## Part 1 — Seeding

Route the current `B` once and take drainage area `A`.

A cell seeds an agent when it is a **highland edge** carrying enough flow:

- `distance_to_plains == 0` (it is Plains) and some 8-neighbour has
  `distance_to_plains > 0` (the Hills boundary), and
- `A ≥ canal_seed_area_m2`.

This encodes the user's observation directly: water organises itself in the
mountains through natural cavities, and the place a channel needs help is where
it debouches onto the plain.

Seeds are processed in **descending drainage area, ties by linear index**.
Order matters because agents interact through trails, so it must be defined.
Each agent starts with `discharge = runoff_m_per_s × A`, the same conversion
the river graph uses.

## Part 2 — The automaton

### State

```cpp
struct CanalAgent {
  glm::vec2 pos;          // world metres
  glm::vec2 dir;          // unit heading
  float discharge_m3_s;
  float ref_level_m;      // the descending reference (Part 3); only ever falls
  int32_t source;         // union-find id; merges alias it
  std::vector<int32_t> visited;  // cells this agent has entered
  bool on_trail;          // travelling along an existing channel
};
```

Each trail cell records `{source, discharge, dir}`.

### The step, in order

1. **If on a trail**, take the trail's stored direction and skip to 6. Steering
   is off while on a trail: after merging, every cell ahead is same-source, and
   same-source repulsion would shove the agent straight back off the trunk it
   just joined.
2. **Build the candidate set**: the 8-neighbours whose bearing is within
   `max_turn_angle` of `dir`. A discrete scored list, not a rotation toward one
   sensor — that is what makes the trade-off explicit and weightable. It also
   makes a reversal impossible, since 180° is never within the limit.
3. **Veto** any candidate that is in `visited` (Rule 1) or whose height exceeds
   `ref_level + max_climb_m` (Rule 2). If none survives, terminate.
4. **Score** every survivor (all terms in METRES, see below).
5. **Pick** the highest score; ties break on lowest linear index. With
   probability `wander_chance`, pick a uniformly random survivor instead — the
   only source of meander.
6. **Move** one cell. Cut it per Part 3. Record the trail. Update `dir`.
7. **Merge** if the cell already carries a trail whose union-find root differs
   from this agent's: union the roots, add discharge, adopt the trail's stored
   direction, set `on_trail`.

### Scoring

Every term is in metres, which is the whole point — a dimensionless weight
against a height difference is untunable by meaning, and this is the balance
the design turns on.

```
ref_next     = ref_level - canal_slope * step_len      // what step 6 will cut to
drop(c)      = height(pos) - height(c)                 // + is downhill
excavate(c)  = max(0, height(c) - ref_next)            // METRES OF ROCK REMOVED
water_at(c)  = sample at pos + dir(c) * sense_distance_m
water_pull(c)= 0 if no water there, else
               same_root(c) ? -w_water
                            : +w_water * clamp(water_drop(c)/falloff_m, -1, +1)

score(c) = w_flow  * drop(c)
         - w_dig   * excavate(c)
         + water_pull(c)
         - w_turn  * |bearing(c) - dir|                // metres per radian
```

- **`w_dig` is the term the design was missing.** Without it an agent turns
  toward attractive water regardless of the trench needed to get there. Costing
  the excavation directly means a turn has to be *worth* the rock it removes,
  so canals prefer to follow ground that is already low and only cut when the
  pull genuinely justifies it. Set `w_dig > w_flow`: a metre dug should hurt
  more than a metre descended helps.
- **`w_turn` is momentum.** Rivers do not zigzag; without it the wander term
  and a noisy surface produce a jittery path rather than a winding one.
- **`water_pull` is signed and source-aware.** Water below attracts, water
  above repels — a flat bonus would steer toward a channel *higher* than the
  agent, and Part 3 would then trench through rising ground to reach it.
  Different source attracts (a real confluence); same source repels (the
  network folding back on itself).

### Source aliasing is a disjoint-set union, and the ids go STALE

Trail cells are **never rewritten on merge**. Rewriting a whole network's cells
on every union would be O(n) per merge, so each cell keeps the id it was laid
with and that id goes stale the moment its network merges into another.

**Therefore every same-source test must resolve through `find()`, never compare
raw ids.** This is the one place the design can silently regress into exactly
the braiding it exists to prevent:

```
same_source(cell, agent)  =  find(cell.source) == find(agent.source)   // correct
                          !=  cell.source == agent.source              // WRONG, and passes naive tests
```

A raw comparison looks right on cells written *after* a merge, because those
carry the merged id already. It fails only on cells laid *before* the merge —
which are most of the network. Those would read as a different source, so the
combined flow would be **attracted back into its own trunk** and braid.

Union by **smaller root**, path-compressed. Smaller-root (rather than by rank
or size) makes the resulting root independent of merge order, so the assignment
is reproducible without depending on the seed processing order for correctness.

### Hard rules

**Rule 1 — strict self-avoidance.** An agent may never re-enter a cell it has
entered. This is the loop guarantee, and Part 3 shows it is what converts the
descent guarantee from a statement about time into one about space.

**Rule 2 — no tunnelling.** A candidate more than `max_climb_m` above
`ref_level` is vetoed. Note this is a veto against driving through a *mountain*,
not against climbing at all: stepping onto higher ground is allowed and the
carve makes it downhill. A hard no-climb rule would fight "turn toward water"
whenever the water sits up-slope; `w_dig` handles the ordinary case by price
rather than prohibition.

**Merging is across roots only.** Adding discharge to same-source water would
create water from nothing and double again each lap. Rule 1 makes an agent's
own cells unreachable and the root check covers the wider case; the merge path
asserts both rather than trusting them.

### Termination

Off-map (dies), absorbed by a lake, boxed in by Rules 1/2, or `max_steps`.
The cap is a pure backstop — Rule 1 already makes non-termination impossible,
since the visited set only grows over a finite grid. Report a histogram of
terminal reasons; it is the main diagnostic when tuning.

## What can go wrong

Each failure, the rule that handles it, and how it would be caught if the rule
does not hold. The last column matters: several "guarantees" earlier in this
work turned out not to hold on real terrain, and were only found by measuring.

| Failure | Handled by | Detected by |
|---|---|---|
| Agent loops forever | Rule 1 — visited set only grows over a finite grid | `hit_step_cap > 0` is a bug, not slowness |
| Channel profile jumps uphill at a loop closure | Rule 1: each cell entered once + falling `ref_level` ⇒ monotone in space | Assert the carved profile is non-increasing at every step of every agent |
| Agent adds discharge to its own trail, creating water | Merge requires differing union-find roots | Assert on merge; total outflow ≤ total seeded discharge |
| Two branches of one network attract and braid | Same-root repulsion | Count merges whose roots were already equal — must be zero |
| **Stale source ids compared raw instead of through `find()`** | Every same-source test resolves roots first | Dedicated aliasing test below — a raw comparison passes every other test in the suite |
| Agent trenches through a hill to reach water | `w_dig` prices the excavation; `max_climb_m` vetoes the extreme | Histogram of per-step excavation; a long tail means `w_dig` is too low |
| Agent steered toward water that is *above* it | `water_pull` signed by relative level | Fixture: trail placed above must repel |
| Agent shoved off a trunk it just joined | Steering off while `on_trail` | Fixture: A merges into B, then follows B to B's terminal |
| Path zigzags instead of winding | `w_turn` momentum + `max_turn_angle` | Mean absolute turn per step; near the limit means `w_turn` is too low |
| Canals are dead straight | `wander_chance` | Sinuosity ~1.0 across all canals |
| Long meander becomes a canyon | incision = `canal_slope × path length` | Max carve depth over all agents; tune against the LONGEST canal, not the mean |
| Agents stranded a few steps in | Rules 1/2 terminate rather than backtrack | Terminal-reason histogram; a large boxed-in share means the sense geometry is wrong, not the rules |
| Agent count explodes | Threshold-driven by design | Log the count; thousands means the threshold is wrong |
| Two agents cut one cell at different refs | `min` keeps the deeper cut; sorted seed order makes it deterministic | Same-seed byte-identical `B` |
| `micro_fill` backfills a canal mouth | Canals are monotone so contain no closed depression | Compare wet-cell counts across `micro_fill`; a canal mouth must not fill |
| Non-determinism from agent interaction | Per-agent RNG keyed on (map seed, seed cell); sorted seeds; union by smaller root; ties on lowest index | Same-seed byte-identical `B` and trail field |

## Part 3 — Carving, with guaranteed descent

Each agent carries a reference level that only ever falls:

```
ref_level -= canal_slope * step_length      // every step, unconditionally
B(cell)    = min(B(cell), ref_level)        // cut only where terrain is higher
```

**Monotone `ref_level` alone is a guarantee in TIME, not in SPACE**, and only
the spatial one is worth anything here. If an agent could revisit a cell, it
would arrive at a lower `ref_level` and cut deeper — so the channel profile
would descend, then jump UP at the loop closure. That is water flowing uphill,
which is exactly what must not happen.

Rule 1 closes the gap. **Each cell is entered at most once, and `ref_level`
falls every step, so the profile along the channel is monotone decreasing by
construction.** A canal cannot contain a pit and cannot contain an uphill step,
so any agent reaching the edge or a lake leaves behind a path water can follow
the whole way. That is the property the whole change exists for, and it is
structural rather than emergent — but it rests on self-avoidance, not on the
descent rule by itself.

The descent must stay **gradual**. `canal_slope` is what lets a long winding
river stay a shallow channel instead of deepening into a canyon: incision is
`canal_slope × path length`, so a meandering path 3x longer than the direct
line cuts 3x deeper for the same endpoints. Tune against the longest canals,
not the average.

Where the terrain already descends faster than `canal_slope`, `min` leaves it
untouched — the canal only cuts where the ground is flat or rising, which is
exactly where the plains need help.

Channel **width** comes from `channel_hydraulics(Q, canal_slope).width_m`,
floored at one texel. At production discharges that floor will almost always
bind (a 512 m map's largest outlet is ~0.0026 m³/s, giving a 0.25 m channel),
so canals will be one texel wide — acceptable here, since the point is to
create a gradient, not a visible feature.

`canal_slope` is the tuning knob that matters: over a 500 m map it sets total
incision. At 0.002 m/m a canal cuts at most ~1 m below its seed across the
whole map, comparable to the plains relief it is fixing.

## Interface changes

New module `src/mapgen/canal_carve.{hpp,cpp}`:

```cpp
struct CanalResult {
  Field2D<float> trail_discharge_m3_s;  // 0 off-canal; for debug + later passes
  int agents = 0, absorbed_by_lake = 0, left_map = 0, hit_step_cap = 0;
};

CanalResult carve_canals(Field2D<float>& B, const Field2D<uint8_t>& lake_mask,
                         const Field2D<float>& dist_to_plains,
                         const ErosionParams& p, float texel_m, uint32_t seed);
```

New `ErosionParams` fields: `canal_seed_area_m2`, `canal_slope`,
`canal_sense_distance_texels`, `canal_max_turn_angle_rad`,
`canal_wander_chance`, `canal_max_climb_m`, `canal_water_falloff_m`, and the
four scoring weights `canal_w_flow`, `canal_w_dig`, `canal_w_turn`,
`canal_w_water_m` — all operating on metre-denominated terms (Part 2).

One new debug stage, `"canals"`, dumped between `"cavities-height"` and
`"sediment-init"`.

**Determinism:** each agent's RNG is seeded from `(map seed, seed cell index)`,
so an agent's wander does not depend on how many agents ran before it. Only
trail interaction depends on order, and that order is sorted.

## Testing

Artificial heightmaps, reusing the two builders already in the test suite.

| Fixture | Expectation |
|---|---|
| Flat plain, one seed | the canal profile never rises, and the agent reaches the map edge |
| Flat plain, one seed, a lake ahead | absorbed by the lake, not the edge |
| Two seeds converging | discharge below the junction is the sum; the channel is deeper and wider there than above it |
| Terrain already descending faster than `canal_slope` | `B` is untouched — `min` must not raise or gratuitously cut |
| Same seed twice | identical `B` and identical trail field |
| **Every fixture, every agent** | the carved profile along a canal is monotone decreasing at EVERY step — the spatial guarantee, checked directly rather than inferred from the descent rule |
| **Every fixture, every agent** | no cell appears twice in an agent's path |
| Trail placed ABOVE the agent | the agent steers away from it, and no trench is cut toward it |
| Trail of the agent's OWN source alongside it | repelled, not attracted — no braiding back into itself |
| Trail of a DIFFERENT source alongside it | attracted, and the two merge |
| A merges into B, then B's trunk continues | A follows the trunk and is NOT pushed off it by same-source repulsion |
| **Aliasing, unit level** | `union(A, B)`, then a cell still carrying the literal id `A` must read as same-source to an agent carrying `B`. Constructed so a raw `cell.source == agent.source` comparison FAILS: the ids genuinely differ and only the roots agree |
| **Aliasing, in situ** | A lays a trunk; B merges into it; the combined flow is then routed near a segment of A's trail laid BEFORE the merge. It must be repelled. Under a raw comparison it would be attracted and braid, so this fixture is the one that proves the unit behaviour matters where it is used |
| Transitive aliasing | after A merges into B and C merges into A, all three resolve to one root, so C and B repel rather than attract |
| Root determinism | union by smaller root gives the same root regardless of the order the unions are applied |
| Network is a forest | walking flow directions from any trail cell terminates; no cycle exists anywhere in the network |
| Agent steps onto higher ground | permitted, and the carved cell ends up BELOW the previous one — descent comes from the cut |
| Agent steered into a mountain | it terminates rather than tunnelling; `max_climb_m` is respected |
| Any fixture | `hit_step_cap == 0` — Rule 1 already forbids non-termination, so any hit is a bug |

**The headline production measurements**, all of which are the point of the
change and none of which are currently satisfied:

- channel texels routed by flood parent: **33–45% → under 5%**
- `in_lake` fraction of the sim grid: **16% → materially lower** (fewer closed
  depressions survive once the plains drain)
- seeded lakes possessing an outlet: **→ all of them**

## Risks

- **Braiding.** If water attraction is too weak relative to wander, canals
  split instead of merging and the network stops being dendritic. Measure the
  merge count; if it is near zero, the weights are wrong.
- **Premature termination.** Rules 1 and 2 both terminate rather than
  backtrack, so over-tight parameters could strand agents a few steps in.
  Report the terminal-reason histogram (edge / lake / merged-out / boxed-in /
  climb-blocked); a large boxed-in share means the wander or sense geometry is
  wrong, not that the rules are.
- **Over-incision.** `canal_slope` too steep trenches the plains into a visible
  grid of ditches. Judge from the hillshade, not from the parameter.
- **Seed explosion.** Threshold-only seeding means terrain decides the agent
  count. Log it; if a seed produces thousands, the threshold is wrong rather
  than the design.
- **Interaction with `micro_fill`**, which runs immediately after and raises
  shallow closed depressions. Canals are monotone so they contain none, but
  their *junctions* with untouched terrain might; verify `micro_fill` does not
  backfill a canal mouth.

## Deferred

- Making the notch mechanism live again, or removing it, once canals give lakes
  real outlets — it is currently inert and disabled.
- Widening canals for large discharges, if the world scale ever makes width
  exceed a texel.
