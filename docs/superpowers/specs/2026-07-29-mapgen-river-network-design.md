# Mapgen: river network rework

Replaces the per-texel `river` intensity raster with an extracted **flow graph**
— directed edges, physical discharge, solved hydraulics — conservatively
rasterized into physical output fields. Fixes the flow-direction defect the
graph would otherwise inherit, and separates the Lake biome from the carved
basin so a coast exists.

## Why

Two defects were measured on seed 2 while investigating "the flow is not
connecting the lakes and is not following the height gradient".

**Defect A — receivers rank by elevation, not gradient.** `hydrology.cpp:48`
sets `r.receiver[j] = i`, the priority-flood cell that first *claimed* `j`.
That ranks candidates by absolute level and ignores the distance to the
neighbour. A diagonal neighbour is √2 further away but on a surface tilted at
θ drops by `|cosθ| + |sinθ|` against the best orthogonal's
`max(|cosθ|, |sinθ|)` — so the diagonal is always at least as low and always
wins the pop race. Measured on tilted planes: **100% of receivers diagonal at
every tilt angle**, 0% matching steepest descent at 0° and 15°. Measured on the
production sim (544×544, 244,322 dry receivers): **96.1% diagonal** (an
unbiased D8 network is ~50%), only **60.4%** agreeing with steepest descent.

Priority-flood (Barnes 2014) is the correct algorithm for depression *filling*,
and the flood parent is a valid acyclic downstream pointer — which is why every
existing test passes; they assert only "never uphill" and "reaches the border".
Flow *direction* is conventionally a separate second pass. That pass is missing.

**Defect B — river gaps at lakes that do not exist.** `river_intensity`
(`erosion.cpp:398`) forces 0 on every `r.in_lake` cell, but `finalize_lakes`
prunes lakes under `min_lake_area_m2` / `min_lake_depth_m` *after* that flag is
set and never updates it. Measured: 5601 sim cells (11.3% of all `in_lake`
cells) are zeroed for lakes producing no visible water, against a total visible
river network of 2736 output cells. Those are scattered micro-depressions, so
they punch the network into disconnected fragments.

Defect B disappears by construction under the graph design below (the graph is
built from surviving lakes only), so it needs no separate fix.

## Decisions taken with the user

- Rivers become a **graph** — nodes with water amount, directed edges — and the
  texture is produced by **conservative rasterization** of that graph.
- Edge direction is structural, not conventional: each cell has exactly one
  receiver and `order` is topological, so the channel subgraph is a directed
  forest and a cycle is impossible.
- **Physical quantities, not aesthetic ramps.** "Intensity" (a 0..1 smoothstep
  over `log2(drainage area)`) is removed. The user rejected it as having no
  physical meaning, and rejected an initial speed model that over-determined
  the system.
- Hydraulics from **Manning plus a regime width closure**, solved so continuity
  `Q = w·d·v` holds identically.
- `MapArtifacts` carries `river_depth_m`, `river_speed_m_s`, `river_flow_dir`
  and the `RiverGraph` itself.
- Lakes get a **freeboard** at finalize so a dry bank of carved bowl is exposed,
  and Lake biome is stamped on `water_depth > 0`.

## Part 1 — Flow direction (prerequisite)

Keep priority-flood for filling; add a second pass for direction.

The filled surface is `hf[i] = r.water_level[i]`, which already satisfies
`hf ≥ h` everywhere; each cell is pushed at `wl ≥ parent's pop level + ε`, so
its `hf` strictly exceeds its parent's. For every non-border cell, assign

```
receiver[i] = argmax over 8-neighbours j with hf[j] < hf[i] of
                  (hf[i] - hf[j]) / dist(i, j)        // dist = texel or texel*sqrt(2)
```

ties breaking on linear index, exactly as the existing tie-break rule requires.

**The invariants hold, and this is load-bearing:**

- *A candidate always exists.* A cell's flood parent popped at a level strictly
  below the cell's own `wl`, so at least one strictly-lower neighbour is always
  present. The flood parent stays as a defensive fallback but is unreachable.
- *Acyclic.* Receivers strictly decrease `hf`, so no cycle can close.
- *`order` stays topological.* `order` is ascending in `wl = hf`, and every
  receiver has strictly lower `hf`, so a receiver still pops before its donors.
  This is what `incise`, `deposit` and `accumulate_drainage` rely on.

