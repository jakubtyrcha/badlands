#!/usr/bin/env python3
"""Turn a pair of rendered map plaques into the authored map's data textures.

The renders are pictures OF a map — a plaque on a wall, with a title strip, a legend,
a scale bar and a compass rose. This strips all of that and emits the two rasters the
engine actually consumes, plus the metadata needed to read them back:

    assets/map/heights_2048.png   2048^2, 16-bit gray, affine-decoded to meters
    assets/map/biome_2048.png     2048^2, 8-bit gray, RAW Biome enum values
    assets/map/map_meta.json      dims + the height affine + the biome name order

Run from the repo root:  python3 scripts/mapgen/map_from_render.py

Three things here are load-bearing and were measured, not guessed:

1. HEIGHTS RESAMPLE IN FLOAT. PIL's resize on an 8-bit 'L' image returns 8-bit: it
   re-quantizes the interpolation away, and every filter collapses back to the source's
   256 levels. Resampling via mode 'F' is what produces the ~244k distinct levels that
   make the 16-bit output worth storing at all. Get this wrong and the whole 16-bit
   path downstream is decorative.

2. LANCZOS, THEN CLAMP. Measured on this crop, Lanczos leaves the fewest quantization
   plateaus (flat-3x3 0.6%, vs bicubic 1.0% / bilinear 1.5% / nearest 2.6%) — it is the
   filter that best dissolves the visible banding at the lake border. It pays for that
   with ringing at sharp steps: ~5.5k px land outside the source's true 0..255, dipping
   to -12 m, which would carve a trench ring around the shoreline. Those values are
   non-physical by construction (the source IS 0..255), so clamping is a restoration,
   not a loss.

3. BIOME UPSAMPLES NEAREST, AFTER CLASSIFYING. A label is not a number: interpolating
   between Forest(2) and Lake(0) invents Swamp(1) along every shoreline. Classify the
   RGB to enum values first, then upsample the labels with NEAREST.

The altitude render is an ALTITUDE MAP WITH HILLSHADE BAKED IN (the NW-SE gradient skew
is +0.71 vs +0.18 NE-SW), so ridges carry a directional bias and water is drawn 40-77
levels below its surroundings. Both are accepted: this is a generated impression, not a
DEM, and de-shading it was the approach that got archived.
"""

import json
import os
import sys

import numpy as np
from PIL import Image

# --- The source renders (outside the repo; they are inputs, not assets) -------------
ALT_SRC = os.path.expanduser("~/repos/Gemini_Generated_Image_psmcyupsmcyupsmc.png")
BIOME_SRC = os.path.expanduser("~/repos/Gemini_Generated_Image_2vn8xl2vn8xl2vn8.png")

# Crop window, in RAW SOURCE-IMAGE pixels (top-left origin). Chosen to sit clear of the
# plaque furniture: the compass rose (x~2420) and the legend/scale strip (y>1460) both
# fall outside it, so there is no decoration to scrub out of the data.
CROP_X, CROP_Y = 628, 241
CROP_SIZE = 1024
OUT_SIZE = 2048  # 2x upsample -> 1 source px = 2 world m

# Biome height tiers: each biome's terrain is remapped so its high end reaches `ceil_m`,
# via a single GLOBAL monotonic height curve (tiered_remap) rather than per-biome. Global
# is what keeps it continuous -- same input height maps to the same output everywhere, so
# there are no cliffs at biome borders and the elevation order (lowland < forest < hills <
# mountain) is preserved. Per-biome normalisation would instead invert the order at the
# forest/hills seam and cliff there.
#
# Each tier's rise is out = floor + span * ease(t), where
#   ease(t) = MIX*t + (1-MIX)*t**exp
# a BLEND of linear and power, not a pure power. The power term pushes the bulk of a
# highland tier toward its floor (more playable area, decorative peaks); the linear term
# gives the curve a nonzero slope at t=0 so the tier rises immediately instead of sitting
# dead-flat at its floor. A pure power (t**5) has zero slope there, which piled terrain
# into visible flat TERRACES at the tier boundaries (20 m, 32 m) -- the linear mix is what
# removes them while keeping the push-low shape (modelled: 47% flat -> 0%).
#
# `anchor_pct`: the input percentile that maps to `ceil_m`. The top tier uses ~p99.8, not
# p97, so only the genuine peak reaches 128 -- anchoring lower would hard-clamp the top
# few % of mountains to exactly 128 and flat-top them (the other terrace).
EASE_MIX = 0.3
BIOME_TIERS = [
    # name        ceil_m  exp  anchor_pct   (floor = previous ceiling; lowland floor = 0)
    ("lowland",     4.0,   1.0, 97.0),  # plains + swamp: flat, linear
    ("forest",     20.0,   3.0, 97.0),  # partly playable
    ("hills",      32.0,   3.0, 97.0),
    ("mountain",   128.0,  3.0, 99.8),  # near-max anchor so peaks aren't a flat mesa
]
SWAMP_FLOOR_M = -0.5  # swamp dips just below the plains 0 m datum (wetland)

