#!/usr/bin/env bash
# scripts/build.sh [target ...] — build (configuring build/ on first run);
# print compile/link errors only + one status line. Exit code = build result.
cd "$(dirname "$0")/.." || exit 1
if [ ! -d build ]; then
  cmake -S . -B build -G Ninja > /tmp/badlands_cfg.log 2>&1 \
    || { tail -20 /tmp/badlands_cfg.log; exit 1; }
fi
args=""
for t in "$@"; do args="$args --target $t"; done
log=$(mktemp)
if cmake --build build $args > "$log" 2>&1; then
  echo "BUILD OK (${*:-all})"
  rm -f "$log"
else
  grep -E "error:|Undefined symbol|FAILED" -A3 "$log" | head -40
  echo "BUILD FAILED (${*:-all}) — full log: $log"
  exit 1
fi
