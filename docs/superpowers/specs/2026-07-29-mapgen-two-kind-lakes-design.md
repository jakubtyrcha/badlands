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

## Part 4 — Tag lake cells, and make the tag an INPUT to routing

Exclude from steepest descent exactly the cells carrying a **lake tag**, instead
of every cell with `water_level > h`.

```cpp
FlowRouting route_flow(const Field2D<float>& h, float texel_m, float epsilon_m,
                       const Field2D<uint8_t>* lake_tag = nullptr);
```

**Per-cell tagging, not a per-cell depth test.** The two are not the same, and
only one is safe. A depth test (`water_level - h > threshold`) leaves a real
lake's shallow margin untagged; inside a lake `hf` is nearly flat at the spill
level, so a margin cell's steepest descent can find a dry neighbour below its
ε-inflated `hf` — and a dry rim cell is only ever guaranteed to sit above the
level of *whichever cell claimed it*, which may have popped much earlier and
lower. That is the invented-exit failure from Stage 1, reappearing at the
shoreline. A tag meaning "belongs to a resolved lake" has no such hole: the
margin is in the lake, so it stays excluded with the rest of its component.

**Taking the tag as an input, rather than deriving it inside `route_flow`,
resolves an ordering circularity.** The tag depends on Part 3's water balance →
which needs catchment area → which needs the receiver graph → which needs the
exclusion. Derived internally, that forces a two-pass routing solve. Supplied
externally, it does not arise, and `carve_cavities` **already returns a per-cell
mask**, so seeded lakes need no analysis at all.

Pass 1 still floods: spurious pits still need filling and `water_level` is still
wanted. Only pass 2's exclusion changes.

**Where the tag comes from inside the loop.** Emergent lakes are not known until
they have been resolved, so iteration *N* is fed the tag resolved at *N−1*, and
iteration 1 uses the seeded mask alone. That is a relaxation rather than an
approximation error — the terrain is changing underneath it regardless — but it
does mean an emergent lake is routed as ordinary terrain for one iteration
before it is recognised. `erode()` therefore needs the basin mask threaded in,
which it does not currently take.

Expected effect: the 33–45% of channel texels currently flood-parent routed
drops toward zero, since ε-flats pond only micrometres and never earn a tag.

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
- `route_flow` takes an optional `const Field2D<uint8_t>* lake_tag`. Passing
  null keeps today's behaviour (exclude every `in_lake` cell), so existing
  callers and tests are unaffected.
- `erode()` takes the basin mask, which it does not currently receive; it owns
  the per-iteration tag and hands it to `route_flow`.

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
| Real lake with a shallow margin | the WHOLE component stays tagged and excluded; component exit count does not grow vs today. This is the case a per-cell DEPTH test fails and a per-cell TAG passes — worth testing both ways once, to pin why the tag exists |
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
  must not make it worse — hence tagging whole lakes rather than deep cells.
- **One-iteration lag.** An emergent lake routes as ordinary terrain for the
  iteration before it is recognised. Measure whether that visibly changes where
  emergent lakes settle; if it does, seed the tag from a cheap ponding test on
  iteration 1 instead of from the seeded mask alone.

## Deferred

- Re-asserting the notch after erosion, if the risk above proves real.
- Seasonal or climate-varying evaporation.
- Sediment infilling of lakes over time.