# Smoothing sigma, in world meters (= output samples). NOT cosmetic — this is what makes
# the result terrain rather than crumpled foil.
#
# The render's fine texture is SHADING, not elevation: adjacent source pixels differ by
# 10-20 luminance = 11-22 m of height across only 2 m of ground. Mapped 1:1 that is a
# cliff at every pixel. Measured on this crop, median slope by sigma:
#
#     sigma:   0(none)   1 m    2 m    3 m    4 m    6 m
#     p50:      47.2    33.0   24.6   23.8   23.6   22.9   degrees
#     >45deg:   54.6%   28.6%  22.4%  21.6%  20.5%  17.4%
#
# 2 m is the knee: it blurs across ~1 source pixel, so it erases the pixel-scale emboss
# and keeps the landform (total relief falls only 279.0 -> 278.2 m). Past 2 m the curve
# flattens, because what remains is real landform plus the water cuts, and further blur
# only rounds the ridges off. Raise it if the terrain still reads too rough.
SMOOTH_SIGMA_M = 2.0

# --- Water depth ------------------------------------------------------------------
#
# The render fills water with dark paint, and dark means low — so water bodies come out
# as cavities scaled by how dark they were DRAWN, not by how deep they are. Measured
# raw: lake beds reach -25 m, and the swamp (a FLAT plateau at ~59 m) is punched through
# by ponds down to -29 m — an 88 m pit with 72-86 degree walls.
#
# This cannot be fixed by compressing the low END of the height range. The render paints
# a pond sitting at 59 m exactly as dark as the lake sitting at 0 m, so luminance alone
# cannot tell them apart, and any global curve would still leave a 59 m pit. The dip has
# to be compressed against LOCAL land level, which needs the biome map's water mask —
# the one input that knows where water is regardless of how dark it was drawn.
#
# So the render's darkness is discarded inside water entirely, and the bed is rebuilt as
#
#     bed = local_land_level - cap * (1 - exp(-d / L))
#
# where `d` is the distance to the nearest shore. Depth growing with distance from shore
# is also how real water bodies behave, and it means depth -> 0 AT the shoreline, so the
# bank grades in instead of dropping off a wall. One formula separates rivers from lakes
# with no classification: a river is only ~3 m from its own bank, a lake is hundreds.
#
#   river  (d ~ 3 m)   -> 16 * (1 - exp(-3/14))   ~= 3.1 m
#   lake   (d >> 42 m) -> 16 * (1 - exp(-inf))    ~= 16 m
#   swamp pond         -> capped at 2 m, tightened by SWAMP_FALLOFF so small ponds still
#                         reach their (shallow) cap rather than being ankle-deep
WATER_DEPTH_M = 8.0     # lake bed cap below local land (in scale with the 32 m hills)
WATER_FALLOFF_M = 14.0  # how fast depth grows from the shore
SWAMP_DEPTH_M = 2.0     # swamp ponds: a wetland, not a quarry
SWAMP_FALLOFF_M = 3.0

# Biome legend colours, sampled from the render's own swatches. Index = Biome enum value
# (src/mapgen/biomes.hpp): Lake=0, Swamp=1, Forest=2, Plains=3, Hills=4, Mountain=5.
BIOME_COLORS = [
    ("lake", (19, 59, 108)),
    ("swamp", (55, 56, 35)),
    ("forest", (53, 84, 44)),
    ("plains", (165, 204, 127)),
    ("hills", (209, 182, 145)),
    ("mountain", (129, 128, 129)),
]

OUT_DIR = "assets/map"


