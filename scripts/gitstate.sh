#!/usr/bin/env bash
# scripts/gitstate.sh — one-shot repo state: branch, HEAD, short status,
# commits ahead of origin/main.
cd "$(dirname "$0")/.." || exit 1
echo "branch: $(git branch --show-current)"
echo "head:   $(git log --oneline -1)"
git status --short
ahead=$(git log --oneline origin/main..HEAD 2>/dev/null | wc -l | tr -d ' ')
if [ "$ahead" != "0" ]; then
  echo "ahead of origin/main ($ahead):"
  git log --oneline origin/main..HEAD
fi
