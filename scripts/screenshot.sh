#!/usr/bin/env bash
# scripts/screenshot.sh <app> <out.png> [args ...] — SIGALRM-bounded headless
# screenshot, e.g. scripts/screenshot.sh badlands_viewer /tmp/t.png --generator 1 --lod 3
cd "$(dirname "$0")/.." || exit 1
app="$1"; out="$2"; shift 2
rm -f "$out"
perl -e 'alarm 60; exec @ARGV' "./build/$app" --screenshot "$out" "$@" \
  > /tmp/badlands_shot.log 2>&1
code=$?
if [ "$code" -eq 0 ] && [ -s "$out" ]; then
  echo "SCREENSHOT OK: $out ($(stat -f%z "$out") bytes)"
else
  tail -20 /tmp/badlands_shot.log
  echo "SCREENSHOT FAILED (exit $code): $out"
  exit 1
fi