Cost is one extra O(8n) pass — negligible against the priority queue.

**One consequence needs verifying, not assuming.** `deposit`'s lake pour relies
on each lake component having exactly one exit, and `erosion.cpp:230-235`
justifies that from the *flood-parent* semantics this change replaces
("priority-flood claims each interior cell from exactly one already-visited
neighbour"). That argument no longer applies. A weaker one does: every non-lake
neighbour of a lake cell satisfies `h ≥ level + ε` — otherwise priority-flood
would have flooded it too — so from inside a lake the only strictly-lower
neighbours are other lake cells and the sill. Uniqueness therefore survives
*generically*, but two rim notches at exactly equal height would produce two
exits, where the old semantics could produce only one. The pour must be checked
against that case and `find_exit`'s comment rewritten; if multi-exit turns out
reachable, splitting the leftover volume across exits is the fix.

**Blast radius.** Every erosion result shifts: incision follows different paths,
so heightmaps, lakes and biomes move for all seeds. `erosion_tests.cpp` and
`generator_tests.cpp` expectations need re-baselining, and
`hydrology_tests.cpp:65-77` — whose comment currently rationalizes this exact
behaviour as "expected given the required tie-break, not a routing bug" —
becomes wrong. Its assertion strengthens: on a plane tilted purely in +x,
steepest descent sends every interior cell to its orthogonal −x neighbour, so
drainage should exit essentially entirely through `x = 0` rather than the 116
of 512 m³ measured today.

## Part 2 — Graph extraction

New module `src/mapgen/river_graph.{hpp,cpp}`.

```cpp
enum class RiverNodeKind : uint8_t { Source, Confluence, LakeInlet, LakeOutlet, Mouth };

struct RiverNode {
  glm::vec2 pos_m;          // WORLD METERS, float — deliberately not a texel index
  float ground_m;
  float drainage_area_m2;
  float discharge_m3_s;
  float width_m, depth_m, speed_m_s;
  int32_t lake_id;          // >= 0 for LakeInlet/LakeOutlet, else -1
  RiverNodeKind kind;
};

struct RiverEdge {
  int32_t from, to;                  // flow is ALWAYS from -> to
  std::vector<glm::vec2> points_m;   // dry-land polyline, world meters
  std::vector<float> discharge_m3_s; // per point
  std::vector<float> width_m, depth_m, speed_m_s;
};

struct RiverGraph { std::vector<RiverNode> nodes; std::vector<RiverEdge> edges; };
```

Storing world-space floats rather than texel indices is the load-bearing
choice — it is what lets the meander/carve followup work off the lattice.

**Algorithm**, over the final `FlowRouting`, drainage `area`, and the *pruned*
`water_depth`:

1. Label 4-connected components of `water_depth > 0` → `lake_id` per cell.
2. Channel set `C = { i : area[i] ≥ min_channel_area_m2 and lake_id[i] < 0 }`.
   Using pruned water rather than `r.in_lake` is what makes Defect B vanish.
3. Per cell in `C`, increment `donors[receiver]` when the receiver is also in `C`.
4. Node cells are: `donors == 0` (Source), `donors ≥ 2` (Confluence), a receiver
   outside `C` (Mouth if `receiver < 0`, LakeInlet if it enters a lake), and
   each lake's spill cell (LakeOutlet).
5. One node per lake, positioned at its **spill cell** — the cell where the
   component's receiver chain leaves it, located as `deposit`'s `find_exit`
   (`erosion.cpp:236-244`) already does. Inlet edges terminate at it
   topologically, but their polylines are
   **clipped at the shoreline**, so nothing is drawn across a lake surface. The
   lake surface is already water; the graph still records the connection.
6. Walk each chain from a node down the receiver pointers, appending cell
   centres in world metres, until the next node cell or a terminus.

Determinism comes from iterating candidate starts in linear-index order over a
deterministic routing.

## Part 3 — Hydraulics

Per polyline point, given `Q` and the local slope `S`:

```
Q = runoff_m_per_s · drainage_area_m2          discharge
w = k_w · sqrt(Q)                              regime width closure (exponent b = 0.5)
v = (1/n) · d^(2/3) · sqrt(S)                  Manning, wide channel (R ≈ d)
Q = w · d · v                                  continuity
  ⟹  d = ( Q·n / (w · sqrt(S)) )^(3/5)
      v = Q / (w · d)
      cross_section = w · d = Q / v            exact by construction
```

Two constants: `k_w` and Manning's `n`. The implied exponents are `w ∝ Q^0.5`,
`d ∝ Q^0.3`, `v ∝ Q^0.2`, summing to 1.0 — continuity holds identically, not
approximately. At fixed `Q`, a steeper reach is shallower and faster (rapids);
a flat reach is deeper and slower.

`S` is taken per segment from the endpoints' ground heights over their
separation, floored at `kMinChannelSlope` (≈1e-4) or `d` diverges on flats.
Lakes are nodes rather than edges, so a lake's zero slope never enters this.

**Scale honesty.** At ~1 m/year runoff a 512 m map drains at most ~0.008 m³/s.
These are creeks, not rivers. `runoff_m_per_s` stays physically honest and the
renderer decides what reads as a river; inflating the constant would make every
unit downstream fictional.

## Part 4 — Conservative rasterization and outputs

Each consecutive point pair is a capsule whose radius interpolates from `w0/2`
to `w1/2`. A texel is covered when **the texel's square** — not its centre —
comes within the interpolated radius of the segment axis. Per covered texel,
write `depth`, `speed` and the unit flow direction sampled at the projection
parameter; on overlap the higher-discharge segment wins, so a trunk beats its
tributary at a confluence.

**Why conservative specifically:** consecutive segments share endpoints and each
segment covers every texel it touches, so the covered set is connected by
construction — even at zero width, since the axis still passes through texels.
That is precisely the fragmentation that today's centre-sample + `dilate_river`
+ `resample_max_pool` chain produces, and all three of those are deleted.
Rasterizing at output resolution from world-space geometry also makes
resolution independence free rather than something `resample_max_pool` has to
approximate.

`MapArtifacts::river` (0..1) is **replaced**:

```cpp
Field2D<float>     river_depth_m;    // 0 outside channels
Field2D<float>     river_speed_m_s;  // 0 outside channels
Field2D<glm::vec2> river_flow_dir;   // unit vector; (0,0) outside channels
RiverGraph         river_graph;
```

Per-texel discharge and cross-section are deliberately absent: both are
through-flow across a *section*, so a per-texel value is a category error.
Width is encoded by how many texels the rasterizer covers, and both quantities
are exact on the graph.

`a.river` has no consumers outside mapgen — only `outputs.cpp:40` (the `map.png`
overlay) and `outputs.cpp:69` (`rivers.png`) plus tests — so this is contained.
The overlay switches to `river_depth_m > 0`; `rivers.png` renders depth.

## Part 5 — Lake shoreline

In `finalize_lakes`, per 4-connected component, take the component's minimum
`r.water_level` as its spill level (the pattern `deposit` already uses for
`wl_cap`), then lower it before computing depths:

```
level = spill - min(lake_freeboard_m, lake_freeboard_frac · max_depth)
depth[i] = max(0, level - ground[i])
```

Both are `ErosionParams` fields rather than constants, since the freeboard is
tuned by eye against the preview and the fractional cap is what stops small
ponds from vanishing outright.

Pruning by area/depth follows unchanged. The sim loop is untouched, so erosion
behaviour does not shift — only the output level drops, exposing a dry band of
already-carved bowl. Sediment the loop deposited above the new level becomes
dry land, which reads as a beach or delta.

The Lake stamp becomes `water_depth > 0`, so the biome covers exactly the water
and the exposed bank keeps whatever `classify_biomes` assigned — Plains, at
bedrock minima. `kLakeStampMinDepthM` is removed. A dedicated `Shore` biome
would fit (7 of `terrain_blend.wesl`'s 8 slots) but is deliberately not part of
this change.

## Interface changes

- `MapArtifacts`: `river` removed; `river_depth_m`, `river_speed_m_s`,
  `river_flow_dir`, `river_graph` added.
- `ErosionOutputs::river` removed; `river_intensity`, `dilate_river` and the
  `resample_max_pool` river path are deleted.
- `ErosionParams`: `stream_min_area_m2` / `river_area_m2` replaced by
  `min_channel_area_m2`, `runoff_m_per_s`, `channel_width_coeff` (`k_w`),
  `manning_n`, `lake_freeboard_m`, `lake_freeboard_frac`.
- `kLakeStampMinDepthM` removed from `generator.hpp`.
- `route_flow`'s signature is unchanged; only receiver semantics change. Its
  header comment must be rewritten — it currently documents the flood-parent
  behaviour as the contract.

## Testing

Routing (`hydrology_tests.cpp`):

- On planes tilted at 0°, 15°, 22.5°, 30°, 45°, 60°, **every** interior receiver
  equals steepest descent. This is the direct regression for Defect A and fails
  loudly today (0% at 0°).
- Receiver graph is acyclic, and `order` remains topological with respect to the
  new receivers — walked explicitly, since three consumers depend on it.
- The tilted-plane drainage test is rewritten: flow exits through `x = 0`.

Graph (`river_graph_tests.cpp`, new target member of `badlands_erosion_tests`):

- Every edge runs from higher to lower ground; walking from any node terminates
  at a Mouth or a lake, never revisiting a node.
- Discharge is conserved at confluences within tolerance.
- Same seed produces an identical graph.
- No edge polyline enters a cell with `water_depth > 0`.

Hydraulics:

- `w · d · v == Q` to float tolerance across a sweep of `Q` and `S`.
- At fixed `Q`, `v` is strictly increasing in `S` and `d` strictly decreasing.

Rasterization:

- The covered set of every edge is 4-connected end to end — the fragmentation
  regression test.
- A synthetic diagonal channel of sub-texel width still produces an unbroken
  covered chain.
- The covered world-space band matches between 256 and 512 output resolution.

Lakes (`generator_tests.cpp`):

- Cells stamped Lake are exactly the cells with `water_depth > 0`.
- Every surviving lake above a size threshold has at least one ring of dry
  carved bowl between its waterline and the basin rim.

## Followup — carved, winding rivers (item 1, not in this change)

The graph is designed to make this possible without rework:

- **Meander:** Chaikin or Catmull-Rom subdivision of `points_m` with lateral
  offset noise scaled by `width_m`, endpoints pinned so topology and lake
  connections survive.
- **Carve:** a channel cross-section from `width_m` / `depth_m` subtracted from
  the heightmap along each polyline.
- **Ordering constraint:** carving must run *before* the water-depth recompute
  and *before* the heightmap smoothing of
  `2026-07-29-mapgen-heightmap-smoothing-design.md`, or the channel is blurred
  away.

## Sequencing note

This spec and the heightmap-smoothing spec both modify the Lake stamp at
`generator.cpp:255-257`. Whichever lands second must reconcile: smoothing moves
the stamp after the depth recompute, and this change alters its threshold to
`> 0`.

## Deferred

- A dedicated `Shore` biome for the exposed bank.
- Per-texel discharge as a convenience channel.
- Sub-threshold tributaries as graph edges (they remain drainage area only).
- Lake level from a water balance rather than spill-minus-freeboard.

---

## v1.1 addendum (2026-07-29, post-Stage-1 review with the user)

Stage 1 (Part 1) shipped as `6e09fad`. These changes were agreed after the
original spec was written and supersede it where they conflict.

### Stage 1 outcome — what the spec got wrong

- **No re-baselining was needed.** The spec predicted every erosion expectation
  would shift. In fact the existing `erosion_tests.cpp` / `generator_tests.cpp`
  cases are structural (conservation, bounds, monotonicity) rather than
  magnitude-pinned, and all passed unchanged.
- **Flooded cells must be EXCLUDED from steepest descent.** The spec assumed
  one rule everywhere. A lake surface is flat and the ε tilt across it is
  bookkeeping, not slope; ranking it by steepest descent invents downhill exits
  through the rim (42 of 113 components grew extra exits, worst 18). Lake cells
  keep the pass-1 flood tree.
- **The single-exit-per-lake claim was already false**, independent of routing.
  4-connected component labels against an 8-connected receiver graph gave 33 of
  82 components multiple exits under the *old* code. `deposit`'s `find_exit`
  therefore sheds leftover overflow at whichever exit `member[0]`'s chain
  reaches, not the true sill. Deterministic and volume-conserving, but
  physically imprecise. **Part 2's LakeOutlet node must locate the true sill
  (the boundary member with the lowest `water_level`) rather than reuse
  `find_exit` as-is.**
- Measured after: dry receivers 51.7% diagonal (unbiased D8 ≈ 50%), 99.2%
  agreeing with steepest descent.

### Known Stage 2 input: ε-flats (deferred by the user)

16.8% of sim cells are flagged `in_lake` and keep flood-tree routing (96.2%
diagonal). Most are shallow ε-flats that `finalize_lakes` **prunes to dry
land**, so Part 2's channel set will include them while their paths follow
flood-expansion order rather than any gradient. De-latticing cannot repair this
— it would smooth a wrong path into a smooth wrong path.

Two candidates, to be decided before extraction lands: rank those cells by true
ground `h` instead of ε-tilted `hf`, or exclude pruned depressions from the
`in_lake` routing exclusion so they get steepest descent like any dry cell. The
second is likely right — a depression pruned to dry land arguably should not
have routed as a lake.

### De-latticing is mandatory (new)

Part 2 step 6 ("appending cell centres") destroys the information needed for
soft turns: D8 offers only 8 headings, so any other heading becomes a
staircase. Quantization is bounded — with `f` the diagonal fraction, sinuosity
is `[(1−f) + f√2] / √(1+f²)`, peaking at exactly 22.5° (`f = 0.414`) at
**1.0824** — but a ~2-texel wobble is not a meander, and rendering cannot
recover the heading afterwards.

The heading is not actually lost: the E:NE step *ratio* encodes it exactly. So
after chain-walking, add:

- **Douglas–Peucker** simplify at ≈0.75–1 texel tolerance.
- **Uniform arc-length resample** (~2–4 texels).

### Hierarchy: Strahler + Shreve (new)

Each `RiverEdge` carries `strahler_order` (leaves 1; equal child orders `i`
give `i+1`, else the max) and `shreve_magnitude` (sum of children; equals the
upstream source count). One pass over the forest in reverse topological order.

### Outputs: physical quantities, hierarchy by colour (supersedes Part 4)

The original spec excluded per-texel discharge as a "category error" while
rasterizing depth and speed — which are equally reach-averaged attributes. The
distinction was arbitrary and is **dropped**.

Width cannot carry hierarchy at this world scale: at ~1 m/yr runoff a 512 m map
drains at most ≈0.0025 m³/s at its largest outlet, so `w = k_w·√Q` ≈ **0.25 m**
— sub-texel. A 5 m river needs ~31 km² of catchment (~10 km map). Runoff stays
physically honest and the rasterized band stays ≈1 texel; width remains on the
graph and is recomputable anywhere as `k_w·√Q`.

```cpp
Field2D<float>     river_discharge_m3_s;  // reach discharge; drives colour
Field2D<uint8_t>   river_class;           // calibrated tier; 0 = no channel
Field2D<float>     river_depth_m;
Field2D<float>     river_speed_m_s;
Field2D<glm::vec2> river_flow_dir;
RiverGraph         river_graph;           // width_m, strahler, shreve
```

`river_class` uses absolute log decades, so a class means the same flow on any
map size and a larger map promotes reaches naturally:

| class | name | Q (m³/s) |
|---|---|---|
| 0 | Rill | < 1e-4 |
| 1 | Brook | 1e-4 – 1e-3 |
| 2 | Stream | 1e-3 – 1e-2 |
| 3 | Creek | 1e-2 – 1e-1 |
| 4 | River | 1e-1 – 1e0 |
| 5 | Major | ≥ 1e0 |

At 512 m the network spans ≈5e-5 to 2.5e-3 m³/s, so only classes **0–2** are
used. If three tiers read as too coarse in the preview, half-decade steps
double them within the same absolute scheme.

`rivers.png` renders `river_class` through a per-class palette (following
`write_biome_png`), not autoscaled grey — that image is the check that
hierarchy is legible.

### Followup redefined: meander cannot ship alone

The original followup named only the carve. A free lateral offset would put the
visual river on a valley wall while `flow`, `water_depth` and the heightmap all
still say the water runs down the thalweg, and carving it would cut a second
disconnected low path. The coherent unit is three parts, shipped together:

1. **Floodplain-constrained offset** — scan perpendicular to flow until height
   exceeds thalweg + Δ and clamp the offset to that half-width. In a narrow
   bedrock gorge the half-width goes to zero, so mountain streams correctly
   stop meandering.
2. **Carve**, drawing from `S` before `B`, mirroring `incise`
   (`erosion.cpp:171-182`) and `diffuse` (`erosion.cpp:376-385`).
3. **Re-route** on the carved heightmap so flow and geometry agree by
   construction.
