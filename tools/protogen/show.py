#!/usr/bin/env python3
"""Render protogen dumps. Height and water arrive in METRES; Q in m^3/s."""
import sys, os, glob
import numpy as np
from PIL import Image


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
    d, n, world = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    cell = world / n
    for hp in sorted(glob.glob(os.path.join(d, "*-height.f32"))):
        tag = os.path.basename(hp)[: -len("-height.f32")]
        h = load(hp, n)
        sh = hillshade(h, cell)
        rgb = np.dstack([sh * 190, sh * 195, sh * 185])

        # Rivers from the erf-squashed discharge, thresholded outside lakes.
        dis = load(hp.replace("-height", "-discharge"), n)
        wat = load(hp.replace("-height", "-water"), n)
        lake = wat > 0.01
        land = ~lake
        thr = float(np.quantile(dis[land], 0.985)) if land.any() else 0.0
        riv = np.clip((dis - thr) / max(dis.max() - thr, 1e-9), 0, 1) ** 0.5
        riv[lake] = 0.0

        t = np.clip(wat / 8.0, 0, 1)[..., None]
        water = np.array([72, 132, 172]) * (1 - t) + np.array([16, 40, 70]) * t
        rgb[lake] = water[lake]
        rv = np.array([48, 108, 168])
        rgb = rgb * (1 - riv[..., None]) + rv * riv[..., None]

        Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(
            os.path.join(d, tag + "-map.png"))
        Image.fromarray((sh * 255).astype(np.uint8)).save(
            os.path.join(d, tag + "-hillshade.png"))
        print(f"{tag}: relief {h.max()-h.min():7.1f} m  "
              f"wet {100*lake.mean():5.2f}%  max depth {wat.max():6.1f} m  "
              f"river px {int((riv>0.1).sum())}")


if __name__ == "__main__":
    main()
