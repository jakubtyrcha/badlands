#!/usr/bin/env python3
"""Option A: priority-flood depression filling as a POST-PASS on a finished
heightmap. Barnes et al., "Priority-Flood: An Optimal Depression-Filling and
Watershed-Labeling Algorithm". O(n log n), deterministic, no sim changes.

water_depth = filled - height; lakes are the connected components of depth > 0,
each flat at its own spill elevation.

usage: lakes.py <height.f32> <n> <world_m> <relief_m> <out.png>
"""
import sys, heapq
import numpy as np
from PIL import Image


def priority_flood(h):
    ny, nx = h.shape
    filled = h.copy()
    closed = np.zeros(h.shape, dtype=bool)
    pq = []
    # Seed with the whole border: water leaves the map there, so those cells
    # are already at their final elevation and everything drains toward them.
    for x in range(nx):
        for y in (0, ny - 1):
            if not closed[y, x]:
                closed[y, x] = True
                heapq.heappush(pq, (float(h[y, x]), y, x))
    for y in range(ny):
        for x in (0, nx - 1):
            if not closed[y, x]:
                closed[y, x] = True
                heapq.heappush(pq, (float(h[y, x]), y, x))

    while pq:
        e, y, x = heapq.heappop(pq)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            j, i = y + dy, x + dx
            if j < 0 or i < 0 or j >= ny or i >= nx or closed[j, i]:
                continue
            closed[j, i] = True
            # Rising from the outlet inward: a cell can never sit below the
            # lowest path out, so raising it to `e` IS its spill level.
            v = h[j, i] if h[j, i] > e else e
            filled[j, i] = v
            heapq.heappush(pq, (float(v), j, i))
    return filled


def label(mask):
    """4-connected components, iterative flood fill (no scipy dependency)."""
    ny, nx = mask.shape
    lab = np.zeros(mask.shape, dtype=np.int32)
    cur = 0
    for sy in range(ny):
        for sx in range(nx):
            if not mask[sy, sx] or lab[sy, sx]:
                continue
            cur += 1
            stack = [(sy, sx)]
            lab[sy, sx] = cur
            while stack:
                y, x = stack.pop()
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    j, i = y + dy, x + dx
                    if 0 <= j < ny and 0 <= i < nx and mask[j, i] and not lab[j, i]:
                        lab[j, i] = cur
                        stack.append((j, i))
    return lab, cur


def hillshade(z, cell, az=315.0, alt=45.0):
    gy, gx = np.gradient(z, cell)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    a, z0 = np.radians(az), np.radians(alt)
    return np.clip(
        np.sin(z0) * np.cos(slope) + np.cos(z0) * np.sin(slope) * np.cos(a - aspect),
        0, 1)


def main():
    hp, n, world, relief, out = (sys.argv[1], int(sys.argv[2]),
                                 float(sys.argv[3]), float(sys.argv[4]),
                                 sys.argv[5])
    cell = world / n
    cell_area = cell * cell
    h = np.fromfile(hp, dtype=np.float32).reshape(n, n).astype(np.float64) * relief

    filled = priority_flood(h)
    depth = np.maximum(filled - h, 0.0)

    print(f"--- {hp}  ({n}x{n}, cell {cell:.1f} m, relief {relief:.0f} m)")
    for thr in (0.01, 0.5, 2.0):
        wet = depth > thr
        print(f"  depth > {thr:>4} m : {100*wet.mean():5.2f}% of map, "
              f"{wet.sum()*cell_area/1e6:8.2f} km2")
    print(f"  max depth {depth.max():.1f} m   mean over wet "
          f"{depth[depth>0.01].mean() if (depth>0.01).any() else 0:.2f} m")

    # Lakes worth the name: >= 0.5 m deep somewhere and >= 1 hectare.
    lab, cnt = label(depth > 0.01)
    keep, areas, depths = 0, [], []
    for k in range(1, cnt + 1):
        m = lab == k
        a = m.sum() * cell_area
        d = depth[m].max()
        if d >= 0.5 and a >= 1e4:
            keep += 1
            areas.append(a)
            depths.append(d)
    print(f"  {cnt} depressions; {keep} are >=0.5 m deep and >=1 ha")
    if keep:
        areas = np.array(areas) / 1e6
        print(f"  those: area km2 median {np.median(areas):.3f} "
              f"max {areas.max():.3f}; max depth median "
              f"{np.median(depths):.1f} m max {max(depths):.1f} m")

    sh = hillshade(h, cell)
    rgb = np.dstack([sh * 190, sh * 195, sh * 185])
    wet = depth > 0.01
    # Deeper water reads darker/bluer, the same Beer-Lambert intuition as the
    # renderer: shallow shows the bed, deep does not.
    t = np.clip(depth / 8.0, 0, 1)[..., None]
    water = np.array([70, 130, 170]) * (1 - t) + np.array([18, 42, 72]) * t
    rgb[wet] = water[wet]
    Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).save(out)
    print(f"  -> {out}")


if __name__ == "__main__":
    main()
