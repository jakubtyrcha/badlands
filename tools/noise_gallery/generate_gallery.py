#!/usr/bin/env python3
"""Generate a static HTML gallery of the noiser noise library.

Drives the (already-built) ``badlands_noise_texgen`` CLI to render one 128x128
grayscale/color thumbnail per parameter setting, then assembles a single
sticky-nav ``index.html`` that shows, per noise family, how its parameters
change the look.

Run from the repo root:

    cmake --build build --target badlands_noise_texgen   # once
    python3 tools/noise_gallery/generate_gallery.py

Output (committed; PNGs are git-LFS):

    docs/noise_gallery/index.html       # open in a browser
    docs/noise_gallery/tiles/*.png      # 128x128 thumbnails
    docs/noise_gallery/scripts/*.noiser # regenerable intermediates (gitignored)

No C++ changes: each tile is a tiny baked-literal .noiser script that imports
the ``texgen`` helper lib (domain_pos/unorm/clamp01/grad_rgb) and, for warp
tiles, the noiser corelib. The thumbnail spec is fixed: window [-2,2]x[-2,2],
signed noise mapped [-1,1] -> [0,1] grayscale.
"""

from __future__ import annotations

import concurrent.futures
import html
import os
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageOps  # optional: per-tile contrast normalization
    _HAVE_PIL = True
except ImportError:  # pragma: no cover
    _HAVE_PIL = False

# --- Paths (relative to repo root: tools/noise_gallery/ -> repo root) --------
REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "build" / "badlands_noise_texgen"
TEX_INCLUDE = REPO / "assets" / "noiser" / "texgen"
CORE_INCLUDE = REPO / "third_party" / "noiser" / "noiser-corelib"

OUT = REPO / "docs" / "noise_gallery"
TILES = OUT / "tiles"
SCRIPTS = OUT / "scripts"  # regenerable intermediates (gitignored)

WINDOW = "-2,-2,2,2"   # domain rectangle mapped across every tile
RES = "128x128"        # thumbnail resolution
TILE_PX = 128

# --- Tile registry -----------------------------------------------------------
# name -> (script_body, mode, normalize). Deduplicated so shared settings
# render once. `normalize` requests a per-tile contrast stretch (see below).
_tiles: dict[str, tuple[str, str, bool]] = {}


def nf(x: float) -> str:
    """Format a float as a noiser f32 literal (always with a decimal point)."""
    return repr(float(x))


def reg(name: str, body: str, mode: str, normalize: bool = False) -> str:
    """Register a tile; return its relative <img src> path."""
    entry = (body, mode, normalize)
    if name in _tiles and _tiles[name] != entry:
        raise ValueError(f"tile name collision with different body: {name}")
    _tiles[name] = entry
    return f"tiles/{name}.png"


# --- Script-body builders (all validated against noiserc) --------------------
def basis_body(intrinsic: str, freq: float, seed: int) -> str:
    return ("import { domain_pos, unorm } from texgen;\n"
            f"unorm({intrinsic}(domain_pos(), {nf(freq)}, {seed}))")


# Cellular scale per return type: a coarse pre-scale that keeps each distance
# combination inside [0,1] WITHOUT clipping (their magnitudes vary wildly by
# both metric and return type). A per-tile contrast stretch (normalize=True)
# then maps each tile to full range, so exact scales here only need to avoid
# saturation. CellValue is a per-cell hash in [-1,1] and uses unorm instead
# (kept faithful, not stretched).
# Non-negative distance returns: a coarse pre-scale (from measured freq=4
# ranges) that keeps each inside [0,1] without heavy clipping; the per-tile
# stretch then maps to full range. CellValue and Distance2Sub are SIGNED
# (per-cell hash / d1-d2 respectively) and use unorm instead so their negative
# half isn't clamped to black.
CELL_SCALE = {
    "Distance": 1.0,
    "Distance2": 0.8,
    "Distance2Add": 0.8,
    "Distance2Mul": 1.0,
    "Distance2Div": 0.8,
}
CELL_SIGNED = {"CellValue", "Distance2Sub"}


def cellular_body(freq: float, seed: int, dist: str, ret: str) -> str:
    call = (f"cellular2d(domain_pos(), {nf(freq)}, {seed}, "
            f"CellularDistance::{dist}, CellularReturn::{ret})")
    if ret in CELL_SIGNED:
        return ("import { domain_pos, unorm } from texgen;\n"
                f"unorm({call})")
    scale = CELL_SCALE.get(ret, 1.0)
    return ("import { domain_pos, clamp01 } from texgen;\n"
            f"clamp01({call} * {nf(scale)})")


