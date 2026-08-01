# River carve + corridor refinement — design

Carves the river network into the terrain heightfield, refines the cluster-LOD
DAG locally so the carve has somewhere to live, and replaces the debug ribbon
with a real water surface.

## What is already settled

Decided in the design conversation, with the measurements that decided it.

| | value | why |
|---|---|---|
| runoff | **3.0 m/yr** | wet-maritime climate; peak Q 0.72 → 2.155 m³/s, trunk 4.24 → 7.34 m |
| `k_b` (bank coefficient) | **0.45** | gives 0.34 m brooks / 1.83 m trunk — a ladder, not a switch |
| base lattice | **2048²** (1 m) | unchanged, deliberately |
| corridor sample | **0.125 m** (8×) | first density at which a p50 brook spans >4 samples |
| corridor extent | `max(3w, 2 m)` | 21,479 m² = **0.51%** of the map |
| added vertices | **+1.35M** (+32%) | on a 4.19M base |

Two findings that shaped this and should not be re-litigated:

- **No bigger catchment exists.** The 16 km parent map's largest channel is
  0.766 m³/s and window W7 already carries 0.718 — 94% of it. Zero discharge
  exits the map frame; the landscape is endorheic and the drainage has not
  integrated in 600 kyr. Re-cutting the window cannot help.
- **Runoff scales the trunk, not the network.** Tripling it doubled the network
  length (4649 → 9266 m) by pulling brooks over the prune threshold, while the
  median width moved 0.48 → 0.52 m and median depth 0.05 → 0.06 m. Horton's
  laws: channel count grows geometrically as order falls, so total length is
  always dominated by the smallest channels. **99% of the network is, and stays,
  sub-metre.** Depth in particular scales as Q^0.3 and is effectively fixed.

## 1. The cavity model

```
cavity_depth(s) = 1.390 · d_flow(s)  +  k_b · w(s)^0.6
                  \__ bankfull __/      \__ bank cut __/
```

- **Bankfull term.** Channel form is cut by the 1–2 yr flood, not by mean flow.
  At `Q_bf = 3·Q` and `d ~ Q^0.3` that is a factor of 1.390. Stated as a
  recurrence interval, so it is a climate assumption rather than a look knob.
- **Bank term.** The exponent is *not* invented: downstream hydraulic geometry
  already gives `d ~ Q^0.3` and `w ~ Q^0.5`, hence `d ~ w^0.6`. `k_b` is the
  single free coefficient in the whole carve.

Resulting ladder at `k_b = 0.45` (W7 at 3 m/yr):

| percentile | width | cavity |
|---|---|---|
| p10 | 0.33 m | 0.28 m |
| p50 | 0.52 m | 0.34 m |
| p90 | 0.93 m | 0.84 m |
| trunk | 7.34 m | 1.83 m |

### Cross-section and bed elevation

- **Compact support, not a Gaussian.** A Gaussian never reaches zero and would
  dish the entire map. Use a raised cosine (or `1 − smoothstep`) over a finite
  half-width, so terrain outside the corridor is **bit-identical** to today.
- **The bed comes from the CENTRELINE, not from local terrain.** Subtracting a
  profile from the existing height inherits every terrain wiggle and lets the
  channel run uphill. Instead take bed elevation along the arc chain — which is
  already routed downhill — and blend the banks outward into the existing
  surface. This is what makes a carved channel actually flow.
- Distance and parameter come from `mapgen::arc_distance_m` / the arc's
  `param0..param1`, both closed-form. This is the payoff for having built the
  arc representation first.

## 2. The corridor mask

