# Mapgen: two-kind lakes, outlet notches, and flat routing

Splits "lake" into two kinds with different provenance and different rules —
**seeded** lakes carved deliberately from the bedrock quantile, and **emergent**
lakes the erosion sim produces — then fixes the flat-routing defect that the
current single-kind model forced.

## Why

**1. A third to half the river network is routed by flood order, not gradient.**
`route_flow` flags a cell `in_lake` whenever `water_level > h`, which catches
real lakes *and every flat*: on level ground the flood front always arrives
above the cell's own height. Stage 1 excluded those cells from the
steepest-descent pass, so they keep the priority-flood claim parent (96.2%
diagonal). Measured on seeds 1–3, **33–45% of channel texels** are routed that
way (45.2% / 33.5% / 44.2%).

The exclusion was necessary: a lake surface is flat, the ε tilt across it is
bookkeeping rather than slope, and ranking it by steepest descent invented
downhill exits through the rim (42 of 113 components). But it is far too broad.

**2. The freeboard is art direction wearing a physical name.** `lake_freeboard_m`
lowers the output water level below the spill point so a dry bank exists. A real
water balance argues the opposite: balance needs
`runoff × catchment = evaporation × lake_area`, and with temperate lake
evaporation ~600–1000 mm/yr against ~200–500 mm/yr of runoff, that requires the
lake to cover 30–60% of its own catchment. Real lakes are 1–10%. **Physics says
these lakes should be full to the brim.** The coast has to come from terrain
shape, not from lowering the water.

**3. Both kinds of lake exist and should not share one rule.** Seeded cavities
are placed deliberately and should always read as lakes with a coast. Lakes the
simulation forms should also be allowed — but only where enough water actually
accumulates, rather than wherever the fill happens to pond a millimetre.

## Decisions taken with the user

- Seeded cavities get a **carved outlet notch** — "the ultimate lake".
- Emergent lakes are allowed **if the simulation accumulates enough water**.
- The two kinds must be **distinguishable** in the data, not merged.
- Spec before code.

## Part 1 — Two kinds

```cpp
enum class LakeKind : uint8_t { Seeded, Emergent };
```

The distinction is provenance, and it is already available: `carve_cavities`
returns the basin mask, and a flood component either touches it or does not.
`micro_fill` already makes exactly this test (`touches_basin`,
`erosion.cpp:126-131`), so this follows an established pattern rather than
inventing one.

- **Seeded** — the component intersects the basin mask. Always a lake; gets a
  notch; never pruned.
- **Emergent** — it does not. Must earn its place by the water test in Part 3.

Kind is carried per lake into `ErosionOutputs` and onto the river graph's lake
nodes, so downstream consumers (biome stamping, the renderer, gameplay) can
treat a deliberate lake differently from an incidental pond.

## Part 2 — Outlet notch (seeded lakes)

A bowl fills to its **lowest rim point**. `carve_cavities` makes the cone zero
exactly at the mask boundary, so the rim is the surrounding terrain and the
water reaches it everywhere — there is no bank by construction. Lowering the
water is unphysical; lowering *one point of the rim* is what real lakes have.

Per seeded basin component, after carving the cone:

1. Find the lowest non-basin cell adjacent to the component — where it would
   spill anyway.
2. Lower that cell, and a one-texel neighbourhood so the notch is not a
   single-texel artifact, to `min_rim_height − notch_depth_m`.

Because that cell was already the minimum, lowering it keeps it strictly lowest,
so the spill level drops by exactly `notch_depth_m` and the band of bowl above
it stays dry. Routing needs no change at all: priority-flood still finds the
true spill, it is simply lower now.

**The tuning relationship, which is the number to reason about:**

```
bank_width_m = notch_depth_m / basin_cone_slope
```

At today's cone slope (`kSlopeMPerM × 2/3` = 0.5 m/m), a 1 m notch gives a 2 m
bank — two texels at 1 m/texel, visible but thin. A 3 m notch gives 6 m. Pick
`notch_depth_m` from the preview against the coast width wanted, not from the
depth number in isolation.

This **replaces** `lake_freeboard_m` / `lake_freeboard_frac`, which are removed.

## Part 3 — Emergent lakes must earn it

Replace the shape-only thresholds (`min_lake_area_m2`, `min_lake_depth_m`) with
a water balance, so "enough water accumulates" is measured rather than assumed.

For a candidate depression with catchment area `A_c` and hypsometry
`A_lake(level)`:

```
A_bal = runoff_m_per_s * A_c / evaporation_m_per_s     // area evaporation can sustain

if A_bal >= A_lake(spill):   fills to spill      (exorheic — overflows, feeds a river)
elif A_bal <= 0:             dry                  (no lake)
else:                        level solves A_lake(level) == A_bal   (endorheic)
```

Three things this buys:

- Tiny pits with negligible catchment resolve to **dry**, which is what the
  area/depth thresholds were approximating.
- Endorheic lakes get a level **below** their spill, so they gain a coast for
  free — the physically honest version of what the freeboard was faking.
- It reuses machinery we already have: the hypsometry is one pass over members
  sorted by height, exactly what `deposit`'s lake pour already builds
  (`erosion.cpp:290-297`).

One new constant, `evaporation_m_per_s`. Per the numbers in "Why", most lakes
will land in the exorheic branch; the branch that matters is the dry one.

## Part 4 — Narrow the routing exclusion

Exclude from steepest descent only cells belonging to a **component whose
maximum ponded depth exceeds `kPondedMinDepthM`** — not every cell with
`water_level > h`.

**Component-level, not cell-level, and that is load-bearing.** A per-cell
threshold would let a real lake's shallow margin route by steepest descent while
its interior did not, reintroducing invented rim exits exactly at the margin.
Testing the component keeps genuine lakes wholly excluded and lets ε-shallow
flats route by gradient in their entirety.

`micro_fill` already uses a 0.75 m cap (`kMicroFillCapM`) for this same class of
"not really a lake" depression; `kPondedMinDepthM` should start there.

Expected effect: the 33–45% of channel texels currently flood-parent routed
drops toward zero, since ε-flats pond only micrometres.

## Interface changes

- `ErosionParams`: `lake_freeboard_m`, `lake_freeboard_frac`,
  `min_lake_area_m2`, `min_lake_depth_m` removed; `notch_depth_m`,
  `evaporation_m_per_s` added.
- `ErosionOutputs`: add `Field2D<uint8_t> lake_id` and a
  `std::vector<LakeKind>` (or a small `LakeInfo` vector carrying kind, level,
  catchment and outlet cell).
- `RiverNode`: add `LakeKind` alongside the existing `lake_id`.
- `carve_cavities` gains the notch pass, so it needs the component labelling it
  does not currently do.
- New `kPondedMinDepthM` in `erosion.hpp`.

## Testing

Artificial heightmaps as in the river-graph work — the two builders
(`make_plane`, `make_valley_network`) already exist in
`river_graph_tests.cpp` and should move somewhere shared.

| Fixture | Expectation |
|---|---|
| Bowl with a carved notch | spill level is exactly `min_rim − notch_depth_m`; a dry band of width `notch_depth_m / cone_slope` exists between waterline and rim |
| Seeded basin, any catchment | always a lake, never pruned, kind == Seeded |
| Pit with negligible catchment | resolves dry (the balance's dry branch) |
| Pit with large catchment | fills to spill, kind == Emergent, exorheic |
| Depression with intermediate catchment | endorheic: level strictly below spill, `A_lake(level) == A_bal` to tolerance |
| Broad ε-flat, no real ponding | every cell routes by steepest descent; 0% flood-parent routed |
| Real lake with a shallow margin | the WHOLE component stays excluded; component exit count does not grow vs today |
| Valley → notched lake → outlet | the graph has LakeInlet, LakeOutlet and a river resuming below the notch |

Plus the production measurement as an explicit check, not just eyeballing:
**channel texels routed by flood parent must fall from 33–45% to under 5%.**

## Risks

- **Notch vs. erosion.** The sim incises the outlet over time, deepening the
  notch and draining the lake further. The notch is carved once at init, so
  `iterations` now indirectly controls bank width. Needs measuring; if it
  matters, the notch may need re-asserting after the loop.
- **Conservation.** Removing `min_lake_area_m2` pruning changes which cells
  `deposit` treats as lake, so its bucket/pour path and the two conservation
  tests (`erosion_tests.cpp:481`, `:608`) must be re-checked.
- **The multi-exit issue is untouched.** Components can still have several
  exits (pre-existing, 4-connected labels vs 8-connected receivers). Part 4
  must not make it worse — hence the component-level test above.

## Deferred

- Re-asserting the notch after erosion, if the risk above proves real.
- Seasonal or climate-varying evaporation.
- Sediment infilling of lakes over time.