def gaussian(a, sigma):
    """Separable Gaussian blur. Hand-rolled because scipy isn't a dependency here and
    PIL's GaussianBlur refuses mode 'F' — and this has to run in float."""
    r = int(3 * sigma)
    x = np.arange(-r, r + 1, dtype=np.float64)
    k = np.exp(-(x * x) / (2 * sigma * sigma))
    k /= k.sum()
    b = np.apply_along_axis(lambda m: np.convolve(m, k, "same"), 1,
                            np.pad(a, ((0, 0), (r, r)), "edge"))[:, r:-r]
    return np.apply_along_axis(lambda m: np.convolve(m, k, "same"), 0,
                               np.pad(b, ((r, r), (0, 0)), "edge"))[r:-r, :]


def slope_stats(h):
    gy, gx = np.gradient(h)  # 1 m samples
    s = np.degrees(np.arctan(np.hypot(gx, gy)))
    return np.percentile(s, 50), np.percentile(s, 90), 100.0 * (s > 45).mean()


def shore_distance(mask, maxd):
    """Distance (in samples/meters) from each True pixel to the nearest False one, by
    repeated 4-connected erosion. Truncated at `maxd` because depth saturates long
    before then — no point computing 350 m of distance to cap at 16 m."""
    d = np.zeros(mask.shape, np.float32)
    cur = mask.copy()
    for i in range(1, maxd + 1):
        e = cur.copy()
        e[1:, :] &= cur[:-1, :]
        e[:-1, :] &= cur[1:, :]
        e[:, 1:] &= cur[:, :-1]
        e[:, :-1] &= cur[:, 1:]
        # Off-map counts as land, so water at the border still grades in.
        e[0, :] = e[-1, :] = False
        e[:, 0] = e[:, -1] = False
        if not e.any():
            break
        d[e] = i
        cur = e
    return d


def shore_level(h, water, factor=4):
    """The level of the water SURFACE at each water pixel: the land height propagated
    inward from the shoreline.

    Deliberately a grassfire fill from the shore, not a blur or an image-pyramid
    inpaint. Both of those average over a neighbourhood, and at the scale of a 700 m
    lake the neighbourhood is dominated by distant forest and mountains rather than by
    the shore — which produced a lake sitting at +62 m, i.e. a mesa instead of a basin.
    A lake's level is set by its shoreline and nothing else.

    Runs on a `factor`-downsampled grid: the surface is smooth and near-flat, so coarse
    is plenty, and it turns ~350 full-resolution iterations into ~90 small ones. Thin
    rivers vanish at this scale, which is not a loss — their cell is then all bank, so
    they read the bank level directly, which is exactly the level a river sits at.
    """
    H, W = h.shape
    hh, ww = H // factor, W // factor
    landm = (~water).astype(np.float64)
    hs = (h * landm)[: hh * factor, : ww * factor].reshape(hh, factor, ww, factor).sum((1, 3))
    ws = landm[: hh * factor, : ww * factor].reshape(hh, factor, ww, factor).sum((1, 3))
    known = ws > 0
    lvl = np.zeros((hh, ww))
    lvl[known] = hs[known] / ws[known]

    def shift(a, dy, dx):
        p = np.pad(a, 1, "edge")
        return p[1 + dy : 1 + dy + hh, 1 + dx : 1 + dx + ww]

    cur, kn = lvl.copy(), known.copy()
    for _ in range(4 * max(hh, ww)):  # bounded; breaks as soon as it is full
        if kn.all():
            break
        s = np.zeros_like(cur)
        c = np.zeros_like(cur)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            s += shift(cur * kn, dy, dx)
            c += shift(kn.astype(np.float64), dy, dx)
        newly = (~kn) & (c > 0)
        if not newly.any():
            break
        cur[newly] = s[newly] / c[newly]
        kn |= newly

    up = np.asarray(
        Image.fromarray(cur.astype(np.float32), mode="F").resize((W, H), Image.BILINEAR)
    ).astype(np.float64)
    # The grassfire fill fans out in plateaus, and the coarse grid + bilinear upsample
    # turns their edges into block facets in the bed. The surface is near-flat, so a
    # generous blur (several coarse cells wide) dissolves the facets without meaningfully
    # moving the level.
    return gaussian(up, 4.0 * factor)


