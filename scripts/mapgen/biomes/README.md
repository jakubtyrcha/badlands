# Biome terrain scripts

One script per biome *variant*. Each is a candidate for what a region of that biome
looks like; `badlands_patchgen` renders any of them over a world patch so they can be
compared by eye.

## The contract

A script is a **pure function of world position in METERS**, returning **one `f32`:
height in meters**.

```
badlands_patchgen --script scripts/mapgen/biomes/hills_ridged_fbm.noiser \
                  --out /tmp/patches --extent 2000 --res 512 --seed 2
```

`--extent` is how much world to look at (meters); `--res` is how many samples to look
with. `meters_per_sample = extent/res` follows. The two standard views:

| View | Flags | m/sample | Judges |
|---|---|---|---|
| structure | `--extent 2000 --res 512` | ~3.9 | large-scale organisation |
| detail | `--extent 512 --res 512` | 1.0 | the map's real extent + density |

Sampling coarser than the signal aliases (Nyquist = 2×mps), so confirm anything
sub-8 m in the detail view.

### Height 0 is the water datum

Not an arbitrary origin — **0 means water level**. So:

- **hills / forest / plains** sit above 0.
- **lake** is the lake *bottom*: below 0, rising to ~0 at its coast.
- **swamp** straddles 0: slightly below = mud under shallow water, above = islands and
  the corridors between them.

Water surface geometry and material are a separate, later system. These scripts only
say where the ground is.

### Frequency is in METERS, not cycles

Every scale knob is a **wavelength in meters** (`*_wavelength_m`), never "cycles across
the map". This is the whole reason a preview means anything: the script produces the same
terrain at that spot regardless of map size, patch extent, or sample density, so a preview
is a literal crop of the world, and a 512 m map is a representative sample of a 2 km one.
Widening `--extent` shows *more world at the same terrain* — never different terrain.

The host supplies the placement; the script turns it into meters:

```noiser
@uni.origin_x: f32 = 0.0;           // patch origin in the world, meters
@uni.origin_z: f32 = 0.0;
@uni.meters_per_sample: f32 = 1.0;  // sample density; do not assume 1
@uni.seed: f32 = 1.0;

let wx = (f32(@warpId.0) + 0.5) * @uni.meters_per_sample + @uni.origin_x;
let wz = (f32(@warpId.1) + 0.5) * @uni.meters_per_sample + @uni.origin_z;
```

Then a wavelength divides the domain, and the generator's own `freq` stays at 1.0:

```noiser
let p = (wx / wl_m, wz / wl_m, 0.0);
Perlin3d { seed: s, freq: 1.0 }.at(p)     // one cycle per wl_m meters
```

### Scope: low/medium frequency only

These describe terrain shape at ~1 m samples. **Microdetail is out of scope** — a later
tessellation/detail system owns anything finer. Wavelengths below a few meters here are
wasted work.

### Shape: one `height_m` function

Each script is a `fn height_m(wx, wz, seed) -> f32` plus a thin tail that calls it. The
tail is only there so `patchgen` can evaluate the file directly; keeping the body in one
named function is what lets the chosen variants be composed into the real pipeline later
without rewriting them.

## noiser traps (learned the hard way — see the language book)

- **Do NOT `import { smoothstep, step, remap } from core::math;`** Those names also exist
  in the auto-imported prelude with **different argument orders** (`smoothstep(x, e0, e1)`
  vs GLSL's `smoothstep(e0, e1, x)`). Both compile. Results differ silently. Use the
  prelude UFCS form: `x.smoothstep(e0, e1)`.
- `if` branches may not be empty or comment-only — every branch must yield a value.
- `Cellular` and `PingPong` have **no** `.gradient()` / `.curl()`.
- `.gradient()` is numerical (2N noise samples) — expensive per pixel.
- Use `while`, not recursion.
