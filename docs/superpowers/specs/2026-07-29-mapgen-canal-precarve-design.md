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
`+sense_angle` from the heading. The attractant at a point is

```
attract = -height  +  trail_weight * trail(point)  +  lake_weight * is_lake(point)
```

so an agent prefers downhill, prefers an existing channel over open ground, and
prefers a lake over either.

**Steer.** Rotate `dir` toward the strongest sensor by `turn_angle`. With
probability `wander_chance`, apply an additional random turn — this is the only
source of meander, and it is what stops canals being straight lines.

**Trail attraction is what makes the network dendritic.** A drainage network
must converge — tributaries merge going downstream and never split. Without
trail attraction, Physarum steering braids and loops, which is wrong for water.

**Move** one texel along `dir`.

**Merge.** On entering a cell that already carries a trail, add this agent's
discharge to it and continue. Downstream cells are then re-carved at the summed
discharge, so the channel widens and deepens below a confluence exactly as it
should. `channel_hydraulics` (already built and tested for the river graph)
turns the summed `Q` into width, depth and speed from Manning plus a regime
width closure, with continuity `Q = w·d·v` holding identically — so a merge
speeds the flow up by `v ∝ Q^0.2` rather than by an invented rule. **The same
hydraulics on both sides of the pipeline, one set of constants.**

**Terminate** when the agent leaves the map (dies), enters a lake (absorbed),
or exceeds `max_steps`. The step cap is a loop backstop, not a design
parameter; hitting it should be logged.

## Part 3 — Carving, with guaranteed descent

Each agent carries a reference level that only ever falls:

```
ref_level -= canal_slope * step_length      // every step, unconditionally
B(cell)    = min(B(cell), ref_level)        // cut only where terrain is higher
```

Because `ref_level` is monotone, **a canal cannot contain a pit**, so any agent
that reaches the edge or a lake leaves behind a path water can follow all the
way. That is the property the whole change exists for, and it is structural
rather than emergent.

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
`canal_wander_chance`, `canal_trail_weight`, `canal_lake_weight`.

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
| Any fixture | no agent exceeds `max_steps`; `hit_step_cap == 0` |

**The headline production measurements**, all of which are the point of the
change and none of which are currently satisfied:

- channel texels routed by flood parent: **33–45% → under 5%**
- `in_lake` fraction of the sim grid: **16% → materially lower** (fewer closed
  depressions survive once the plains drain)
- seeded lakes possessing an outlet: **→ all of them**

## Risks

- **Braiding.** If trail attraction is too weak relative to wander, canals
  split instead of merging and the network stops being dendritic. Measure the
  merge count; if it is near zero, the weights are wrong.
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
