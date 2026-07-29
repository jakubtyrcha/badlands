#!/usr/bin/env bash
# scripts/noiser_guard.sh [BASE] — assert third_party/noiser is neither staged
# nor (given BASE) present in BASE..HEAD. OK/FAIL line; exit code matches.
cd "$(dirname "$0")/.." || exit 1
staged=$(git status --porcelain | grep -i noiser | grep -v '^ M' || true)
if [ -n "$staged" ]; then
  echo "FAIL: noiser staged/modified beyond ' M':"; echo "$staged"; exit 1
fi
if [ -n "${1:-}" ]; then
  if git diff --name-only "$1"..HEAD | grep -qi noiser; then
    echo "FAIL: noiser appears in $1..HEAD"; exit 1
  fi
fi
echo "OK: noiser untouched"
