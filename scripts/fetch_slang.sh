#!/usr/bin/env bash
# scripts/fetch_slang.sh — fetch the pinned Slang SDK into third_party/toolchains/slang.
#
# PINNED, for the same reason Dawn is (cmake/FetchDawn.cmake): the recipe this
# replaces resolved `releases/latest` at run time, so two machines cloning a week
# apart got different compilers and neither could tell. The probe results in
# docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md were measured
# against this exact version.
#
# The SDK is gitignored (third_party/toolchains/), so this is how a fresh clone
# gets one. Idempotent: re-running with the pin already installed does nothing.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

VERSION="2026.14.1"
ASSET="slang-${VERSION}-macos-aarch64.tar.gz"
URL="https://github.com/shader-slang/slang/releases/download/v${VERSION}/${ASSET}"
# Verified, not decorative: this is a prebuilt binary fetched over the network
# and then linked into every shader the engine compiles.
SHA256="92da7ab6226dd951037cd85397f830ae78fe40fbbb8928882e0b2654e468fdd4"

DEST="third_party/toolchains/slang"
STAMP="${DEST}/.version"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$VERSION" ] && [ -f "${DEST}/include/slang.h" ]; then
  echo "slang ${VERSION} already installed at ${DEST}"
  exit 0
fi

if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
  echo "fetch_slang.sh: only the macOS arm64 asset is pinned; this host is $(uname -s)/$(uname -m)." >&2
  echo "Add the matching asset and its checksum above before running here." >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "fetching slang ${VERSION}..."
curl -fsSL -o "${tmp}/${ASSET}" "$URL"

actual="$(shasum -a 256 "${tmp}/${ASSET}" | cut -d' ' -f1)"
if [ "$actual" != "$SHA256" ]; then
  echo "fetch_slang.sh: checksum mismatch for ${ASSET}" >&2
  echo "  expected ${SHA256}" >&2
  echo "  got      ${actual}" >&2
  exit 1
fi

# Replace rather than extract over the top, so a version bump cannot leave a
# stale file from the previous SDK behind for CMake to find.
rm -rf "$DEST"
mkdir -p "$DEST"
tar xzf "${tmp}/${ASSET}" -C "$DEST"

if [ ! -f "${DEST}/include/slang.h" ] || [ ! -f "${DEST}/lib/libslang.dylib" ]; then
  echo "fetch_slang.sh: extracted tree is missing include/slang.h or lib/libslang.dylib." >&2
  echo "  The asset layout changed; update DEST handling above." >&2
  exit 1
fi

echo "$VERSION" > "$STAMP"
echo "slang ${VERSION} -> ${DEST}"
