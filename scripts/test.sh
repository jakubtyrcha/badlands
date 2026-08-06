#!/usr/bin/env bash
# scripts/test.sh                       — full ctest suite, compact summary
# scripts/test.sh <regex>               — ctest -R <regex>, failures + totals only
# scripts/test.sh <binary> "<filter>"   — ./build/<binary> "<filter>" (Catch2), filtered
cd "$(dirname "$0")/.." || exit 1
if [ $# -ge 2 ]; then
  ./build/"$1" "$2" 2>&1 | grep -E \
    "FAILED|with expansion|test cases:|assertions:|All tests passed" | tail -20
  exit "${PIPESTATUS[0]}"
elif [ $# -eq 1 ]; then
  ctest --test-dir build -R "$1" -LE display --output-on-failure --no-tests=error 2>&1 | grep -E \
    "FAILED|with expansion|test cases:|assertions:|tests passed|Errors while running|No tests were found" | tail -30
  exit "${PIPESTATUS[0]}"
else
  # -LE display: tests that need a real display stay out of the default run.
  # They flake over SSH or with the screen locked, for reasons that say
  # nothing about the code. Run them with: ctest --test-dir build -L display
  ctest --test-dir build -LE display 2>&1 | tail -6
  exit "${PIPESTATUS[0]}"
fi
