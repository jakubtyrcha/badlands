# Stage 1 (`coarse-hydraulic-erosion-sim`) — benchmark + reference gap

Exploration report. No code changed. Establishes the current state as a
benchmark and lists where it diverges from the reference it claims to follow.

- **Reference article:** [Meandering Rivers in Particle-Based Hydraulic Erosion
  Simulations][art] (Nick McDonald, 2023-12-12)
- **Reference source:** `weigert/SimpleHydrology` @ master — the article's
  companion repo. Every claim below was checked against the actual source, not
  the article's prose.
- **Our code:** `tools/protogen/protogen.cpp`

[art]: https://nickmcd.me/2023/12/12/meandering-rivers-in-particle-based-hydraulic-erosion-simulations/

---

## 1. Benchmark run

Reproduce exactly:

```sh
c++ -O3 -std=c++23 -I src -I third_party/FastNoiseLite -I third_party/glm \
  -I build/_deps/taskflow-src \
  tools/protogen/protogen.cpp src/core/parallel.cpp \
  src/mapgen/hydrology.cpp src/mapgen/river_graph.cpp src/mapgen/river_prune.cpp \
  src/mapgen/coarse_io.cpp src/mapgen/river_io.cpp -o /tmp/protogen

mkdir -p /tmp/pg16 && /tmp/protogen --res 1024 --world 16384 --steps 3000 \
  --drops 4096 --snapshot-every 500 --out /tmp/pg16
python3 tools/protogen/show.py /tmp/pg16
```

Seed 1, 1024² cells, 16 km world (16 m cells), 900 m initial relief,
3000 steps × 4096 drops = 12.3 M particles. `--test` is green (27/27).

### Evolution

| step | relief | lakes | wet | deepest |
|---|---|---|---|---|
| 500 | 872.8 m | 75 | 10.34% | 281.0 m |
| 1000 | 870.0 m | 96 | 9.21% | 314.5 m |
| 1500 | 852.7 m | 85 | 6.29% | 320.4 m |
| 2000 | 808.7 m | 74 | 6.31% | 281.1 m |
| 2500 | 808.4 m | 93 | 4.91% | 283.9 m |
| 3000 | 808.3 m | 74 | 4.35% | 266.7 m |

### Final state

| quantity | value |
|---|---|
| relief | 808.3 m (from 900 m) |
| median terrain slope | 18.2° (p90 56.0°) |
| land under 2° / 5° | 12.3% / 26.8% |
| soil | mean 21.6 m; 40.0% of cells stripped to bedrock; max 392 m |
| lakes | 74, 4.35% wet, deepest 266.7 m, 0.33 km³ stored |
| river graph | 32 117 nodes, 32 047 edges, 1 185 km channel |
| runtime | 373 s — **drops 357.8 s, grid 2.2 s, lakes 13.0 s** |

**Relief has converged; the lake population has not.** Relief is flat from step
2000 (808.7 → 808.3 m) but wet fraction is still falling monotonically
(10.34% → 4.35%). The run is not in steady state — basins are still silting up,
so "lakes at end" is a snapshot of a decaying transient, not a converged value.

**The lake statistics are not a stable benchmark quantity.** Lake count swings
74 / 96 / 85 / 74 / 93 / 74 across consecutive snapshots. Any tuning judged on a
single end-of-run lake count is reading noise.

### This does not reproduce the README's table

`tools/protogen/README.md` cites run M16b at the same settings:

| | README (M16b) | this run |
|---|---|---|
| final relief | 808 m | 808.3 m ✅ |
| runtime | 5 min | 6.2 min ✅ |
| lakes at end | 37 | **74** ❌ |
| wet at end | 3.20% | **4.35%** ❌ |
| deepest lake | 308 m | **266.7 m** ❌ |

Relief and runtime match to the digit; every lake number is off by ~2×. Either
the code moved since M16b or the table is stale — worth resolving before it is
used as a baseline.

### Renders

`/tmp/pg16/3000-step-map.png` (hillshade + rivers + lakes), `-hillshade.png`,
one per snapshot. Regenerate with the `show.py` line above.

---

## 2. What we implement faithfully

Checked line-by-line against `SimpleHydrology/source/{water,world,cellpool}.h`:

- The **descend loop**: finite-difference normal, gravity force, momentum
  transfer force, `sqrt(2)·normalize(speed)`, `pos += speed`.
- The **equilibrium law** `c_eq = (1 + entrainment·discharge)·(h_here − h_there)`,
  clamped at 0, and the `deposition_rate·cdiff` mass transfer.
- Mass-conservative **evaporation** (`sediment /= (1−e)`, `volume *= (1−e)`).
- **Discharge and momentum EMA maps**, double-buffered, one EMA per step over
  all drops — structurally identical to the reference's `erode(cycles)`.
- **Cascade** (thermal erosion / angle of repose), in place, per particle.
- **Every `Drop` constant verbatim**: `evapRate` 0.001, `depositionRate` 0.1,
  `minVol` 0.01, `maxAge` 500, `entrainment` 10, `gravity` 1,
  `momentumTransfer` 1, `settling` 0.8.

The README's hard-won findings (serial in-place particles, in-place cascade,
flux divergence in lakes, sediment displaces water) all hold up and should stay.

---

## 3. Divergences from the reference

Ordered by how much they bear on meandering.

### 3.1 The momentum force is unbounded — the reference's is not 🔴

The reference uses **two different discharge values** in the same function:

```cpp
// water.h:99  — momentum denominator: RAW discharge, unbounded
speed += momentumTransfer*dot(normalize(fspeed),normalize(speed))
         /(volume + cell->discharge)*fspeed;

// water.h:127 — c_eq: the erf-SQUASHED accessor, in [0,1]
float c_eq = (1.0f+entrainment*node->discharge(ipos))*(cell->height-h2);
//                                  cellpool.h:243  return erf(0.4f*get(p)->discharge);
```

protogen uses its erf-squashed field, bounded to `[0,1]`, in **both**
(`protogen.cpp:773` and `:789`).

Since `|fspeed|` grows with traffic, the reference's force
`≈ |fspeed|/(1+Q_raw)` **saturates at ~√2**. Ours is `|fspeed|/(1+erf(...)) ≈
|fspeed|/2` and **grows linearly with traffic, without limit**.

The gravity term has an exact closed form here. With `scale = relief/cell`, the
finite-difference gradient is `gx = tan θ` and `nl = 1/cos θ`, so

```
gravity contribution = (gravity/volume)·(gx/nl) = sin θ / volume   ≤ ~1.65
```

bounded, because `volume ≥ 0.606`. So the reference's momentum force,
saturating at **~√2 ≈ 1.41**, is deliberately *the same order as gravity* —
the two genuinely compete, and that competition is what lets a channel be
pushed off its line and pulled back downhill. That is the meander mechanism.

Measured traffic in this run (p99 = 100 visits/cell/step, max = 8691):

| | reference | protogen |
|---|---|---|
| momentum force, p99 channel cell | ≈ 1.4 | **≈ 52** |
| momentum force, busiest cell | ≈ 1.4 | **≈ 4900** |
| gravity force (`sin θ / volume`) | ≤ 1.65 | ≤ 1.65 |

In the reference the two are comparable. Here momentum outweighs gravity by
**30–3000×**, and the gap widens with channel size — exactly in the trunk
rivers where meanders belong. After renormalisation to `√2` the particle
follows the stored momentum field almost exactly. Channels become
self-perpetuating and rigid; they cannot migrate, and migration *is*
meandering.

This is the single most likely reason we get winding valleys and not meanders.

### 3.2 `erf` is applied inside the EMA, not on read 🔴

```cpp
// reference world.h:83 — EMA over RAW, erf on read (cellpool.h:243)
cell.discharge = (1-lrate)*cell.discharge + lrate*cell.discharge_track;

// protogen.cpp:961 — erf folded INSIDE the average
g.discharge_b[i] = (1-lr)*g.discharge[i] + lr*std::erf(es*g.vol_track[i]);
```

`erf` is concave, so by Jensen `E[erf(x)] ≤ erf(E[x])` — the field systematically
under-reads, worst for bursty flow. Two consequences:

- `c_eq`'s entrainment term is under-fed, so channels incise less than intended.
- **The raw discharge field is destroyed**, which is *why* 3.1 happened: there is
  no unsquashed field left to put in the momentum denominator.