def dist_from_shoreline(water, band):
    """Distance (meters) from the water/land edge, on BOTH sides, capped at `band`.
    0 on the edge pixels, growing to `band`; anything past the band reads > band."""
    edge = np.zeros(water.shape, bool)
    diff_v = water[1:, :] != water[:-1, :]
    edge[1:, :] |= diff_v
    edge[:-1, :] |= diff_v
    diff_h = water[:, 1:] != water[:, :-1]
    edge[:, 1:] |= diff_h
    edge[:, :-1] |= diff_h

    d = np.full(water.shape, band + 1.0)
    d[edge] = 0.0
    cur = edge.copy()
    for i in range(1, band + 1):
        nb = np.zeros_like(cur)
        nb[1:, :] |= cur[:-1, :]
        nb[:-1, :] |= cur[1:, :]
        nb[:, 1:] |= cur[:, :-1]
        nb[:, :-1] |= cur[:, 1:]
        nb &= d > band  # only pixels not yet reached
        if not nb.any():
            break
        d[nb] = i
        cur = nb
    return d


def smooth_shoreline(h, water, band_m=32, sigma=14.0):
    """Grade the coast across a band straddling the waterline.

    The render draws a bright rim-light at the water's edge (measured: source land
    pixels rise ~3 levels right at the shore), which maps to a raised lip of "land"
    at the coast — and the de-emboss blur mixing dark water into the near-shore land
    digs a small trough just inland of it. Both are decoration, not elevation. Not
    filter ringing: bilinear/bicubic/lanczos all reproduce it identically, because it
    is in the source.

    So blend the heights toward a blurred copy within a band around the shoreline,
    feathered by distance so the effect is strongest AT the edge and vanishes by
    `band_m` inland — it grades the coast without touching terrain away from water.
    """
    d = dist_from_shoreline(water, band_m)
    w = np.clip(1.0 - d / band_m, 0.0, 1.0)  # 1 on the shoreline -> 0 at band edge
    w = w * w * (3 - 2 * w)  # smoothstep, so the band blends in without a seam
    return h * (1.0 - w) + gaussian(h, sigma) * w


def tiered_remap(alt, labels, names):
    """Global monotonic height curve: remap the continuous de-embossed heights so each
    biome tier reaches its BIOME_TIERS ceiling, with eased highland segments.

    A single increasing function of height, so it is continuous everywhere and preserves
    the elevation order -- no biome-edge cliffs. The tier ceilings are anchored at each
    tier's ~p90 input height (measured, not hard-coded), and each segment rises as
    t**ease from the tier below's ceiling, so highlands start flat (apron) and steepen to
    decorative peaks. Returns heights in meters; water pixels get the lowland band as a
    placeholder and are overwritten by rebuild_water_beds.
    """
    def group(tier):
        if tier == "lowland":
            return ["plains", "swamp"]
        return [tier]

    # Anchor input heights: p2 of the lowland is the floor (-> 0 m); each tier's p90 is
    # where it reaches its ceiling. Force strictly increasing so the LUT is monotone even
    # if two tiers overlap in height.
    # Each tier's ceiling is anchored at its own input percentile (see BIOME_TIERS): the
    # ~(100-pct)% above it climbs into the next tier's segment. The tail is real terrain
    # (tall hills becoming low mountains), not spikes.
    lowland_mask = np.isin(labels, [names.index(b) for b in ("plains", "swamp")])
    in_anchors = [float(np.percentile(alt[lowland_mask], 2))]
    out_anchors = [0.0]
    ease = [1.0]
    for name, ceil_m, e, pct in BIOME_TIERS:
        m = np.isin(labels, [names.index(b) for b in group(name)])
        pin = float(np.percentile(alt[m], pct))
        in_anchors.append(max(pin, in_anchors[-1] + 1e-3))
        out_anchors.append(ceil_m)
        ease.append(e)

    def shape(t, e):
        # Linear below the floor blended with a power above -- nonzero start slope, so no
        # flat terrace at the tier boundary (see EASE_MIX).
        return t if e == 1.0 else EASE_MIX * t + (1.0 - EASE_MIX) * t ** e

    out = np.empty_like(alt, np.float64)
    # Below the floor / above the top peak: clamp to the ends. The top anchor is ~p99.8,
    # so the high clamp touches only ~0.2% -- the true summit, not a mesa.
    out[alt <= in_anchors[0]] = out_anchors[0]
    out[alt >= in_anchors[-1]] = out_anchors[-1]
    for i in range(len(in_anchors) - 1):
        a_in, b_in = in_anchors[i], in_anchors[i + 1]
        a_out, b_out = out_anchors[i], out_anchors[i + 1]
        seg = (alt > a_in) & (alt < b_in)
        t = (alt[seg] - a_in) / (b_in - a_in)
        out[seg] = a_out + (b_out - a_out) * shape(t, ease[i + 1])

    # Swamp is a wetland: drop its floor just below the plains datum so it reads as
    # low ground rather than dry flat. (A small biome-specific offset on top of the
    # global curve; it does not affect ordering since swamp is already the lowest tier.)
    swamp = labels == names.index("swamp")
    out[swamp] += SWAMP_FLOOR_M
    print(f"tiers:     anchors(in->out) " +
          ", ".join(f"{a:.0f}->{o:.0f}" for a, o in zip(in_anchors, out_anchors)))
    return out


