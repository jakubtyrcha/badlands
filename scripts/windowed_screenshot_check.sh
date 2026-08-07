#!/usr/bin/env bash
# scripts/windowed_screenshot_check.sh — the layer's --screenshot path, end to end.
#
# THE EXIT CODE IS NOT ENOUGH ON ITS OWN, which is the point: the run used to
# exit 0 having written no file at all when its single capped frame's Acquire was
# skipped, and on an HDR display it wrote a PNG of raw half-float bit patterns
# that was a valid file of the right dimensions. So this asserts the process
# succeeded AND that a plausible PNG landed on disk.
set -euo pipefail
cd "$(dirname "$0")/.."

app=${1:?usage: windowed_screenshot_check.sh <app> [args...]}
shift
out="$(mktemp -t badlands_shot).png"
trap 'rm -f "$out"' EXIT

"./build/$app" --frames 4 --width 320 --height 240 --screenshot "$out" "$@"

if [ ! -s "$out" ]; then
  echo "FAIL: $app exited 0 but wrote no screenshot" >&2
  exit 1
fi

magic=$(head -c 4 "$out" | od -An -tx1 | tr -d ' \n')
if [ "$magic" != "89504e47" ]; then
  echo "FAIL: $out is not a PNG (magic $magic)" >&2
  exit 1
fi

# THE PNG'S OWN DIMENSIONS, not the ones asked for: --width/--height are POINTS
# and the surface is PIXELS, so on a HiDPI display the image is four times the
# area requested and a threshold built from the request rejects a good file.
# IHDR holds width then height as big-endian uint32 at offset 16. Read as bytes
# and reassemble, because `od --endian` is a GNU extension macOS does not have.
ihdr=$(od -An -tx1 -j16 -N8 "$out" | tr -d ' \n')
hex_at() { printf '%s' "${ihdr:$1:8}"; }
w=$(( 16#$(hex_at 0) ))
h=$(( 16#$(hex_at 8) ))
raw=$(( w * h * 4 ))
size=$(wc -c < "$out")
if [ "$raw" -le 0 ]; then
  echo "FAIL: could not read the PNG's dimensions from $out" >&2
  exit 1
fi
# Half the raw size. Real frames here land near 5%; the half-float-as-bytes bug
# produced a file LARGER than raw, because noise does not compress.
if [ "$size" -ge $(( raw / 2 )) ]; then
  echo "FAIL: $out is $size bytes for ${w}x${h} (raw $raw) -- barely compressed," >&2
  echo "      which is what an unconverted float surface produces" >&2
  exit 1
fi

echo "windowed screenshot OK (${w}x${h}, $size bytes)"