def fbm_body(freq: float, seed: int, oct_: int, lac: float, gain: float) -> str:
    return ("import { domain_pos, unorm } from texgen;\n"
            f"unorm(fbm2d(domain_pos(), {nf(freq)}, {seed}, {oct_}, "
            f"{nf(lac)}, {nf(gain)}))")


def ridged_body(freq: float, seed: int, oct_: int, lac: float,
                gain: float) -> str:
    return ("import { domain_pos, clamp01 } from texgen;\n"
            f"clamp01(ridged2d(domain_pos(), {nf(freq)}, {seed}, {oct_}, "
            f"{nf(lac)}, {nf(gain)}))")


def pingpong_body(freq: float, seed: int, oct_: int, lac: float, gain: float,
                  strength: float) -> str:
    return ("import { domain_pos, unorm } from texgen;\n"
            f"unorm(pingpong2d(domain_pos(), {nf(freq)}, {seed}, {oct_}, "
            f"{nf(lac)}, {nf(gain)}, {nf(strength)}))")


def deriv_body(struct: str, method: str, freq: float, seed: int) -> str:
    """Gradient / curl colour viz (rgb). method is 'gradient' or 'curl'."""
    return ("import { domain_pos, grad_rgb } from texgen;\n"
            f"grad_rgb({struct} {{ seed: {seed}, freq: {nf(freq)} }}"
            f".{method}(domain_pos()))")


def warp_body(freq: float, seed: int, wseed: int, wfreq: float, oct_: int,
              strength: float) -> str:
    return ("import { domain_pos, unorm } from texgen;\n"
            "import { fbm_vec2_2d } from core::math::noise_vector;\n"
            f"unorm(perlin2d(domain_pos() + fbm_vec2_2d(domain_pos(), "
            f"{nf(wfreq)}, {wseed}, {oct_}, 2.0, 0.5) * {nf(strength)}, "
            f"{nf(freq)}, {seed}))")


# --- Family catalog ----------------------------------------------------------
BASIS = [  # (label, scalar intrinsic, gradient/curl struct)
    ("Perlin", "perlin2d", "Perlin2d"),
    ("Simplex", "simplex2d", "Simplex2d"),
    ("Value", "value2d", "ValueNoise2d"),
    ("ValueCubic", "value_cubic_2d", "ValueCubic2d"),
    ("OpenSimplex2", "opensimplex2_2d", "OpenSimplex2_2d"),
    ("OpenSimplex2S", "opensimplex2s_2d", "OpenSimplex2S2d"),
]
CELL_DISTANCES = ["Euclidean", "EuclideanSq", "Manhattan", "Hybrid"]
CELL_RETURNS = ["CellValue", "Distance", "Distance2", "Distance2Add",
                "Distance2Sub", "Distance2Mul", "Distance2Div"]
FRACTALS = [  # (label, body builder, has_strength)
    ("FBM", fbm_body, False),
    ("Ridged", ridged_body, False),
    ("PingPong", pingpong_body, True),
]

FREQS = [0.5, 1.0, 2.0, 4.0, 8.0]
SEEDS = [0, 1, 2, 3, 4]
OCTAVES = [1, 2, 3, 4, 6, 8]
GAINS = [0.3, 0.4, 0.5, 0.6, 0.7]
LACS = [1.5, 2.0, 2.5, 3.0, 4.0]
STRENGTHS = [1.0, 2.0, 3.0, 4.0]      # pingpong strength
WARP_STRENGTHS = [0.0, 0.1, 0.25, 0.5, 1.0]


def slug(*parts) -> str:
    s = "__".join(str(p) for p in parts)
    return (s.replace(" ", "").replace(".", "p").replace("-", "m")
            .replace("::", "_"))


# --- HTML building -----------------------------------------------------------
# Each block builder returns an HTML string and registers its tiles.
def tile_img(src: str, tooltip: str) -> str:
    t = html.escape(tooltip, quote=True)
    return (f'<img class="tile" width="{TILE_PX}" height="{TILE_PX}" '
            f'loading="lazy" src="{src}" alt="{t}" title="{t}">')


