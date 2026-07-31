#!/usr/bin/env python3
"""Per-lake geometry from a priority-flood fill: area, depths, volume."""
import sys
import numpy as np
from lakes import priority_flood, label

hp, n, world, relief = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
cell = world / n
ca = cell * cell
h = np.fromfile(hp, dtype=np.float32).reshape(n, n).astype(np.float64) * relief
depth = np.maximum(priority_flood(h) - h, 0.0)

lab, cnt = label(depth > 0.01)
rows = []
for k in range(1, cnt + 1):
    m = lab == k
    a = m.sum() * ca
    d = depth[m]
    rows.append((a, d.max(), d.mean(), d.sum() * ca))
rows.sort(key=lambda r: -r[0])

print(f"{hp}  cell {cell:.0f} m  map {world/1000:.0f}x{world/1000:.0f} km")
print(f"{cnt} depressions total")
big = [r for r in rows if r[0] >= 1e4]           # >= 1 hectare
print(f"{len(big)} at >= 1 ha; they hold "
      f"{sum(r[3] for r in big)/1e9:.3f} km3 of the "
      f"{sum(r[3] for r in rows)/1e9:.3f} km3 total\n")
print(f"{'rank':>4} {'area km2':>9} {'max depth m':>12} {'mean depth m':>13} {'vol Mm3':>10}")
for i, r in enumerate(rows[:12], 1):
    print(f"{i:>4} {r[0]/1e6:9.3f} {r[1]:12.1f} {r[2]:13.1f} {r[3]/1e6:10.1f}")

if big:
    md = np.array([r[1] for r in big])
    mn = np.array([r[2] for r in big])
    print(f"\nover the {len(big)} lakes >= 1 ha:")
    print(f"  max depth  : median {np.median(md):6.1f} m  p90 {np.percentile(md,90):6.1f} m")
    print(f"  mean depth : median {np.median(mn):6.1f} m  p90 {np.percentile(mn,90):6.1f} m")