Conservative rasterization of the arc chains at the 1 m lattice, following the
precedent already set by `rasterize_rivers` ("a texel is covered when its SQUARE
overlaps the segment's capsule, not when its centre does"), extended from
segments to arcs.

The output is a connected region **R** of 1 m texels. Connectivity is the load-
bearing property: a corridor with a hole in it produces a pinched channel.

## 3. Refined tessellation and the fan band

### The rule that makes refinement provably local

**No new vertex may lie on ∂R.** The corridor boundary keeps only its coarse 1 m
vertices; fine vertices are inset and a fan band spans the gap.

Consequences, and they are the whole reason for this design:

- The coarse mesh outside R is untouched — same vertices, same triangles.
- `WeldChildren` (welds by exact float position) needs no change.
- `LockVertex` (locks by position on the group footprint) needs no change.
- The pinned seam-equality tests keep passing **unmodified**.

### Refinement is contiguous across R, not per texel

If each 1 m texel fanned to its own four corners, the bed would be pinched back
to the coarse lattice at every texel edge — a corrugated channel, worse than no
carve at all. R's interior is refined as **one continuous 0.125 m grid**,
including across leaf-tile boundaries that fall inside R.

Two tiles sharing an edge inside R must produce bitwise-identical vertices on
it. That holds because refinement is a pure function of `(global corridor mask,
fixed 8× subdivision, heightfield)` and never of which tile is being built.

### The fan band

Graded over a 1 m collar (1 → 0.5 → 0.25 → 0.125), which keeps every triangle
under 2:1 aspect. The alternative — fanning straight from 1 m to 0.125 m —
is watertight but produces 8:1 slivers that shade poorly and simplify badly.
The collar costs one extra metre of corridor width.

### The cluster triangle budget — the structural change

`kClusterTriBudget = 2 · kTileQuads² = 128`, and a leaf cluster **is** one 8×8 m
tile. Refined to 0.125 m that tile is 64×64 quads = **8192 triangles, 64× the
budget**. Runtime LOD selection and per-cluster draws both assume the budget
holds.

Fix, using the existing machinery rather than fighting it:

- A refined 8 m tile is emitted as **64 leaf clusters** of 1 m each (8×8 fine
  quads = 128 tris — exactly the budget).
- The group → simplify → split loop merges them 64 → 16 → 4 → 1 over three
  levels, at which point they match a coarse tile and join the normal hierarchy.
- Corridor regions run those three levels as a **pre-pass**, so every region
  holds level-0-equivalent clusters before the main loop begins.

Scale: the corridor touches roughly **336 of 65,536 tiles (~0.5%)**, so the
pre-pass is small and local.

## 4. The DAG no longer has uniform depth

This is the risky half of the change and gets the heaviest validation.

### What survives unchanged

`SelectClusters` is a **flat pass over every cluster**, not a traversal:

```
selected  ⟺  proj(own_error, own_sphere) ≤ tau  ∧  proj(parent_error, parent_sphere) > tau
```

It never reads `level`. Its correctness rests on **projected error being monotone
along every leaf→root chain**, which is a per-chain property and stays true under
mixed depth. The runtime cut needs no change, and that is the single most
important thing this audit establishes.

### The four sites that DO assume uniform depth

**(a) `terrain_clusters.cpp:648` — the scratch free. Most dangerous.**

```cpp
for (uint32_t c = 0; c < cluster_geom.size(); ++c) {
  if (dag.clusters[c].level == cur) cluster_geom[c] = ClusterGeom{};
}
```

Its comment states the assumption outright: *"Every level-`cur` cluster was in
`grid` and has now been welded into level cur+1."* Under mixed depth a level-`cur`
cluster may still be waiting in a region, and clearing it yields silent
use-after-clear — an empty cluster, not a crash.

*Fix:* free by **liveness, not level**. Track which cluster ids were consumed
this round and clear exactly those.

**(b) `cluster_terrain.cpp:127` — `level != 0` used to mean "is a leaf".**

```cpp
if (c.level != 0) continue;   // "Leaves are full-resolution, so their union is
                              //  the whole terrain."
```

Under mixed depth level 0 is no longer the leaf set, so the whole-map AABB is
computed from a subset and the entity-level cull clips terrain away.

*Fix:* the depth-independent leaf test is `own_group == kNoGroup`, which the
header already documents as the leaf sentinel.

**(c) `EmitCluster(dag, geom, out, cur + 1, gidx)` — level from the loop counter.**

A group consuming a mix of pre-passed corridor clusters and coarse leaves still
labels its output `cur + 1`. Level becomes a build-order artifact rather than a
tree property.

*Fix:* derive as `1 + max(child.level)`. Level is then descriptive only — nothing
correctness-critical may read it.

**(d) `dag.level_count` and `sel_level_hist_`.**

`sel_level_hist_.assign(dag_.level_count, 0)` then `if (c.level < size) ++hist[...]`
**silently drops** anything out of range. Debug UI only, but it would hide exactly
the anomaly we would be looking for.

*Fix:* size from `max(cluster.level) + 1` and assert no cluster is dropped.

### New invariants to pin

Existing `[terrain_clusters]` cases all build uniform trees. Each of these needs
a **mixed-depth fixture** — a small map with a synthetic corridor mask — in
addition to the uniform one:

1. **No T-junctions.** For every edge of every triangle in a selected cut, no
   other vertex lies in that edge's interior. This is the direct validation of
   the fan band and the strongest single check in the suite.
2. **Exact cover under mixed depth.** Sweep tau; for each, assert the selected
   clusters tile the map exactly once — area sums to the map area, with no
   overlap and no gap.
3. **Leafness is `own_group == kNoGroup`**, and the union of leaf bounds is the
   whole map. Pins the fix for (b) and forbids the `level == 0` idiom returning.
4. **Error monotonicity along every leaf→root chain**, on a mixed-depth tree.
   Existing test, new fixture — this is what `SelectClusters` actually depends on.
5. **No geometry is freed while live.** Assert every cluster consumed by a group
   had non-empty geometry at the moment of welding. Pins (a).
6. **`level_count > max(cluster.level)`**, and the selection histogram drops
   nothing. Pins (d).
7. **Corridor terrain outside R is bit-identical** to a build with an empty mask.
   This is the claim that "refinement is provably local", stated as a test.
8. **Parallel == serial, bitwise**, on the mixed-depth fixture. The existing
   determinism test with the new build path.

## 5. Water surface

Replaces the debug ribbon built in `src/mapview/river_surface.cpp`.

- Surface at `bed + d_flow`, so the water sits **inside** the carved cavity with
  the bank freeboard visible — that difference is what makes a carved channel
  read as a channel.
- Lake material (`BuildStillWaterForwardFactory` + `StillLakeWaterParams`).
- **Known limitation, accepted:** its Beer-Lambert extinction is calibrated to
  2.5–10 m visibility depths. At a 0.06 m median the brooks will render nearly
  clear. The carved cavity, not the water tint, is what will make them visible.
- Retires `kMinRibbonWidthM` — with a cavity to sit in, the surface can be drawn
  true to width.

## 6. Build order

Each step is independently testable and lands on its own.

1. **Corridor mask** — conservative arc rasterization at 1 m. Tests:
   connectivity, width, and that every channel texel is covered.
2. **Cavity field** — profile + centreline bed. Tests: monotone downhill, compact
   support (terrain outside R unchanged), ladder values at the percentiles above.
3. **Refined tessellation + fan + DAG changes** — §3 and §4. The risky step; all
   eight invariants above land with it.
4. **Water surface** — §5.

## Out of scope

Carried forward from the phase-2b exclusions and unchanged: foliage, a river
shader, lake surface animation, a more sophisticated terrain material, rocks.

The **brook material band** (wet/rocky terrain along the centreline) remains
wanted but is deferred — the cavity is the priority, and the band is a texture
problem that does not interact with any of the above.

## Risks

- **Step 3 is the whole risk.** Steps 1, 2 and 4 are pure CPU with no shared-code
  contact. If step 3 proves worse than estimated, steps 1–2 still ship as a
  carve at the 1 m lattice — visible for the trunk, invisible for brooks.
- **DAG memory.** +32% on a DAG already at 379 MB puts it near 500 MB, against a
  256 MiB WebGPU default that `GpuContext` already works around by requesting the
  adapter ceiling. If this bites, dropping the base to 1024² costs nothing real
  (2048² interpolates 16,384 source samples) and more than pays for the corridor
  — deliberately not taken now, but it is the release valve.
- **The `level` field becomes advisory.** After (c) it describes tree shape and
  nothing may branch on it. Worth a comment at the declaration, since the
  temptation to use it as a leaf test is exactly what (b) was.