def matrix_block(title: str, subtitle: str, corner: str,
                 col_defs: list[tuple[str, object]],
                 row_defs: list[tuple[str, object]],
                 tile_fn, mode: str, normalize_fn=None) -> str:
    """A 2D grid: columns share a header, each row is prefixed by a header.

    col_defs / row_defs are (header_label, value). tile_fn(rval, cval) ->
    (tile_name, script_body, tooltip). normalize_fn(rval, cval) -> bool
    requests a per-tile contrast stretch for that cell (None = never).
    """
    ncol = len(col_defs)
    cells = [f'<div class="corner">{html.escape(corner)}</div>']
    for cl, _ in col_defs:
        cells.append(f'<div class="colhead">{html.escape(cl)}</div>')
    for rl, rv in row_defs:
        cells.append(f'<div class="rowhead">{html.escape(rl)}</div>')
        for _, cv in col_defs:
            name, body, tip = tile_fn(rv, cv)
            norm = bool(normalize_fn(rv, cv)) if normalize_fn else False
            src = reg(name, body, mode, norm)
            cells.append(tile_img(src, tip))
    grid = (f'<div class="grid" style="grid-template-columns:max-content '
            f'repeat({ncol},{TILE_PX}px)">' + "".join(cells) + "</div>")
    sub = f'<p class="sub">{html.escape(subtitle)}</p>' if subtitle else ""
    return f'<div class="block"><h3>{html.escape(title)}</h3>{sub}{grid}</div>'


