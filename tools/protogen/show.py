#!/usr/bin/env python3
"""Render protogen dumps. Height and water arrive in METRES; Q in m^3/s.

    python3 tools/protogen/show.py <dump-dir> [resolution world_m]

Resolution and world size come from <dump-dir>/world.txt (written by
src/mapgen/coarse_io.hpp) unless given explicitly -- which a window.cpp
output still needs, since that tool writes a patch-style map.txt, not
world.txt.

ADDITIVE (Task 7): a phase-1 tag also carries `depth` (raw h, metres) and
`vel` (speed magnitude, m/s) rasters, rendered into their own PNGs when
present. A phase-0 tag has neither, and an old dump has no `discharge`
alongside a phase-1 tag either (phase 1 never updates it) -- every one of
these is checked with `os.path.exists` and simply skipped when absent, so
old dumps and phase-0 tags render exactly as before.
"""
import sys, os, glob
import numpy as np
from PIL import Image


def read_world_txt(d):
    """Reads resolution/world_size_m out of <d>/world.txt. Unknown keys are
    ignored, matching coarse_io.cpp's own forward-compatibility rule."""
    res, world = None, None
    with open(os.path.join(d, "world.txt")) as f:
        for line in f:
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "resolution":
                res = int(parts[1])
            elif parts[0] == "world_size_m":
                world = float(parts[1])
    if res is None or world is None:
        raise SystemExit(f"{d}/world.txt: missing resolution/world_size_m")
    return res, world


def load(p, n):
    return np.fromfile(p, dtype=np.float32).reshape(n, n).astype(np.float64)


def hillshade(z, cell, az=315.0, alt=45.0):
    gy, gx = np.gradient(z, cell)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    a, z0 = np.radians(az), np.radians(alt)
    return np.clip(np.sin(z0) * np.cos(slope) +
                   np.cos(z0) * np.sin(slope) * np.cos(a - aspect), 0, 1)


def main():
    d = sys.argv[1]
    if len(sys.argv) >= 4:
        n, world = int(sys.argv[2]), float(sys.argv[3])
    else:
        n, world = read_world_txt(d)
    cell = world / n
    for hp in sorted(glob.glob(os.path.join(d, "*-height.f32"))):
        tag = os.path.basename(hp)[: -len("-height.f32")]
        h = load(hp, n)
        sh = hillshade(h, cell)
        rgb = np.dstack([sh * 190, sh * 195, sh * 185])

        wat = load(hp.replace("-height", "-water"), n)
        lake = wat > 0.01
        land = ~lake

        # Rivers from the erf-squashed discharge, thresholded outside lakes.
        # `discharge` is a phase-0 particle-EMA field: a phase-1 snapshot
        # (Task 7's `%04d-cycle` tags) carries none, so this degrades to "no
        # river overlay" there instead of crashing on a missing file.
        dis_path = hp.replace("-height", "-discharge")
        if os.path.exists(dis_path):
            dis = load(dis_path, n)
            thr = float(np.quantile(dis[land], 0.985)) if land.any() else 0.0
            riv = np.clip((dis - thr) / max(dis.max() - thr, 1e-9), 0, 1) ** 0.5
            riv[lake] = 0.0
        else:
            riv = np.zeros((n, n))

        t = np.clip(wat / 8.0, 0, 1)[..., None]
        water = np.array([72, 132, 172]) * (1 - t) + np.array([16, 40, 70]) * t
        rgb[lake] = water[lake]
        rv = np.array([48, 108, 168])
        rgb = rgb * (1 - riv[..., None]) + rv * riv[..., None]

        Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(
            os.path.join(d, tag + "-map.png"))
        Image.fromarray((sh * 255).astype(np.uint8)).save(
            os.path.join(d, tag + "-hillshade.png"))

        # ADDITIVE (Task 7): depth/vel, only when the phase-1 rasters exist.
        extra = ""
        depth_path = hp.replace("-height", "-depth")
        if os.path.exists(depth_path):
            depth = load(depth_path, n)
            # Log-scaled blue ramp, 0.01-10 m: a linear scale washes out the
            # cm-deep channel film next to a metres-deep lake, and this run's
            # whole point is showing both on one map. Dry ground (below the
            # 1 cm floor) gets its own flat tan, not the ramp's low end, so
            # "shallow but wet" stays visually distinct from "dry".
            lo, hi = 0.01, 10.0
            dry = depth <= lo
            tt = np.clip((np.log10(np.maximum(depth, lo)) - np.log10(lo)) /
                         (np.log10(hi) - np.log10(lo)), 0, 1)[..., None]
            rgb_d = (np.array([225, 235, 245]) * (1 - tt) +
                     np.array([10, 40, 100]) * tt)
            rgb_d[dry] = [225, 216, 196]
            Image.fromarray(np.clip(rgb_d, 0, 255).astype(np.uint8)).save(
                os.path.join(d, tag + "-depth.png"))
            extra += f"  depth max {depth.max():6.2f} m"

        vel_path = hp.replace("-height", "-vel")
        if os.path.exists(vel_path):
            vel = load(vel_path, n)
            vmax = max(float(np.quantile(vel, 0.99)), 1e-6)
            tt = np.clip(vel / vmax, 0, 1)[..., None]
            rgb_v = (np.array([20, 20, 40]) * (1 - tt) +
                     np.array([255, 205, 40]) * tt)
            Image.fromarray(np.clip(rgb_v, 0, 255).astype(np.uint8)).save(
                os.path.join(d, tag + "-vel.png"))
            extra += f"  vel p99 {vmax:5.2f} m/s"

        print(f"{tag}: relief {h.max()-h.min():7.1f} m  "
              f"wet {100*lake.mean():5.2f}%  max depth {wat.max():6.1f} m  "
              f"river px {int((riv>0.1).sum())}{extra}")


if __name__ == "__main__":
    main()
