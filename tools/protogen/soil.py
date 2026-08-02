#!/usr/bin/env python3
"""Render the substrate: erodible cover over bedrock, in METRES.

show.py draws the surface; this draws what the surface is MADE OF, which is what
the biome classification is cut from.

    python3 tools/protogen/soil.py <dump-dir> [resolution world_m]

Resolution and world size come from <dump-dir>/world.txt (src/mapgen/coarse_io.hpp)
unless given explicitly.

Writes <tag>-soil.png per snapshot: bare rock reads as grey-brown, deepening to
green as cover thickens, with standing water overlaid in blue. The colour scale
is LOGARITHMIC because the distribution spans four decades -- half the map sits
under 0.5 m while valley fill reaches hundreds of metres, and a linear ramp shows
nothing but the basins.
"""
import sys
import os
import glob

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
    for sp in sorted(glob.glob(os.path.join(d, "*-soil.f32"))):
        tag = os.path.basename(sp)[: -len("-soil.f32")]
        soil = load(sp, n)
        h = load(sp.replace("-soil", "-height"), n)
        wat = load(sp.replace("-soil", "-water"), n)

        # log10 over [1 cm, 100 m]; below the floor is bare rock.
        t = np.clip((np.log10(np.maximum(soil, 1e-2)) + 2.0) / 4.0, 0, 1)[..., None]
        rock = np.array([124, 112, 104])   # exposed bedrock
        thick = np.array([96, 148, 72])    # deep alluvium
        rgb = rock * (1 - t) + thick * t

        # Relight so the landform stays readable under the flat substrate colour.
        sh = hillshade(h, cell)[..., None]
        rgb = rgb * (0.55 + 0.55 * sh)

        lake = wat > 0.01
        rgb[lake] = np.array([60, 120, 165])

        Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(
            os.path.join(d, tag + "-soil.png"))

        dry = soil[~lake]
        bare = float((dry < 0.01).mean()) if dry.size else 0.0
        print(f"{tag}: soil m min/median/max = "
              f"{dry.min():6.2f}/{np.median(dry):7.2f}/{dry.max():8.2f}  "
              f"bare rock {100*bare:5.1f}%")


if __name__ == "__main__":
    main()