def build_html() -> str:
    sections: list[tuple[str, str, str]] = []  # (id, nav_label, inner_html)

    # --- Basis noises --------------------------------------------------------
    blocks = []
    # comparison strip: all families at freq=2, seed=0
    blocks.append(matrix_block(
        "Family comparison", "freq = 2, seed = 0", "family",
        [(lab, (intr, struct)) for lab, intr, struct in BASIS],
        [("noise", None)],
        lambda rv, cv: (
            slug("basis_cmp", cv[0]),
            basis_body(cv[0], 2.0, 0),
            f"{cv[0]}(freq=2, seed=0)"),
        "grayscale"))
    # frequency sweep: rows = families, cols = freq
    blocks.append(matrix_block(
        "Frequency sweep", "seed = 0; frequency increases left -> right",
        "family \\ freq",
        [(f"f={f:g}", f) for f in FREQS],
        [(lab, intr) for lab, intr, _ in BASIS],
        lambda rv, cv: (
            slug("basis_freq", rv, cv),
            basis_body(rv, cv, 0),
            f"{rv}(freq={cv:g}, seed=0)"),
        "grayscale"))
    # seed variation: rows = families, cols = seed
    blocks.append(matrix_block(
        "Seed variation", "freq = 2; each column is a different seed",
        "family \\ seed",
        [(f"s={s}", s) for s in SEEDS],
        [(lab, intr) for lab, intr, _ in BASIS],
        lambda rv, cv: (
            slug("basis_seed", rv, cv),
            basis_body(rv, 2.0, cv),
            f"{rv}(freq=2, seed={cv})"),
        "grayscale"))
    sections.append(("basis", "Basis", "".join(blocks)))

    # --- Cellular ------------------------------------------------------------
    blocks = []
    blocks.append(matrix_block(
        "Distance x return matrix",
        "freq = 4, seed = 0; distance returns are per-tile contrast-stretched",
        "distance \\ return",
        [(r, r) for r in CELL_RETURNS],
        [(d, d) for d in CELL_DISTANCES],
        lambda rv, cv: (
            slug("cell", rv, cv),
            cellular_body(4.0, 0, rv, cv),
            f"cellular(dist={rv}, return={cv}, freq=4, seed=0)"),
        "grayscale", normalize_fn=lambda rv, cv: cv != "CellValue"))
    blocks.append(matrix_block(
        "Frequency sweep", "Euclidean / Distance, seed = 0", "return \\ freq",
        [(f"f={f:g}", f) for f in [1.0, 2.0, 4.0, 8.0, 16.0]],
        [("Distance", "Distance")],
        lambda rv, cv: (
            slug("cell_freq", cv),
            cellular_body(cv, 0, "Euclidean", "Distance"),
            f"cellular(Euclidean/Distance, freq={cv:g}, seed=0)"),
        "grayscale", normalize_fn=lambda rv, cv: True))
    blocks.append(matrix_block(
        "Seed variation", "Euclidean / CellValue, freq = 4", "return \\ seed",
        [(f"s={s}", s) for s in SEEDS],
        [("CellValue", "CellValue")],
        lambda rv, cv: (
            slug("cell_seed", cv),
            cellular_body(4.0, cv, "Euclidean", "CellValue"),
            f"cellular(Euclidean/CellValue, freq=4, seed={cv})"),
        "grayscale"))
    sections.append(("cellular", "Cellular", "".join(blocks)))

    # --- Fractals ------------------------------------------------------------
    blocks = []
    for label, body_fn, has_strength in FRACTALS:
        def make_octgain(bfn, lab):
            def f(rv, cv):
                # rv = octaves, cv = gain; freq=1, seed=0, lac=2
                if lab == "PingPong":
                    body = bfn(1.0, 0, rv, 2.0, cv, 2.0)
                    tip = (f"{lab}(oct={rv}, gain={cv:g}, lac=2, freq=1, "
                           f"strength=2)")
                else:
                    body = bfn(1.0, 0, rv, 2.0, cv)
                    tip = f"{lab}(oct={rv}, gain={cv:g}, lac=2, freq=1)"
                return (slug(lab, "octgain", rv, cv), body, tip)
            return f
        blocks.append(matrix_block(
            f"{label}: octaves x gain", "freq = 1, lacunarity = 2, seed = 0",
            "octaves \\ gain",
            [(f"g={g:g}", g) for g in GAINS],
            [(f"o={o}", o) for o in OCTAVES],
            make_octgain(body_fn, label), "grayscale"))

        def make_lac(bfn, lab):
            def f(rv, cv):
                # cv = lacunarity; oct=5, gain=0.5, freq=1, seed=0
                if lab == "PingPong":
                    body = bfn(1.0, 0, 5, cv, 0.5, 2.0)
                else:
                    body = bfn(1.0, 0, 5, cv, 0.5)
                return (slug(lab, "lac", cv), body,
                        f"{lab}(lacunarity={cv:g}, oct=5, gain=0.5, freq=1)")
            return f
        blocks.append(matrix_block(
            f"{label}: lacunarity sweep", "octaves = 5, gain = 0.5, freq = 1",
            "\\ lacunarity",
            [(f"l={l:g}", l) for l in LACS],
            [("noise", None)],
            make_lac(body_fn, label), "grayscale"))

        if has_strength:
            def make_strength(bfn, lab):
                def f(rv, cv):
                    body = bfn(1.0, 0, 4, 2.0, 0.5, cv)
                    return (slug(lab, "strength", cv), body,
                            f"{lab}(strength={cv:g}, oct=4, gain=0.5, freq=1)")
                return f
            blocks.append(matrix_block(
                f"{label}: strength sweep", "octaves = 4, gain = 0.5, freq = 1",
                "\\ strength",
                [(f"str={s:g}", s) for s in STRENGTHS],
                [("noise", None)],
                make_strength(body_fn, label), "grayscale"))
    sections.append(("fractals", "Fractals", "".join(blocks)))

    # --- Gradient & Curl (colour) -------------------------------------------
    blocks = []
    DERIV = BASIS + [("FBM", None, "FBM2d"), ("Ridged", None, "Ridged2d")]
    for method in ("gradient", "curl"):
        blocks.append(matrix_block(
            f"{method.capitalize()} field (colour)",
            "dN/dx -> red, dN/dy -> green; freq = 2, seed = 0", "family",
            [(lab, struct) for lab, _, struct in DERIV],
            [(method, None)],
            (lambda m: lambda rv, cv: (
                slug("deriv", m, cv),
                deriv_body(cv, m, 2.0, 0),
                f"{cv}.{m}(freq=2, seed=0)"))(method),
            "rgb"))
    sections.append(("gradient", "Gradient/Curl", "".join(blocks)))

    # --- Domain warp ---------------------------------------------------------
    blocks = []
    blocks.append(matrix_block(
        "Domain warp: strength sweep",
        "base Perlin(freq=2) warped by fbm_vec2 field; str = 0 is unwarped",
        "warp seed \\ strength",
        [(f"str={s:g}", s) for s in WARP_STRENGTHS],
        [(f"wseed={w}", w) for w in (7, 13)],
        lambda rv, cv: (
            slug("warp", rv, cv),
            warp_body(2.0, 0, rv, 1.0, 4, cv),
            f"Perlin(freq=2) warped by fbm_vec2(wseed={rv}) * {cv:g}"),
        "grayscale"))
    sections.append(("warp", "Warp", "".join(blocks)))

    # --- Assemble page -------------------------------------------------------
    nav = " ".join(f'<a href="#{sid}">{html.escape(lab)}</a>'
                   for sid, lab, _ in sections)
    body_parts = []
    for sid, lab, inner in sections:
        body_parts.append(
            f'<section id="{sid}"><h2>{html.escape(lab)}</h2>{inner}</section>')

    return _PAGE.format(nav=nav, tile_count=len(_tiles),
                        body="".join(body_parts))


