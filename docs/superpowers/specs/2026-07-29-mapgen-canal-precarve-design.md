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

## Part 2 — The agent

```cpp
struct CanalAgent {
  glm::vec2 pos;        // world metres
  glm::vec2 dir;        // unit heading
  float discharge_m3_s;
  float ref_level_m;    // the descending reference — see Part 3
};
```

**Sense.** Three samples at `sense_distance_m`, at `-sense_angle`, `0`,
`+sense_angle` from the heading.

**Both terms are in METRES, and that is how the balance is set.** A
dimensionless weight against a height difference would be opaque to tune; in
metres the weight reads directly as *how much drop a nearby channel is worth*.

```
drop(pt)      = agent_level - height(pt)                  // + is downhill
water_pull(pt)= 0 if no water, else
                same_source(pt) ? -water_weight_m
                                : water_weight_m * clamp(water_drop(pt) / falloff_m, -1, +1)
attract(pt)   = drop(pt) + water_pull(pt)                 // metres, comparable
```

So `water_weight_m = 3` means a lower channel of a different source pulls as
hard as three metres of extra descent, and `water_drop` scaling keeps that pull
signed: **water below attracts, water above repels.** A flat bonus would steer
an agent toward a channel sitting *higher* than it, and Part 3's carve would
then dutifully trench through rising ground to reach it.

**Source ownership, with merge aliases.** Every trail cell records a source id.
Ids are held in a union-find; a merge unions the two, so an agent and
everything upstream of it share one root. Then:

- **Different source attracts** — a genuine confluence, two catchments meeting.
- **Same source repels** — that is not a confluence, it is the agent's own
  network folding back on itself.

This generalises self-avoidance, and catches a case bare self-avoidance misses:
two branches of one network running parallel would otherwise attract each other
and braid. Union by smaller root, path-compressed, so it stays deterministic.

**Steering is OFF while on a trail.** Once an agent has merged it is travelling
along same-source cells, and same-source repulsion would immediately shove it
back off the trunk it just joined. On a trail the agent follows the stored
direction; sensors and attractants apply only off-trail. You have joined the
river, you go where it goes.

**Steer.** Rotate `dir` toward the strongest sensor by `turn_angle`. With
probability `wander_chance`, apply an additional random turn — this is the only
source of meander, and it is what stops canals being straight lines.

**Different-source attraction is what makes the network dendritic.** A drainage
network must converge — tributaries merge going downstream and never split.
Without it, Physarum steering braids and loops, which is wrong for water.

**Move** one texel along `dir`, subject to two hard rules.

**Rule 1 — strict self-avoidance. An agent may never re-enter a cell it has
already carved.** This is the loop guarantee, and it is load-bearing for far
more than tidiness (see Part 3). If every candidate step is self-visited, the
agent is boxed in and terminates.

**Rule 2 — stepping uphill is ALLOWED; the carve makes it downhill.** An agent
may step onto higher ground, and Part 3 then cuts that cell below the previous
one. Descent is enforced by the cut, not by the step choice — which is what
removes the conflict between "always go downhill" and "turn toward water". A
hard no-climb rule would make those two goals fight whenever the water an agent
should reach sits up-slope.

The height term in the attractant already discourages climbing in metres, so
this should rarely bind, but `max_climb_m` remains a hard veto against
tunnelling straight through a mountain. If every candidate exceeds it, the
agent terminates.

**Merge — another agent's trail only.** On entering a cell carrying ANOTHER
agent's trail, add this agent's discharge to it and adopt the trail's stored
flow direction, then continue along it. Downstream cells are re-carved at the
summed discharge, so the channel widens and deepens below a confluence exactly
as it should. `channel_hydraulics` (already built and tested for the river
graph) turns the summed `Q` into width, depth and speed from Manning plus a
regime width closure, with continuity `Q = w·d·v` holding identically — so a
merge speeds the flow up by `v ∝ Q^0.2` rather than by an invented rule. **The
same hydraulics on both sides of the pipeline, one set of constants.**

Self-intersection is NOT a merge. Adding discharge to same-source water would
create water from nothing, and double again on each lap. Rule 1 makes the case
unreachable for an agent's own cells, and the union-find root check catches the
wider same-source case; the merge path must assert both rather than trust them.
Each trail cell records source id, discharge and flow direction.

**Terminate** when the agent leaves the map (dies), enters a lake (absorbed),
is boxed in by Rule 1 or Rule 2, or exceeds `max_steps`. The step cap is now a
pure backstop — Rule 1 already makes non-termination impossible, since the
visited set only grows and the grid is finite. A nonzero cap count means
something is wrong, not merely slow.

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
`canal_sense_distance_texels`, `canal_sense_angle_rad`, `canal_turn_angle_rad`,
`canal_wander_chance`, `canal_water_weight_m` (in METRES — see Part 2),
`canal_water_falloff_m`, `canal_max_climb_m`.

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
| Union aliasing | after A merges into B and C merges into A, all three resolve to one root, so C and B repel rather than attract |
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
