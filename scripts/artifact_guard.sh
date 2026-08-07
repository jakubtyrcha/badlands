#!/usr/bin/env bash
# scripts/artifact_guard.sh — assert no app's DEFAULT output file is tracked by
# git at the repo root.
#
# Those paths are what a run without --out writes, and every ctest entry runs
# with WORKING_DIRECTORY = the source dir. So a tracked file there is one the
# test suite silently overwrites -- and because *.png is git-LFS, `git add -A`
# after a test run then stages a machine-specific render as if it were an asset.
# That happened once, with object_viewer.png.
set -euo pipefail
cd "$(dirname "$0")/.."

tracked=$(git ls-files -- object_viewer.png rhi_lab.png)
if [ -n "$tracked" ]; then
  echo "FAIL: these are apps' default output paths and must not be tracked:" >&2
  echo "$tracked" >&2
  echo "Remove with: git rm --cached <path>" >&2
  exit 1
fi
echo "artifact guard OK"