_PAGE = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Noiser noise gallery</title>
<style>
  :root {{ --bg:#0d0f13; --panel:#151922; --fg:#e6e9ef; --mut:#8b93a3;
           --line:#262c38; --accent:#6ea8ff; }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--bg); color:var(--fg);
    font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }}
  header {{ position:sticky; top:0; z-index:10; background:rgba(13,15,19,.95);
    backdrop-filter:blur(6px); border-bottom:1px solid var(--line);
    padding:8px 16px; display:flex; gap:16px; align-items:baseline;
    flex-wrap:wrap; }}
  header b {{ font-size:14px; }}
  header .meta {{ color:var(--mut); }}
  nav a {{ color:var(--accent); text-decoration:none; margin-right:12px; }}
  nav a:hover {{ text-decoration:underline; }}
  section {{ padding:8px 16px 24px; border-bottom:1px solid var(--line); }}
  h2 {{ font-size:18px; margin:16px 0 4px; }}
  h3 {{ font-size:14px; margin:18px 0 2px; color:var(--fg); }}
  .sub {{ margin:0 0 8px; color:var(--mut); }}
  .block {{ margin-bottom:16px; }}
  .grid {{ display:grid; gap:6px; align-items:center; width:max-content;
    max-width:100%; overflow-x:auto; padding-bottom:4px; }}
  .tile {{ display:block; background:#000; border:1px solid var(--line);
    border-radius:2px; }}
  .colhead {{ color:var(--mut); text-align:center; font-size:12px;
    white-space:nowrap; }}
  .rowhead {{ color:var(--mut); text-align:right; padding-right:8px;
    white-space:nowrap; }}
  .corner {{ color:#5a6272; text-align:right; padding-right:8px;
    white-space:nowrap; font-size:11px; }}
</style></head>
<body>
<header>
  <b>Noiser noise gallery</b>
  <span class="meta">{tile_count} tiles &middot; 128px &middot; window
    [-2,2]&sup2; &middot; [-1,1]&rarr;[0,1]</span>
  <nav>{nav}</nav>
</header>
{body}
</body></html>
"""


# --- Rendering ---------------------------------------------------------------
def render_tile(item: tuple[str, tuple[str, str, bool]]
                ) -> tuple[str, str | None]:
    name, (body, mode, normalize) = item
    script = SCRIPTS / f"{name}.noiser"
    png = TILES / f"{name}.png"
    script.write_text(body + "\n")
    cmd = [str(TOOL), str(script),
           "--include", str(TEX_INCLUDE),
           "--include", str(CORE_INCLUDE),
           "--window", WINDOW, "--res", RES, "--mode", mode,
           "--out", str(png)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return name, "timeout"
    if r.returncode != 0:
        return name, (r.stderr or r.stdout or "unknown error").strip()
    # Per-tile contrast stretch (cellular distance returns only): their raw
    # magnitude varies by orders of magnitude across metric/return, so a fixed
    # [-1,1]->[0,1] map leaves most tiles near-black. autocontrast maps each
    # tile's own range to full 0..255 so its structure is legible.
    if normalize and _HAVE_PIL:
        im = ImageOps.autocontrast(Image.open(png).convert("L"), cutoff=0.5)
        im.convert("RGB").save(png)
    return name, None


def main() -> int:
    if not TOOL.exists():
        print(f"error: {TOOL} not found; build it first:\n"
              f"  cmake --build build --target badlands_noise_texgen",
              file=sys.stderr)
        return 1
    TILES.mkdir(parents=True, exist_ok=True)
    SCRIPTS.mkdir(parents=True, exist_ok=True)

    page = build_html()  # populates _tiles as a side effect
    if not _HAVE_PIL and any(n for _, _, n in _tiles.values()):
        print("note: Pillow not installed; cellular distance tiles will not be "
              "contrast-stretched (pip install Pillow for clearer tiles).",
              file=sys.stderr)
    print(f"rendering {len(_tiles)} tiles with "
          f"{os.cpu_count()} workers ...", flush=True)

    failures: list[tuple[str, str]] = []
    done = 0
    total = len(_tiles)
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=os.cpu_count() or 4) as ex:
        for name, err in ex.map(render_tile, list(_tiles.items())):
            done += 1
            if err:
                failures.append((name, err))
            if done % 25 == 0 or done == total:
                print(f"  {done}/{total}", flush=True)

    index = OUT / "index.html"
    index.write_text(page)

    if failures:
        print(f"\n{len(failures)} tile(s) FAILED:", file=sys.stderr)
        for name, err in failures[:10]:
            print(f"  {name}: {err.splitlines()[0] if err else ''}",
                  file=sys.stderr)
    print(f"\nwrote {index}")
    print(f"open it with:  open {index}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