def rebuild_water_beds(h, labels, names):
    """Replace the painted-in water cavities with beds graded from each shore.

    Returns (h, report). See the WATER_DEPTH_M block for why this is local rather than a
    global compression of the low range."""
    water = labels == names.index("lake")
    if not water.any():
        return h, "no water"

    # Ponds inside the swamp are shallow wetland, not lake. A pond is surrounded by
    # swamp; the main lake is not — so context, not size, separates them.
    swamp_ctx = gaussian((labels == names.index("swamp")).astype(np.float64), 12.0) > 0.3
    in_swamp = water & swamp_ctx

    land = shore_level(h, water)  # the water surface, propagated in from the shore
    d = shore_distance(water, maxd=64)
    cap = np.where(in_swamp, SWAMP_DEPTH_M, WATER_DEPTH_M)
    fall = np.where(in_swamp, SWAMP_FALLOFF_M, WATER_FALLOFF_M)
    depth = cap * (1.0 - np.exp(-d / fall))

    out = np.where(water, land - depth, h)

    # The swamp's own pits are NOT lake-labelled, so the pass above misses them: the
    # render's dark pond speckles do not line up pixel-for-pixel with the biome map's
    # pond polygons, which leaves swamp-labelled holes ~55 m below a plateau that is
    # otherwise flat to within 1.6 m (p25..p75 = 58.0..59.6). Floor them against the
    # local swamp surface. A wide blur is a fair estimate of that surface precisely
    # because the pits are a small fraction of the area, so they barely drag it down.
    swamp = labels == names.index("swamp")
    if swamp.any():
        level = gaussian(out, 16.0)
        floored = np.maximum(out, level - SWAMP_DEPTH_M)
        out = np.where(swamp, floored, out)

    # Rivers are just water close to a bank; report them separately so the 2-4 m target
    # is checkable rather than assumed.
    river = water & (d <= 4)
    dpt = depth[water]
    rep = f"{100*water.mean():.1f}% of map | "
    if river.any():
        rep += f"river(d<=4m) depth p50={np.median(depth[river]):.1f} p95={np.percentile(depth[river],95):.1f} | "
    rep += f"all-water p50={np.median(dpt):.1f} max={dpt.max():.1f}"
    if in_swamp.any():
        rep += f" | swamp ponds max {depth[in_swamp].max():.1f}"
    return out, rep


def crop(path, mode):
    im = Image.open(path).convert(mode)
    box = (CROP_X, CROP_Y, CROP_X + CROP_SIZE, CROP_Y + CROP_SIZE)
    if box[2] > im.width or box[3] > im.height:
        sys.exit(f"crop {box} exceeds {path} ({im.width}x{im.height})")
    return np.asarray(im.crop(box)).astype(np.float32)