Fixing 3.2 is a prerequisite for fixing 3.1. Both are cheap to A/B.

### 3.3 No vegetation — the reference's bank-cohesion feedback is missing 🔴

The reference's erosion rate is modulated by roots:

```cpp
// water.h:86
float effD = depositionRate*(1.0f - cell->rootdensity);
```

with a full feedback loop in `vegetation.h`: plants spawn where discharge is low
and slope is gentle, **die where discharge is high** (a river washes them out),
and root into a 3×3 stencil (1.0 centre / 0.6 edge / 0.4 diagonal).

Net effect: **the channel bed is bare and erodible while the banks are
vegetated and resistant.** That lateral cohesion contrast is the textbook
prerequisite for meandering rather than braiding.

We have no vegetation. Our substitute — the two-layer bedrock/soil substrate —
resists **vertically** (depth-stratified), not **laterally**. It is a good model
of what it models; it does not do this job.

Note that the slot where `(1 - rootdensity)` sits in the reference holds
`volume` in ours (`protogen.cpp:796`). That factor is defensible on its own
terms — see 3.5, it is the mass-consistent choice — but it means there is no
longer anywhere for a cohesion term to go without revisiting that line.

### 3.4 `lrate` is 10× the reference's, under a "DO NOT RETUNE" comment 🟡

```cpp
float World::lrate = 0.1f;   // world.h:42
float lrate = 0.01f;         // protogen.cpp:100, under
                             // "reference erosion constants ... DO NOT RETUNE"
```

The comment block is accurate for all eight `Drop` constants and wrong for this
one. A 10× longer EMA time constant makes the discharge and momentum fields
sluggish: channels persist harder and adapt slower, which again suppresses
migration. Whether 0.01 was a deliberate choice is not recorded.

### 3.5 Mass leak: a concentration is deposited as a mass 🔴

`sediment` is a **concentration** — the code rescales it by `/(1−evap)` in step
with `volume *= (1−evap)`, so the carried mass is `sediment × volume`. The
transfer step respects this (terrain moves `volume·deposition_rate·cdiff`).
But all three exit paths book the bare concentration:

| site | code | should be |
|---|---|---|
| `protogen.cpp:676,819` death | `Deposit(g, c, sediment)` | `sediment * volume` |
| `protogen.cpp:718–724` lake | `Deposit(g, here, drop)` | `drop * volume` |
| `protogen.cpp:670,785` off-map | `lost_offmap += sediment` | `sediment * volume` |

`max_age` is the dominant exit (the code says so at `:816`) and volume decays to
`0.999^500 = 0.606` by then, so deposits land **up to 1.65× too heavy**.

Measured on this run, using the test's own formula
`residual = Δsum(h) + lost_offmap`:

| | value |
|---|---|
| sum(h) initial → final | 5.4286e5 → 5.1008e5 (−6.04%) |
| `lost_offmap` counter | 5.7686e4 (−10.63%) |
| **unexplained mass gain** | **+2.49e4 = +4.59% of initial** |

The test asserts this residual stays under 1%. **Production is 4.6× over.**

The suite passes because the leak is cumulative and its fixture is 200 steps:

| fixture | steps | residual |
|---|---|---|
| bowl-64 (the test's own fixture) | 200 | 0.09% |
| noise-64 | 200 | 0.04% |
| noise-128 | 400 | 0.14% |
| noise-128 | 1200 | 0.41% |
| **noise-1024 (production)** | **3000** | **4.59%** |

Growth is linear in step count. This is not a bad assertion — it is a good
assertion whose fixture never reaches the regime where the bug lives.

### 3.6 Dynamic time-stepping: article ≠ repo ⚪

The article says the constant time-step was replaced so a particle is guaranteed
to land in a *neighbouring* cell. The companion repo does **not** do this — it
still normalises to `sqrt(2)` (`water.h:105–108`, under a comment that already says
"Dynamic Time-Step"). We match the repo. The refinement lives in the author's
newer `soillib`, not in the code this port was made from.

Worth knowing before anyone "fixes" it to match the prose: at `sqrt(2)` a
particle can skip a cell diagonally, so tunnelling is real, but this is a
divergence from the *article*, not from our reference implementation.

### 3.7 Smaller items

- **Off-map handling.** The reference keeps eroding at the border
  (`h2 = height − 0.002`); we return early and book the load as lost. Off-map
  loss reaches 10.6% of initial mass — a large sink that deserves a look.
- **Initial terrain is pure fBm**, normalised to [0,1] × 900 m, with no base
  gradient and no sea level. Median slope is 18.2°; only 12.3% of the map is
  under 2°. Meandering is a low-gradient alluvial phenomenon — there may simply
  be very little terrain in the regime where it can occur.
- **Momentum is never dumped.** `Dump()` writes height/water/discharge/Q/soil.
  The article's headline field is invisible in our output, so nobody can look at
  the mechanism directly. This blocked quantifying 3.1 by measurement — the
  numbers above are analytic.

---

## 4. Does it meander? No.

New measurement (no such metric existed). Sinuosity = arc length / chord length.

Reaches in `rivers.bin` are cut at every confluence — median reach is 34 m, ~2
cells — so per-edge sinuosity is 1.000 by construction and tells you nothing.
Measured properly: chain edges into continuous downstream stems (following the
highest-flow successor), then slide a fixed-arc-length window.

| baseline | windows | median | p90 | p99 | >1.3 | >1.5 |
|---|---|---|---|---|---|---|
| 100 m | 11 641 | 1.053 | 1.203 | 1.444 | 3.5% | 0.6% |
| 250 m | 8 296 | 1.110 | 1.306 | 1.714 | 10.5% | 3.2% |
| 500 m | 6 353 | 1.167 | 1.450 | 1.986 | 23.1% | 7.8% |
| 1000 m | 4 388 | 1.243 | 1.586 | 2.113 | 40.3% | 15.1% |
| 2000 m | 2 432 | 1.331 | 1.668 | 1.943 | 55.5% | 24.5% |

At a 500 m baseline, sinuosity is flat across Strahler orders 3–8
(1.115 / 1.172 / 1.178 / 1.152 / 1.196 / 1.125) — **trunk rivers are no more
sinuous than headwater creeks.** Real meandering is strongly size-dependent.

**Sinuosity rising monotonically with baseline is the signature of a channel
following a winding valley, not of a channel meandering within one.** A true
meander train peaks at ~10–14 channel widths and is *more* sinuous than the
valley containing it. Ours does the opposite.

Visual confirmation (`/tmp/pg16/zoom0.png`, `zoom1.png`, crops on the two
largest trunks): on the low-gradient reaches the channels are **braided /
anastomosing** — multiple threads splitting and rejoining, with abrupt angular
turns. A handful of small loops appear, but there are no smooth bend trains, no
point bars, and no oxbows. The article's headline emergent results — meander
propagation, cutoff, oxbow lakes, meander scarring — are absent.

Braiding instead of meandering is exactly what 3.3 (no bank cohesion) predicts.

---

## 5. Candidate directions

Not decisions — input for the next conversation, cheapest first.

**Correctness (independent of meandering):**
1. Fix the concentration/mass leak (3.5), and extend the mass fixture to a
   production-length run so the assertion can see it.
2. Resolve the README-vs-run discrepancy so there is a trustworthy baseline.

**Meandering, in dependency order:**
3. Move `erf` out of the EMA (3.2). Cheap, and unblocks the next one.
4. Restore the raw discharge in the momentum denominator (3.1). Highest expected
   payoff; the current field is 37–3500× over-coupled.
5. Set `lrate` to the reference's 0.1, or record why 0.01 is right (3.4).
6. Consider bank cohesion (3.3) — either the reference's vegetation, or a
   lateral-cohesion term. This is an architectural addition, not a tuning knob,
   and needs its own design pass.
7. Ask whether the fBm initial terrain admits meandering at all (3.7), before
   tuning the sim to produce something the terrain forbids.

**Optimisation:** the particle pass is **96% of runtime** (357.8 s of 373 s).
Nothing else is worth optimising. The README already establishes that deferred
writes are not viable; the remaining option is the reference's racy in-place
parallelism, which costs reproducibility.

**Tooling gaps this exposed:** momentum is not dumped (3.7), and there was no
sinuosity metric. Any meandering work needs both to be steerable rather than
guesswork. The two scratch scripts used here are throwaway and not committed.
