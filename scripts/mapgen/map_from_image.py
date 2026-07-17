#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pillow", "scipy"]
# ///
"""Author the 2048x2048 map (biome texture + heightmap) from the reference hillshade.

The source (assets/map/reference_hillshade.png) is a NW-lit lidar-style hillshade, NOT a
DEM: brightness encodes slope/aspect, so heights must be synthesized, not read off.

Segmentation: water is automatic — dark AND flat (lake interior local-std ~0.2 vs
textured mountain shadow 14-23), which also self-excludes rivers (too narrow to pass the
flatness window). Everything else is hand-traced ZONE polygons: texture metrics measured
on this image separate neither mountain-vs-canopy nor canopy-vs-scrubby-plains, so the
polygons (refined against seg_overlay_478.png) are the authority.

Heights = LOW + DETAIL:
  - LOW: per-region base ramps from distance transforms (plains rise from the lake shore,
    each zone ramps toward its interior and is grounded at the waterline, bog nearly
    flat, lake bed a gentle bowl carved after the smoothing blur);
  - DETAIL: relief recovered from the shading itself by damped directional FFT inversion
    of the linearized hillshade equation I ≈ k*dh/du (u = the away-from-sun direction),
    amplitude set per biome so canopy stipple never imprints as real terrain.

Datum: 0 = lake water level (scripts/mapgen/biomes/README.md); lake bed < 0, swamp
straddles 0. Output is meter-true at 1 m/sample (mapgen kMetersPerSample). Deterministic.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

# ----------------------------------------------------------------------------- constants

SRC_SIZE = 478          # reference image edge, px
OUT_SIZE = 2048         # map edge, samples (= meters at 1 m/sample)
SCALE = OUT_SIZE / SRC_SIZE

# Biome indices — MUST match src/mapgen/biomes.hpp enum order.
LAKE, SWAMP, FOREST, PLAINS, HILLS, MOUNTAIN = range(6)
BIOME_NAMES = ["lake", "swamp", "forest", "plains", "hills", "mountain"]
PALETTE = np.array(
    [
        [40, 90, 160],    # Lake
        [74, 96, 70],     # Swamp
        [34, 110, 44],    # Forest
        [156, 178, 96],   # Plains
        [134, 112, 88],   # Hills
        [168, 168, 174],  # Mountain
    ],
    dtype=np.uint8,
)

# --- water segmentation (source-resolution pixels / 0-255 gray) ---
WATER_MAX_GRAY = 60.0    # lake interior measures ~38
WATER_MAX_STD = 4.0      # 5x5 local std; water ~0.2, textured shadow >= 14
WATER_STD_RADIUS = 2
MIN_LAKE_HOLE_PX = 200   # fill only small holes — a sealed-off peninsula must survive
MIN_POOL_PX = 6          # bog pools smaller than this are dropped
MIN_LOCH_PX = 25         # non-bog water bodies smaller than this are dropped
MIN_REGION_PX_2048 = 32  # final label cleanup: anything under 2 blocks is absorbed

# --- zone polygons (source px, x right / y down; traced on grid_ref, refined against
# seg_overlay_478). Painted in dict order (later wins), then water on top. ---
# name -> (biome, peak_m, ramp_m, vertices)
ZONES: dict[str, tuple[int, float, float, list[tuple[float, float]]]] = {
    # Dissected hills west of the river valley (top-left corner).
    "hills_nw_west": (HILLS, 55.0, 150.0,
                      [(0, 0), (85, 0), (120, 55), (110, 105), (75, 150),
                       (50, 165), (0, 180)]),
    # Dissected patch between the river plain and the NE massif.
    "hills_n_mid": (HILLS, 45.0, 120.0,
                    [(135, 0), (275, 0), (245, 55), (195, 95), (150, 60)]),
    # Rise south-east of the lake's SE lobe.
    "hills_se_knot": (HILLS, 45.0, 120.0,
                      [(295, 285), (370, 290), (380, 350), (330, 365), (300, 340)]),
    # Band along the east edge, south of the east outflow corridor.
    "hills_e_band": (HILLS, 40.0, 120.0,
                     [(430, 275), (478, 272), (478, 380), (435, 360)]),
    # Rugged bottom-right quadrant (east of the south river valley).
    "hills_sr": (HILLS, 50.0, 140.0,
                 [(300, 360), (380, 350), (435, 360), (478, 380), (478, 478),
                  (285, 478), (290, 430)]),
    # The NE massif, running down to the lake's north-east shore.
    "mountains_ne": (MOUNTAIN, 190.0, 260.0,
                     [(280, 0), (478, 0), (478, 245), (430, 240), (395, 185),
                      (340, 140), (300, 90), (280, 45)]),
    # Canopy-draped knoll field west of the lake.
    "forest_west_knolls": (FOREST, 8.0, 120.0,
                           [(35, 170), (150, 165), (150, 200), (140, 240),
                            (115, 265), (55, 272), (0, 268), (0, 205)]),
    # Wooded band along the lake's north shore.
    "forest_north_shore": (FOREST, 8.0, 120.0,
                           [(155, 108), (330, 112), (335, 142), (155, 132)]),
    # The wooded peninsula on the east shore.
    "forest_peninsula": (FOREST, 8.0, 120.0,
                         [(288, 232), (345, 238), (348, 268), (335, 292),
                          (300, 290), (285, 262)]),
    # Wooded knolls between the lake's south shore and the bog.
    "forest_south_band": (FOREST, 8.0, 120.0,
                          [(210, 318), (300, 325), (295, 380), (255, 400),
                           (215, 395), (205, 355)]),
    # Wooded valley floor along the south river, down to the map edge.
    "forest_south_valley": (FOREST, 8.0, 120.0,
                            [(205, 395), (280, 400), (285, 478), (210, 478)]),
    # The bog (user: bottom-left). Flat; pools stay Swamp, depressed in the heightmap.
    "bog_sw": (SWAMP, 0.0, 0.0,
               [(0, 252), (60, 250), (120, 258), (175, 290), (205, 320),
                (210, 355), (200, 400), (210, 430), (200, 478), (0, 478)]),
}

# --- LOW height synthesis (meters; distances in meters == 2048-res px) ---
PLAINS_BASE_M = 1.5
PLAINS_RISE_M = 18.5
PLAINS_RISE_DIST = 600.0
ZONE_RAMP_EXP = 1.3      # sstep^exp — keeps foothills gentle
BOG_BASE_M = 0.6
BOG_RISE_M = 1.0
BOG_RISE_DIST = 150.0
LOW_BLUR_M = 18.0
LAKE_DEPTH_M = 12.0
LAKE_DEPTH_DIST = 110.0  # shore -> full depth over this distance ("gentle slopes")
LAKE_BED_BLUR_M = 8.0    # softens the EDT medial-axis creases in the bowl
SHORE_BLEND_M = 60.0     # land band pulled smoothly toward the waterline
SHORE_MIN_LAND_M = 0.35  # land within 50 m of shore clamped at least this high
SHORE_MIN_LAND_DIST = 50.0
POOL_DEPTH_M = 0.45      # bog pools: shallow water per the swamp datum convention
POOL_BLEND_M = 8.0

# --- DETAIL recovery (source-resolution px unless noted) ---
DET_PRE_SIGMA = 1.6      # erases canopy stipple so trees do not become terrain
DET_HP_SIGMA = 24.0      # removes albedo/exposure low frequency (drift guard)
DET_PAD = 32             # mirror pad before FFT (kills wraparound seams)
DET_DAMP_WAVELEN = 90.0  # Wiener damping wavelength
DET_SUN_DIR = (1.0, 1.0)  # away-from-sun (source is NW-lit) in (x, y-down)
# Per-biome detail amplitude in meters (indexed by biome id). These are STARTING
# values: calibrate_amps() rescales each so our rendered shade's texture contrast per
# biome matches the source's contrast over the same pixels.
DET_AMP_M = np.array([0.0, 0.6, 3.5, 2.5, 10.0, 30.0], dtype=np.float32)
DET_AMP_BLUR_M = 15.0
DET_SHORE_TAPER_M = 30.0
CAL_CLASSES = (SWAMP, FOREST, PLAINS, HILLS, MOUNTAIN)
CAL_ITERS = 3
CAL_HP_SIGMA = 12.0      # compare texture contrast, not base shading level

# --- output encoding (fixed so reruns stay comparable) ---
HEIGHT_LO_M = -16.0
HEIGHT_HI_M = 240.0
CLAMP_LO_M = -14.0
CLAMP_HI_M = 235.0

# verification hillshade — hillshade.cpp's formula (alt 45, ambient 0.25, central
# differences), sun placed top-left in IMAGE coords to match the reference's lighting.
SHADE_AMBIENT = 0.25
SHADE_SUN_XY = (-1.0, -1.0)  # horizontal toward-sun direction, (x, y-down)

# ----------------------------------------------------------------------------- helpers


def sstep(x: np.ndarray | float) -> np.ndarray | float:
    """smoothstep of the already-normalized argument, clamped to [0,1]."""
    t = np.clip(x, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def disk(radius: int) -> np.ndarray:
    y, x = np.ogrid[-radius:radius + 1, -radius:radius + 1]
    return (x * x + y * y) <= radius * radius


def local_std(img: np.ndarray, radius: int) -> np.ndarray:
    size = 2 * radius + 1
    mean = ndimage.uniform_filter(img, size)
    mean_sq = ndimage.uniform_filter(img * img, size)
    return np.sqrt(np.maximum(mean_sq - mean * mean, 0.0))


def poly_mask(verts: list[tuple[float, float]], size: int, scale: float = 1.0) -> np.ndarray:
    im = Image.new("L", (size, size), 0)
    ImageDraw.Draw(im).polygon([(x * scale, y * scale) for x, y in verts], fill=1)
    return np.asarray(im, dtype=bool)


def fill_small_holes(mask: np.ndarray, max_px: int) -> np.ndarray:
    """Fill only enclosed holes below max_px (a sealed-off peninsula must survive)."""
    holes, n = ndimage.label(~mask)
    if n == 0:
        return mask
    sizes = ndimage.sum_labels(np.ones_like(holes), holes, index=np.arange(1, n + 1))
    border = np.zeros_like(mask)
    border[0, :] = border[-1, :] = border[:, 0] = border[:, -1] = True
    touches = np.zeros(n + 1, dtype=bool)
    touches[np.unique(holes[border])] = True
    fill_ids = [i for i in range(1, n + 1) if sizes[i - 1] < max_px and not touches[i]]
    out = mask.copy()
    for i in fill_ids:
        out[holes == i] = True
    return out


def absorb_small_regions(labels: np.ndarray, min_px: int) -> np.ndarray:
    """Merge connected same-class regions under min_px into their dominant neighbor."""
    out = labels.copy()
    for biome in range(len(BIOME_NAMES)):
        lab, n = ndimage.label(out == biome)
        if n == 0:
            continue
        sizes = ndimage.sum_labels(np.ones_like(lab), lab, index=np.arange(1, n + 1))
        for i in np.nonzero(sizes < min_px)[0] + 1:
            comp = lab == i
            ring = ndimage.binary_dilation(comp, disk(1)) & ~comp
            neighbors = out[ring]
            neighbors = neighbors[neighbors != biome]
            if neighbors.size:
                out[comp] = np.bincount(neighbors).argmax()
    return out


def upsample_labels(labels: np.ndarray, out_size: int) -> np.ndarray:
    """Bilinear one-hot upsample + argmax: smooth boundaries, no NN staircase."""
    zoom = out_size / labels.shape[0]
    stack = [
        ndimage.zoom((labels == b).astype(np.float32), zoom, order=1)
        for b in range(len(BIOME_NAMES))
    ]
    return np.argmax(np.stack(stack), axis=0).astype(np.uint8)


def upsample_mask(mask: np.ndarray, out_size: int) -> np.ndarray:
    zoom = out_size / mask.shape[0]
    return ndimage.zoom(mask.astype(np.float32), zoom, order=1) > 0.5


def hillshade(height: np.ndarray, mps: float) -> np.ndarray:
    """Replicates src/mapgen/hillshade.cpp (central differences, alt 45, ambient 0.25);
    the horizontal sun direction is SHADE_SUN_XY in image coords."""
    dhdx = np.empty_like(height)
    dhdy = np.empty_like(height)
    dhdx[:, 1:-1] = (height[:, 2:] - height[:, :-2]) / (2.0 * mps)
    dhdx[:, 0] = (height[:, 1] - height[:, 0]) / mps
    dhdx[:, -1] = (height[:, -1] - height[:, -2]) / mps
    dhdy[1:-1, :] = (height[2:, :] - height[:-2, :]) / (2.0 * mps)
    dhdy[0, :] = (height[1, :] - height[0, :]) / mps
    dhdy[-1, :] = (height[-1, :] - height[-2, :]) / mps

    sx, sy = SHADE_SUN_XY
    norm = np.hypot(sx, sy)
    alt = np.deg2rad(45.0)
    light = np.array([sx / norm * np.cos(alt), np.sin(alt), sy / norm * np.cos(alt)])
    inv_len = 1.0 / np.sqrt(dhdx * dhdx + 1.0 + dhdy * dhdy)
    diffuse = np.maximum((-dhdx * light[0] + light[1] - dhdy * light[2]) * inv_len, 0.0)
    return np.clip(SHADE_AMBIENT + (1.0 - SHADE_AMBIENT) * diffuse, 0.0, 1.0)


def save_gray(arr01: np.ndarray, path: Path) -> None:
    Image.fromarray((np.clip(arr01, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)).save(path)


# ------------------------------------------------------------------------------- stages


def load_gray(path: Path) -> np.ndarray:
    img = Image.open(path).convert("L")
    if img.size != (SRC_SIZE, SRC_SIZE):
        raise SystemExit(f"expected {SRC_SIZE}x{SRC_SIZE} source, got {img.size}")
    return np.asarray(img, dtype=np.float32)


def segment(gray: np.ndarray) -> dict[str, np.ndarray]:
    """Source-resolution masks + label image. Zones paint in dict order (later wins),
    water paints last."""
    zone_masks = {name: poly_mask(z[3], SRC_SIZE) for name, z in ZONES.items()}
    bog = zone_masks["bog_sw"]

    # Water: dark AND flat. Rivers (3-5 px) fail the 5x5 flatness window by themselves.
    water = (gray < WATER_MAX_GRAY) & (local_std(gray, WATER_STD_RADIUS) < WATER_MAX_STD)
    lab, n = ndimage.label(water)
    if n == 0:
        raise SystemExit("no water found — thresholds wrong for this source?")
    sizes = ndimage.sum_labels(np.ones_like(lab), lab, index=np.arange(1, n + 1))
    lake_id = int(np.argmax(sizes)) + 1
    lake = ndimage.binary_closing(lab == lake_id, disk(2))
    lake = fill_small_holes(lake, MIN_LAKE_HOLE_PX)

    pools = np.zeros_like(water)
    for i in range(1, n + 1):
        if i == lake_id:
            continue
        comp = lab == i
        in_bog = np.mean(bog[comp]) > 0.5
        if in_bog and sizes[i - 1] >= MIN_POOL_PX:
            pools |= comp
        elif not in_bog and sizes[i - 1] >= MIN_LOCH_PX:
            lake |= comp  # detached open-water body outside the bog

    labels = np.full(gray.shape, PLAINS, dtype=np.uint8)
    for name, (biome, _, _, _) in ZONES.items():
        labels[zone_masks[name]] = biome
    labels[lake] = LAKE

    return {"labels": labels, "lake": lake, "pools": pools,
            **{f"zone_{k}": v for k, v in zone_masks.items()}}


def synth_low(labels2048: np.ndarray, pools2048: np.ndarray) -> np.ndarray:
    """Region base ramps, blurred, then water carved AFTER the blur (crisp shoreline)."""
    lake = labels2048 == LAKE
    bog = labels2048 == SWAMP
    d_out = ndimage.distance_transform_edt(~lake)  # meters to shore, on land
    d_in = ndimage.distance_transform_edt(lake)    # meters to shore, in water

    h = PLAINS_BASE_M + PLAINS_RISE_M * sstep(d_out / PLAINS_RISE_DIST)
    for name, (_, peak, ramp, verts) in ZONES.items():
        if peak <= 0.0:
            continue
        # Grounded at the waterline: the ramp's distance field restarts at the shore,
        # so massifs rise from the lake instead of overhanging it with a collar.
        zone = poly_mask(verts, OUT_SIZE, SCALE) & ~lake
        d_zone = ndimage.distance_transform_edt(zone)
        h += peak * sstep(d_zone / ramp) ** ZONE_RAMP_EXP
    d_bog = ndimage.distance_transform_edt(bog)
    h = np.where(bog, BOG_BASE_M + BOG_RISE_M * sstep(d_bog / BOG_RISE_DIST), h)

    h = ndimage.gaussian_filter(h, LOW_BLUR_M)

    h_lake = ndimage.gaussian_filter(
        -LAKE_DEPTH_M * sstep(d_in / LAKE_DEPTH_DIST), LAKE_BED_BLUR_M)
    shore_pull = sstep(1.0 - d_out / SHORE_BLEND_M)  # 1 at waterline -> 0 at 25 m
    h = h * (1.0 - shore_pull) + h_lake * shore_pull
    h = np.where(lake, h_lake, h)
    near_shore_land = ~lake & (d_out < SHORE_MIN_LAND_DIST)
    h = np.where(near_shore_land, np.maximum(h, SHORE_MIN_LAND_M), h)

    d_pool_out = ndimage.distance_transform_edt(~pools2048)
    pool_pull = sstep(1.0 - d_pool_out / POOL_BLEND_M)
    h = h * (1.0 - pool_pull) + (-POOL_DEPTH_M) * pool_pull
    return h.astype(np.float32)


def recover_detail(gray: np.ndarray, norm_mask: np.ndarray) -> np.ndarray:
    """Damped directional FFT inversion of I ≈ k*dh/du. Returns unit-std relief
    (std measured over norm_mask) at source resolution."""
    ip = ndimage.gaussian_filter(gray / 255.0, DET_PRE_SIGMA)
    ihp = ip - ndimage.gaussian_filter(ip, DET_HP_SIGMA)
    padded = np.pad(ihp, DET_PAD, mode="reflect")

    n = padded.shape[0]
    fx = np.fft.fftfreq(n)[None, :] * 2.0 * np.pi
    fy = np.fft.fftfreq(n)[:, None] * 2.0 * np.pi
    ux, uy = DET_SUN_DIR
    un = np.hypot(ux, uy)
    d = 1j * (fx * ux / un + fy * uy / un)
    eps = 2.0 * np.pi / DET_DAMP_WAVELEN
    h = np.real(np.fft.ifft2(np.fft.fft2(padded) * np.conj(d) / (np.abs(d) ** 2 + eps ** 2)))

    h = h[DET_PAD:-DET_PAD, DET_PAD:-DET_PAD]
    h -= ndimage.gaussian_filter(h, DET_HP_SIGMA)
    std = float(np.std(h[norm_mask]))
    return (h / max(std, 1e-9)).astype(np.float32)


def compose(h_low: np.ndarray, det2048: np.ndarray, labels2048: np.ndarray,
            pools2048: np.ndarray, amps: np.ndarray) -> np.ndarray:
    lake = labels2048 == LAKE
    amp = amps[labels2048]
    amp = ndimage.gaussian_filter(amp, DET_AMP_BLUR_M)
    d_out = ndimage.distance_transform_edt(~lake)
    amp *= sstep(d_out / DET_SHORE_TAPER_M)  # 0 at (and inside) the waterline
    d_pool_out = ndimage.distance_transform_edt(~pools2048)
    amp *= sstep(d_pool_out / POOL_BLEND_M)
    return np.clip(h_low + amp * det2048, CLAMP_LO_M, CLAMP_HI_M).astype(np.float32)


def shade_at_source_res(height: np.ndarray) -> np.ndarray:
    """The reference is effectively a ~4.28 m/px hillshade, so shade a 478-sample
    downsample at that spacing — shading the 1 m grid and then downscaling would wash
    the relief out and any comparison against the source would lie."""
    h478 = np.asarray(Image.fromarray(height, mode="F").resize(
        (SRC_SIZE, SRC_SIZE), Image.LANCZOS))
    return hillshade(h478.astype(np.float64), SCALE)


def calibrate_amps(gray: np.ndarray, h_low: np.ndarray, det2048: np.ndarray,
                   labels2048: np.ndarray, pools2048: np.ndarray,
                   labels478: np.ndarray) -> np.ndarray:
    """Fixed-point rescale of the per-biome detail amplitudes: after each render, the
    high-passed shade contrast per biome is compared against the source's contrast over
    the same pixels and the amplitude scaled by the ratio. The shading is nonlinear
    (saturation), hence iterate with a damped update instead of solving once."""
    src_hp = gray / 255.0 - ndimage.gaussian_filter(gray / 255.0, CAL_HP_SIGMA)
    amps = DET_AMP_M.copy()
    for _ in range(CAL_ITERS):
        shade = shade_at_source_res(compose(h_low, det2048, labels2048, pools2048, amps))
        shade_hp = shade - ndimage.gaussian_filter(shade, CAL_HP_SIGMA)
        for c in CAL_CLASSES:
            mask = ndimage.binary_erosion(labels478 == c, disk(4))
            if int(mask.sum()) < 500:
                continue
            ratio = float(np.std(src_hp[mask]) / max(np.std(shade_hp[mask]), 1e-6))
            amps[c] *= float(np.clip(ratio, 0.5, 2.0))
    return amps


# ------------------------------------------------------------------------------ outputs


def write_outputs(out_dir: Path, height: np.ndarray, labels2048: np.ndarray,
                  src_path: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    v = (height - HEIGHT_LO_M) / (HEIGHT_HI_M - HEIGHT_LO_M) * 65535.0
    Image.fromarray(np.clip(v + 0.5, 0, 65535).astype(np.uint16)).save(
        out_dir / "heights_2048.png")
    Image.fromarray(labels2048, mode="L").save(out_dir / "biome_2048.png")
    Image.fromarray(PALETTE[labels2048]).save(out_dir / "biome_preview_2048.png")
    meta = {
        "width": OUT_SIZE,
        "height": OUT_SIZE,
        "meters_per_sample": 1.0,
        "height_min_m": HEIGHT_LO_M,
        "height_max_m": HEIGHT_HI_M,
        "water_level_m": 0.0,
        "encoding": "h_m = height_min_m + v/65535*(height_max_m-height_min_m)",
        "biomes": BIOME_NAMES,
        "source_image": src_path.name,
        "script": "scripts/mapgen/map_from_image.py",
    }
    (out_dir / "map_meta.json").write_text(json.dumps(meta, indent=2) + "\n")


def render_verify(verify_dir: Path, gray: np.ndarray, height: np.ndarray,
                  labels2048: np.ndarray, seg: dict[str, np.ndarray],
                  dump_stages: bool, stages: dict[str, np.ndarray]) -> None:
    verify_dir.mkdir(parents=True, exist_ok=True)

    save_gray(hillshade(height, 1.0), verify_dir / "hillshade_2048.png")

    # Side-by-side at source scale.
    shade478 = shade_at_source_res(height)
    gap = np.full((SRC_SIZE, 4), 255, dtype=np.uint8)
    Image.fromarray(np.hstack(
        [gray.astype(np.uint8), gap,
         (shade478 * 255.0 + 0.5).astype(np.uint8)])).save(verify_dir / "compare_478.png")

    # Segmentation overlay: class boundaries in palette colors + zone polygon outlines.
    overlay = np.stack([gray, gray, gray], axis=-1).astype(np.uint8)
    labels478 = seg["labels"]
    edge = np.zeros(labels478.shape, dtype=bool)
    edge[:, 1:] |= labels478[:, 1:] != labels478[:, :-1]
    edge[1:, :] |= labels478[1:, :] != labels478[:-1, :]
    overlay[edge] = PALETTE[labels478[edge]]
    im = Image.fromarray(overlay)
    draw = ImageDraw.Draw(im)
    for name, (_, _, _, verts) in ZONES.items():
        draw.line(verts + verts[:1], fill=(255, 80, 40), width=1)
    im.save(verify_dir / "seg_overlay_478.png")

    if dump_stages:
        for name, arr in stages.items():
            a = arr.astype(np.float32)
            lo, hi = float(a.min()), float(a.max())
            save_gray((a - lo) / max(hi - lo, 1e-9), verify_dir / f"stage_{name}.png")


# --------------------------------------------------------------------------------- main


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", type=Path, default=Path("assets/map/reference_hillshade.png"))
    ap.add_argument("--out", type=Path, default=Path("assets/map"))
    ap.add_argument("--verify-out", type=Path, default=Path("mapgen_out/map_from_image"))
    ap.add_argument("--dump-stages", action="store_true")
    args = ap.parse_args()

    gray = load_gray(args.src)
    seg = segment(gray)

    labels2048 = upsample_labels(seg["labels"], OUT_SIZE)
    labels2048 = absorb_small_regions(labels2048, MIN_REGION_PX_2048)
    pools2048 = upsample_mask(seg["pools"], OUT_SIZE)

    h_low = synth_low(labels2048, pools2048)
    det = recover_detail(gray, seg["zone_mountains_ne"])
    det2048 = ndimage.zoom(det, OUT_SIZE / SRC_SIZE, order=3)
    amps = calibrate_amps(gray, h_low, det2048, labels2048, pools2048, seg["labels"])
    for c in CAL_CLASSES:
        print(f"detail amp {BIOME_NAMES[c]:9s} {DET_AMP_M[c]:5.1f} -> {amps[c]:5.1f} m")
    height = compose(h_low, det2048, labels2048, pools2048, amps)

    write_outputs(args.out, height, labels2048, args.src)
    render_verify(args.verify_out, gray, height, labels2048, seg, args.dump_stages,
                  {"water": seg["lake"], "pools": seg["pools"],
                   "h_low": h_low, "h_det": det})

    print(f"height  min {height.min():7.2f} m   max {height.max():7.2f} m")
    counts = np.bincount(labels2048.ravel(), minlength=len(BIOME_NAMES))
    for name, c in zip(BIOME_NAMES, counts):
        print(f"{name:9s} {100.0 * c / labels2048.size:5.1f} %")
    print(f"outputs -> {args.out}   verify -> {args.verify_out}")


if __name__ == "__main__":
    main()