def main():
    for p in (ALT_SRC, BIOME_SRC):
        if not os.path.exists(p):
            sys.exit(f"missing source render: {p}")
    os.makedirs(OUT_DIR, exist_ok=True)

    alt = crop(ALT_SRC, "L")  # (1024,1024) f32, 0..255
    bio = crop(BIOME_SRC, "RGB")  # (1024,1024,3) f32

    # --- biome: classify to enum FIRST, then upsample the labels with NEAREST --------
    names = [n for n, _ in BIOME_COLORS]
    cols = np.array([c for _, c in BIOME_COLORS], np.float32)
    d2 = ((bio[:, :, None, :] - cols[None, None, :, :]) ** 2).sum(3)
    labels = d2.argmin(2).astype(np.uint8)
    fit = np.sqrt(d2.min(2))
    print(f"biome palette fit: median {np.median(fit):.1f}, frac>60 {100*(fit>60).mean():.2f}%")

    labels_up = np.asarray(
        Image.fromarray(labels, mode="L").resize((OUT_SIZE, OUT_SIZE), Image.NEAREST)
    )
    assert labels_up.max() < len(BIOME_COLORS), "biome label out of enum range"

    # --- heights: FLOAT resample (see note 1), Lanczos (note 2), then clamp ----------
    alt_up = np.asarray(
        Image.fromarray(alt, mode="F").resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)
    ).astype(np.float32)
    rang = (float(alt_up.min()), float(alt_up.max()))
    alt_up = np.clip(alt_up, 0.0, 255.0)  # undo Lanczos ringing; source is 0..255
    print(f"heights: lanczos range {rang[0]:.2f}..{rang[1]:.2f} -> clamped 0..255")
    print(f"         distinct levels {len(np.unique(alt_up)):d} (256 would mean 8-bit resample!)")

    # --- de-emboss: strip the baked-in hillshade (see SMOOTH_SIGMA_M) ----------------
    # Clamp first, then blur: blurring the ringing spikes would smear them outward
    # instead of removing them.
    if SMOOTH_SIGMA_M > 0:
        alt_up = gaussian(alt_up, SMOOTH_SIGMA_M)

    # --- biome-tiered height curve: set the vertical scale per biome (see BIOME_TIERS) -
    h = tiered_remap(alt_up, labels_up, names)

    # Water beds are REBUILT, not merely rescaled -- the render's darkness carries no
    # depth information (see the WATER_DEPTH_M block). Runs on the remapped land, so the
    # lake surface picks up the ~0-4 m lowland level from its shore.
    h, water_report = rebuild_water_beds(h, labels_up, names)
    print(f"water:     {water_report}")

    # Grade the coast: dissolve the render's shoreline rim-light (see smooth_shoreline).
    h = smooth_shoreline(h, labels_up == names.index("lake"))

    # No global shift: tiered_remap already fixes the datum -- 0 m is the lowland/plains
    # floor, water digs below it (lake bed to ~-8, swamp to -0.5). height_min_m is simply
    # negative. The only sub-datum land is river-bleed at label borders; clamp those few
    # up to the swamp floor so nothing non-water sits beneath the water table.
    land = ~(labels_up == names.index("lake"))
    h[land] = np.maximum(h[land], SWAMP_FLOOR_M)
    print(f"datum:     0 m = lowland floor; min {h.min():.1f} m (water), max {h.max():.1f} m")

    hmin, hmax = float(h.min()), float(h.max())
    print(f"           heights {hmin:.1f}..{hmax:.1f} m")
    p50, p90, over45 = slope_stats(h)
    print(f"slope:     p50={p50:.1f} p90={p90:.1f} deg, >45deg={over45:.1f}%  "
          f"(sigma={SMOOTH_SIGMA_M} m; unsmoothed this is p50=47 / 55%)")

    # --- encode -----------------------------------------------------------------------
    # 16-bit affine, matching map_meta's encoding string exactly.
    q = np.rint((h - hmin) / (hmax - hmin) * 65535.0).astype(np.uint16)
    Image.fromarray(q).save(f"{OUT_DIR}/heights_{OUT_SIZE}.png")  # uint16 -> I;16 gray
    Image.fromarray(labels_up).save(f"{OUT_DIR}/biome_{OUT_SIZE}.png")

    meta = {
        "width": OUT_SIZE,
        "height": OUT_SIZE,
        "meters_per_sample": 1.0,
        "height_min_m": round(hmin, 4),
        "height_max_m": round(hmax, 4),
        "water_level_m": 0.0,
        "encoding": "h_m = height_min_m + v/65535*(height_max_m-height_min_m)",
        "biomes": names,
        "source_alt": os.path.basename(ALT_SRC),
        "source_biome": os.path.basename(BIOME_SRC),
        "source_crop": {"x": CROP_X, "y": CROP_Y, "size": CROP_SIZE},
        "script": "scripts/mapgen/map_from_render.py",
    }
    with open(f"{OUT_DIR}/map_meta.json", "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    print()
    for i, n in enumerate(names):
        m = labels_up == i
        if m.any():
            print(f"  {i} {n:9s} {100*m.mean():5.1f}%  h {h[m].min():7.1f}..{h[m].max():6.1f} m")
    print(f"\nwrote {OUT_DIR}/heights_{OUT_SIZE}.png, biome_{OUT_SIZE}.png, map_meta.json")


if __name__ == "__main__":
    main()
