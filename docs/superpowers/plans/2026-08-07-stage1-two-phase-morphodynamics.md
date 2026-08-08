# Stage 1 rework: two-phase morphodynamics (Lagrangian carve + Eulerian SWE/Exner)

## Context

Stage 1 (`tools/protogen/`, the coarse hydraulic erosion sim) is producing disappointing terrain: no proper rivers forming in valleys and joining into larger structures — scattered noisy puddles instead. A diagnostic run this session (8 km / 512², 3000×1024, seed 1) measured why:

- The sim ends with **9,215 labelled lakes, 25.35% wet**; the standing-water solver over-ponds ~2.7× what the terrain can hold (priority-flood says max 9.14% wet) and its surplus routing failed to settle (2M rounds).
- The particle pass **manufactures the pit field**: 1,117 depressions in the initial fBm → **13,956 after erosion**, while relief never moves off 329.2 m. Dominant mechanism: the terminal travel-budget exit at `protogen.cpp:2026` dumps each particle's full remaining load into one cell (~40% of all deposited mass).
- The extracted river graph itself is fine (proper tree, 10,595 nodes) — the failure is bed incision + water representation, not topology.

Approved design (brainstormed section-by-section with Jakub):

- **Phase 0 — Lagrangian carving** (existing `Descend`+`Cascade`, serial, transport law verbatim, `dt_years` clock). Purpose: valleys, ridges, mountain mass. Terminal dump redirected into a new suspended-sediment field `sus`; one Jacobi settle pass per landscape step. **All standing-water/lake machinery deleted** (~1,300 lines + 22 tests).
- **Phase 1 — Eulerian staggered morphodynamics** (new): virtual-pipes SWE + Exner with MORFAC. **One water representation** — depth field; a lake is deep+slow, a river shallow+fast, and the sim never asks which. GPU-shaped by design: every pass a gather-only Jacobi stencil over ping-pong buffers, no atomics; the pass list is the frozen future GPU dispatch list. CPU now (Taskflow `ParallelFor`), quality before performance — no runtime budget.
- Capacity law: **Mei form on the energy slope** — `C = Kc·|∇(bed+h)|·|v|` (unconditionally stable under MORFAC; zeroes in lakes via both factors; energy slope fixes flat-reach terracing). Exchange relaxed over `adaptation_length`; bed delta ×MORFAC in exactly one place; soil-first substrate; transverse-bed-slope deflection of sediment flux.
- `swe_flux` carries **implicit Manning drag** (same quadratic solve as `Descend`'s drag at `protogen.cpp:1830`) so velocity has a terminal value.
- **Warm start**: priority-flood lake prefill + Manning channel depths from D8 discharge (dry start at real rain ≈ 1e8 substeps — rejected). Rain runs at the real `runoff_m_per_yr`.
- **Output boundary (user decision):** `water.f32` KEEPS standing-water-only semantics — a boundary-only classification (seed deep+slow, grow across the wet component so whole lakes incl. margins are tagged, per `route_flow`'s lake_tag contract `src/mapgen/hydrology.hpp:53-64`) writes it and feeds `lake_tag`. Full SWE state goes to NEW `depth.f32` / `vel.f32` rasters nothing existing reads (verified: patch source globs `*-height.f32` only; `coarse_io` parser ignores unknown keys — pinned by `src/mapgen/coarse_io_tests.cpp:71`). Rivers keep flowing through `rivers.bin`, chain unchanged. This averts 7 verified downstream breakages (ReconstructWater's flat-surface-per-component, biome flips, relief-filter kill, soil-cutoff shift, select.cpp gate inversion, rivers.bin collapse, flat-slab river rendering).
- **Perf data**: per-pass wall-clock accumulators; end-of-run table (total, mean/cycle, cell-updates/sec) printed + written to `perf.txt`. Failure = fail loudly: CFL reduction doubles as NaN/Inf tripwire; dt-floor abort; periodic **mass tripwire** (every ~10 cycles, Σ(bedrock+soil+sus) vs the phase-0 baseline net of ledgered border export — drift > ε aborts loudly, catching ghost sediment under MORFAC early); never write garbage artifacts.
- Depression count + drainage connectivity are **printed diagnostics, not asserts** — methods get tests, Jakub judges the landscape by eye.

No spec doc was written (user: "start planning directly") — this plan is the design record. At execution start, copy it to `docs/superpowers/plans/2026-08-07-stage1-two-phase-morphodynamics.md` and commit.

**Build line** (current, from `tools/protogen/README.md`; Task 2 adds the two new TUs):

```sh
cd /Users/jakub/repos/badlands-clone-2 && c++ -O3 -std=c++23 \
  -I src -I third_party/FastNoiseLite -I third_party/glm -I build/_deps/taskflow-src \
  tools/protogen/protogen.cpp src/core/parallel.cpp \
  src/mapgen/hydrology.cpp src/mapgen/river_graph.cpp src/mapgen/river_prune.cpp \
  src/mapgen/coarse_io.cpp src/mapgen/river_io.cpp -o /tmp/protogen
```

**Byte-compare fixture** (Tasks 1–2 gates): `--res 256 --world 4096 --steps 300 --drops 256 --seed 7 --snapshot-every 300`.

Ordering note: retirement runs BEFORE the file split so the audited line numbers below stay valid and `protogen.hpp` is written once against post-retirement types.

---

## Task 1: Retire the standing-water/lake machinery (bit-identical gate)

Delete all lake/water code from phase 0; prove surviving physics byte-identical to today's water-disabled behavior. Files: `tools/protogen/protogen.cpp` only.

1. **Baseline first**: add temporary `--no-water` flag (`enable_water=false, enable_lake_deposit=false, enable_closing_relax=false`), build, run fixture → `/tmp/pg-base`. Flag dies at end of task. A default-flags baseline will NOT match — water was on and moved sediment.
2. **Delete 22 tests + 2 helpers**: LakeFlatness 2875, LakeIdValid 2908, LakeAreaBounded 2936, DeltaProfile 3243, S1 3325, S2 3393, LakeLevelSolvesBalance 3784, WaterFieldZeroLossLimit 3845, SpillRoutesDownstream 3886, EvaporationEquilibrium 3961, SeaLevelFillsNotDrains 4020, WaterBoundaryConditions 4052, WaterDoesNotAccumulate 4090, WaterDrainsOffTheMap 4265, WaterConversionScales 4288, QPerUnitVol/WorstVsOracle 4339-4354, W1–W10 4363-4828. Surviving-test edits: KnobLiveness drops settling_velocity/plume_wander_deg/evaporation rows (2846-2856); VolumeDiscretizationInvariance drops `deposited_lake` term (3520); T1/T3 drop `enable_lake_deposit`; `Isolated()` drops retired toggles; `test::Run`/`RunSim` lose the `lakes` param.
3. **Delete machinery**: solver block (SolveWaterEquilibrium 766-1065, WaterLedger 1067, RelaxWaterStep 1076-1280, HarvestSpill 1290, Lake 1336, LabelWater 1353, CullUnsupportedWater 1422, TrimBodiesToSills 1474, SampleWater 1567); RunSim sites (sea priming 2274-2278, re-emission Descends 2342-2364, water block 2419-2429, closing relax 2468-2482); Descend branches (convert_water lambda 1625-1632 + 5 call sites, lake-exit 1653-1660, contact conversion 1669-1679, plume block 1680-1758, backwater reads 1954-1959 become bed-only); Grid water fields (lines 400, 409, 411, 424-425, 432, 437-438, 443-444, 448, 452, 456); Params water knobs + CLI flags + banner (see exploration list: enable_lake_deposit, enable_water, test_inflow_*, disperse, settling_velocity, plume_velocity, lake_deposit_length_m, plume_wander_deg, min_dispersion_depth_m, sea_level_m, relax_iters, relax_dt_yr, enable_closing_relax). **Retain**: `evaporation_m_per_yr` (+flag; future `swe_depth` consumer — comment it, drop its KnobLiveness row until Task 4), `channel_width_coeff` (dry-walk Manning 1783), `runoff_m_per_yr`, `PriorityFlood` 686-722 (test oracle only — already has no sim callers).
4. **Keep the contract alive**: `Dump` writes all-zeros `tag-water.f32`; `WriteWorldArtifacts`/`SoilCutoffs`/`RunExtractRivers` run against zeros (0-lake case already handled). `deposited_lake` leaves the ledger print; the ledger itself stays.
5. SimStats: keep `t_drops`/`t_grid`, drop lake bookkeeping; fix step + timing prints. Drop `rng` from `Descend` if the wander cone was its last use.

**Verify**: build; `/tmp/protogen --test` all green; run fixture (default flags) → `/tmp/pg-new`; `cmp` height and soil `.f32` vs `/tmp/pg-base`.

## Task 2: Mechanical file split

`protogen.hpp` (named namespace: `Grid`, `Params`, `SimStats`, constants, decls the test TU names — export deliberately, others stay `static`; ODR is the one hazard) + `protogen_tests.cpp` (whole `namespace test`) + `protogen_swe.cpp` (stub, so the build line is final from here) + README build-line update. Pure code motion, no test changes.

**Verify**: build 3-TU line; `--test` green; re-run fixture, `cmp` vs Task 1's `/tmp/pg-new`.

## Task 3: `sus` field + terminal-exit redirect + settle pass

Files: `protogen.hpp` (Grid: `sus`/`sus_b`; Params: settle fraction, diffusion share; SimStats: `t_settle`), `protogen.cpp`, `protogen_tests.cpp`.

**Tests first**: `TerminalLoadInjection` (single-source fixture, particle dies mid-slope → no one-cell mound/pit at death cell; ledger closes incl. `sus`); extend MassConservation/MassConservationLongRun/VolumeDiscretizationInvariance ledgers with `Σ sus`; KnobLiveness rows for settle knobs.

**Impl**: terminal exit 2026 → `g.sus[last_cell] += carried_mass()` (repurpose `deposited_death` accounting as `injected_sus`); `min_vol` exit stays a bed Deposit (unreachable in practice per comment 2021-2024). `SettleSus`: ParallelFor gather-only Jacobi, fraction settles onto bed (building soil), remainder diffuses to 4-neighbours; called per step next to `Diffuse` (2411), timed `t_settle`. CLI + banner.

**Verify**: `--test`; small run + `python3 tools/protogen/show.py` — valley-floor pit speckle should visibly drop. Output intentionally not byte-identical from here.

## Task 4: SWE fluid passes, cycle skeleton, control + tripwires

Files: `protogen_swe.cpp` (passes + `RunSweCycles`), `protogen.hpp` (state `h`, per-face `flux[4]` ping-pong, `vel`; Params: substeps=50 default, CFL number, Manning n, dt floor, wet threshold `ε_wet`; timers), `protogen_tests.cpp`.

**Tests first**: `SweWellBalancedness` (prefilled still lake, no rain → zero flux, bit-still); `SweFillOracle` (rain over fixed bowl → PriorityFlood spill surface within tol); `SweWaterLedger` (rain − evap − border outflow = Δstorage); `SweDeterminism` (bit-exact twice — pins fixed-order chunked CFL max-reduction); `SweTripwire` (seeded NaN + forced sub-floor dt → loud abort path + snapshot, no crash); `SweWetDryFrontStability` (cliff fixture, deep cell bordering a bone-dry cell: no NaN/Inf in vel, per-substep outflow ≤ cell volume); `SweLakeMomentumDissipation` (fast inflow into a deep static lake: velocity decays geometrically into the lake body, no jet shooting across the surface); `SweManningConvergence` (uniform incline + constant rain: h and v asymptote to the analytic Manning steady state for that slope and roughness).

**Impl**: `swe_flux` — face flux from `bed+h` head differences, export clamp (cell can't outflow more than it holds), implicit Manning drag (quadratic solve per `protogen.cpp:1830` pattern), **wet-dry front handling**: flux only computed where donor `h > ε_wet`, and the velocity derivation guards `h → 0` — a dry cell bordering a wet one must not synthesize a pressure spike or a divide-by-zero; `swe_depth` — divergence + rain (real rate) − evaporation (`evaporation_m_per_yr` regains its consumer; restore KnobLiveness row); open border outflow at base level; `swe_velocity` — from fluxes and depth. `RunSweCycles`: per cycle CFL dt (`0.5·Δx/√(g·h_max)`, deterministic fixed-order max), N substeps, morpho hook empty, MORFAC clamp scaffolding inert. All passes ParallelFor gather-only Jacobi — keep the pass list a literal list of functions (the frozen dispatch list).

**Verify**: build + `--test`.

## Task 5: Warm start (phase-0 → phase-1 handoff)

Files: `protogen_swe.cpp`, `protogen_tests.cpp`.

**Tests first**: `WarmStartLakeLevel` (basin `bed+h` equals PriorityFlood spill surface); `WarmStartProximity` (carved-valley fixture, k fluid-only cycles → small L∞ water-surface drift).

**Impl**: PriorityFlood on final phase-0 bed → `h` in depressions; `route_flow`+`accumulate_drainage` (already linked) → D8 discharge → Manning depth per channel cell; fluxes start zero (safe from well-balanced start).

**RATIFIED DEVIATION (Jakub, post-review):** the Manning conveyance width is **`cell_m`, not Descend's regime width** `channel_width_coeff·√Q`. The SWE solver has no sub-grid channel — its flux conveyance is `A_pipe = cell_m·h`, so its steady depth at discharge Q is algebraically `(n·(Q/cell_m)/√S)^(3/5)`; seeding the regime depth (calibrated for a channel narrower than the cell) injects a volume the solver immediately collapses, failing the WarmStartProximity gate 4× over (measured 0.1229 m vs bound 0.03 m; cell_m gives 0.0154 m). Descend's regime width is untouched everywhere else.

**Verify**: build + `--test`.

## Task 6: Morpho passes — sed_exchange, sed_advect, talus pair

Files: `protogen_swe.cpp`, `protogen.hpp` (Params: `Kc`, K_erode/K_deposit, adaptation-length reuse, transverse-slope coeff, morfac, clamp fraction), `protogen_tests.cpp`.

**Tests first**: `FlatReachTransport` (flowing water over flat bed: C>0 — pins the energy-slope amendment); `StillLakeInert`; `GeologyDeltaFormation` (rebuild S1's fixture on SWE — sediment-rich river into a static lake: deposition strictly concentrated at the inlet mound, zero at lake center and far edges); `ExnerLedger` (Δbed+Δsus+border export = 0); `ExnerCapacityLimiter` (river initialized with `sus` exactly = C: erosion identically zero, deposition exactly zero, sediment purely advects); `ExnerSoilBedrockConservation` (0.1 m soil on bedrock, 0.5 m erosion demand, erodibility 0.1: soil fully removed, remaining 0.4 m demand ×0.1 takes exactly 0.04 m bedrock, `sus` gains exactly 0.14 m — the soil→bedrock transition is mass-conservative by construction); `MorphoTransverseSlopeDeflection` (straight river down Y with a slight X-tilt: sediment flux deflects toward the lower bank, asymmetric deposition — the mechanism that seeds meandering); `GeologyKnickpointRetreat` (river over a sudden step: the point of maximum erosion migrates upstream over cycles — proves the energy slope drives incision); `GeologyNoBottomlessPits` (localized high-velocity spot at M=10,000: eroded depth stays bounded by base level + transport capacity, no single-pixel drill); `NoRunaway` (max bed change/cycle under clamp, no NaN); `TalusRepose` (post-pass slopes ≤ repose); `MorfacInvariance` (halve M, double cycles → same landscape within tol); `SubstepSufficiency` (double N → below-tol change); `SweResolutionInvariance`; KnobLiveness rows for every new knob.

**Impl**: `sed_exchange` — `C = Kc·S_energy·|v|`, exchange over adaptation length, bed delta ×MORFAC (only here), soil-first then bedrock ×`bedrock_erodibility` with the residual demand scaled *before* it attacks bedrock (mass-conservative split, pinned by `ExnerSoilBedrockConservation`), deposition builds soil, transverse-slope deflection; `sed_advect` — semi-Lagrangian backtrace of `sus` along `vel` with a **high-order interpolator (Catmull-Rom bicubic)** — bilinear backtrace is dissipative enough to smear banks into mud-slides; `talus_flux`+`talus_apply` — Jacobi pair, phase-0 repose angle. Activate MORFAC clamp in `RunSweCycles`; morpho ×1 after substeps; activate the periodic **mass tripwire** (every ~10 cycles: Σ(bedrock+soil+sus) vs phase-0 baseline net of ledgered exports, drift > ε → loud abort).

**RATIFIED (Jakub, post-review): MORFAC applies to the erosion demand BEFORE the substrate split.** `yield(M·D)` is the exact aggregation of M sequential fluid intervals over a depleting soil column (the substrate law is additive); the plan's literal split-then-×M order can demand M× more soil than exists and silently erode bedrock at full rate. Conditions: `MorfacAggregationIdentity` (1 cycle at M=100 ≡ 100 cycles at M=1 on deep uniform soil, unclamped), `MorfacMantleTransition` (0.5 m soil, erodibility 0.1, D=0.01 m, one M=300 cycle → soil exactly 0, bedrock eroded exactly 0.25 m, with in-test comments documenting that the fluid did not update when it hit bedrock), and a documented tripwire forbidding soil-production/weathering interleaved with M>1 cycles (additivity breaks under weathering — heavy in-code documentation required).

**AMENDMENT (Jakub, ruling on the Task-5 width deviation):** accepting that the SWE solver sees every channel as `cell_m`-wide sheet flow changes the transport regime — the SWE velocity over a channel cell is wider-and-slower than the sub-grid channel that carved it, so a naive `C ∝ |v|` both under-erodes in-channel and unnaturally widens rivers. `sed_exchange` MUST carry a **sub-grid channelization factor** reconciling the two: default formulation scales the erosion/deposition exchange by the fractional wet width `min(1, w_regime/cell_m)` with `w_regime = channel_width_coeff·√Q_local` (Q_local from the cell's face fluxes); an equivalent conveyance-correction on the effective stress/velocity is acceptable if better justified in-code. **Do not handwave the stage-0→stage-1 transition: a mandatory test `ChannelPersistence` must prove a carved steady-state channel does not disintegrate under the SWE morpho pass** (channel cross-section/width stays bounded over cycles; the test must FAIL with the channelization factor removed).

**Verify**: build + `--test` (keep fixtures 32-64 cells — the invariance tests are the suite-runtime risk; shrink fixtures before weakening tolerances).

## Task 7: Driver, output boundary, perf, show.py

Files: `protogen.cpp` (main, Dump, WriteWorldArtifacts, SoilCutoffs, RunExtractRivers), `protogen_swe.cpp` (classification), `protogen_tests.cpp`, `tools/protogen/show.py`, `src/mapgen/coarse_io.{hpp,cpp}` + `coarse_io_tests.cpp` (three optional provenance fields).

**Tests first**: `BoundaryClassification` (channel-into-lake fixture: whole lake incl. margins tagged, channel not); KnobLiveness rows `--cycles`/`--substeps`.

**Impl**: (a) main: phase 0 → warm start → `RunSweCycles(--cycles)`; tripwire abort dumps final snapshot, exits non-zero. (b) Dump: NEW `tag-depth.f32` (h), `tag-vel.f32` (speed); `tag-water.f32` = classification output only (replaces zeros). (c) WriteWorldArtifacts: surface = bed + lake-only water, `lake_tag = water>0`, rivers.bin chain unchanged; `CoarseManifest` gains optional `morfac`/`cycles`/`substeps` (parser already ignores unknowns; extend struct+writer+parser+one test row); `--extract-rivers` keeps working. (d) Perf: per-pass accumulators; table (total, mean/cycle, cell-updates/sec) printed + `perf.txt`; depression count + drainage connectivity printed diagnostics. (e) show.py: additive depth/vel rendering.

**Verify**: `--test`; 8 km run (`--res 512 --world 8192 --steps 3000 --drops 1024 --cycles 200 --snapshot-every 750`) + `show.py` + `--extract-rivers` + `ls perf.txt`.

## Task 8: Full-run validation + README rewrite

8 km and 16 km (`--res 1024 --world 16384 --steps 3000 --drops 4096`) runs; termination judged from snapshots (quality-first); tripwires must not fire; `select.cpp` against the 16 km dump (plains/channel gates should improve on lake-only water); depression-count diagnostic before/after phase 1. README: pipeline description, results + perf tables, findings update (delete stale lake findings, keep true phase-0 ones, note the 308 m bathymetry item's fate). Any defect found → regression test in Tasks 3-7 files BEFORE the fix. Jakub judges the visual result from `show.py` renders — no extra tooling.

---

## Cross-cutting risks

1. Task 1's bit-gate is only valid against the `--no-water` baseline.
2. Interim `water.f32` must exist as zeros through Tasks 1–6 or Dump/WriteWorldArtifacts/RunExtractRivers/show.py/select.cpp break mid-sequence.
3. Split ODR hazard: export only what the test TU names; rest stays `static`.
4. MorfacInvariance/SubstepSufficiency dominate suite runtime — fixture size is the lever, not tolerance.
5. `evaporation_m_per_yr` is consumer-less between Tasks 1 and 4 — KnobLiveness row out in Task 1, back in Task 4.

## Verification (end-to-end)

- Every task: 3-TU build line + `/tmp/protogen --test` green before moving on.
- Tasks 1-2: byte-compare gates (`cmp` on height/soil rasters).
- Task 7 onward: 8 km run renders via `show.py`; `--extract-rivers` round-trip; `perf.txt` present.
- Task 8: 16 km run; `select.cpp` gates; README tables from measured data; final visual judgment by Jakub.
